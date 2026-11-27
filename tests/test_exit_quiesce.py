"""Host model of the exit quiesce (renderer/src/vgx_play.cpp: HandOver's flag check and VgxQuiesceForExit): the real HandOver and VgxQuiesceForExit text is compiled on the host with stand-ins for
the slot and the semaphores, next to a back end thread that follows the loop's protocol (busy before pending clears, busy clear before the empty post). It checks that no frame is handed over
after the flag, that the wait covers a back end that is drawing and is bounded for one that is stuck, and that a refused restart (VgxQuiesceCancel) gives the pictures back. Mutants must fail."""
import pathlib, re, shutil, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = (ROOT / "renderer/src/vgx_play.cpp").read_text()
CC = shutil.which("g++")

DRIVE = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
namespace vgx::rec {
struct CmdBuf { int n = 0; void clear() { n = 0; } };
inline void swap(CmdBuf& a, CmdBuf& b) { std::swap(a.n, b.n); }
class Sema { public: explicit Sema(int c, const char*) : c_(c) {} void Wait() { std::unique_lock<std::mutex> l(m_); cv_.wait(l, [&] { return c_ > 0; }); --c_; }
  void Post() { { std::lock_guard<std::mutex> l(m_); ++c_; } cv_.notify_one(); } private: std::mutex m_; std::condition_variable cv_; int c_; };
bool AsyncLog() { return false; }
bool SemaSync() { return true; }
bool g_slotFull = false; uint32_t g_slotFlags = 0, g_curFlags = 0; CmdBuf g_slot;
std::mutex g_pipeMtx; std::condition_variable g_pipeCv;
std::atomic<uint64_t> g_slotWaitNs{0}, g_beIdleNs{0}, g_dbgPlayer{0}, g_dbgHanded{0};
bool g_async = true;
void WaitPipe(std::unique_lock<std::mutex>&, bool) {}
%(state)s
Sema& g_semEmpty = *new Sema(1, "e");
Sema& g_semFull = *new Sema(0, "f");
%(hand)s
}
extern "C" void VgxHostQuiesceGpu(unsigned) {}
%(quiesce)s
using namespace vgx::rec;
static std::atomic<int> drawn{0}, drawMs{0}, takeDelay{0};
static std::atomic<bool> stuck{false}, stop{false};
static void backend() {   /* the protocol of the loop in vgx_play.cpp */
    for (;;) {
        g_semFull.Wait();
        if (stop) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(takeDelay));   /* the gap between the wake-up and the busy flag */
        g_beBusy.store(1); g_pending.store(0);
        if (stuck) { for (int i = 0; i < 2000 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        else std::this_thread::sleep_for(std::chrono::milliseconds(drawMs));
        ++drawn;
        g_slot.clear();
        g_beBusy.store(0);
        g_semEmpty.Post();
    }
}
int main() {
    std::thread be(backend);
    CmdBuf buf;
    drawMs = 40;
    /* 1. the back end is drawing picture 1 and the player is blocked handing over picture 2: the quiesce waits for 1 and 2 is never drawn */
    std::thread player([&] { CmdBuf b; b.n = 1; HandOver(b); b.n = 1; HandOver(b); b.n = 1; HandOver(b); });
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    auto t0 = std::chrono::steady_clock::now();
    int ok = VgxQuiesceForExit(500);
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) return 10;
    if (g_pending.load() || g_beBusy.load()) return 11;
    player.join();
    int after = drawn;
    if (after != 1) return 12;           /* picture 1 was finished, 2 and 3 were never handed over */
    /* 2. no frame goes to the back end after the flag */
    buf.n = 1; HandOver(buf);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (drawn != after) return 13;
    if (buf.n != 0) return 14;           /* the dropped buffer is cleared, not left full */
    /* 3. the cancel gives the pictures back */
    VgxQuiesceCancel();
    buf.n = 1; HandOver(buf);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (drawn != after + 1) return 15;
    /* 3b. the back end has been woken but has not set busy yet: pending covers the gap */
    takeDelay = 40;
    buf.n = 1; HandOver(buf);
    ok = VgxQuiesceForExit(500);
    if (!ok || drawn != after + 2) return 18;
    VgxQuiesceCancel();
    takeDelay = 0;
    /* 4. a stuck back end: the wait is bounded */
    stuck = true;
    buf.n = 1; HandOver(buf);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    t0 = std::chrono::steady_clock::now();
    ok = VgxQuiesceForExit(100);
    ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ok) return 16;
    if (ms < 90 || ms > 400) return 17;
    stop = true; g_semFull.Post();
    be.join();
    return 0;
}
'''


def pieces():
    state = SRC[SRC.index("std::atomic<bool> g_quiesce{false};"):SRC.index("\n", SRC.index("std::atomic<int> g_inHand"))]
    h0 = SRC.index("struct HandGuard")
    h1 = SRC.index("\n}\n", SRC.index("void HandOver(CmdBuf& buf) {")) + 3
    q0 = SRC.index("extern \"C\" void VgxHostQuiesceGpu(unsigned ms) __attribute__((weak));")
    q1 = SRC.index("\n}\n", SRC.index("extern \"C\" int VgxQuiesceForExit(unsigned ms) {")) + 3
    quiesce = SRC[q0:q1].replace('extern "C" void VgxHostQuiesceGpu(unsigned ms) __attribute__((weak));   // renderer/vita/vgx_host_vita.cpp', "")
    return state, SRC[h0:h1], quiesce


def build(hand=None, quiesce=None):
    state, h, q = pieces()
    h = hand if hand is not None else h
    q = quiesce if quiesce is not None else q
    src = DRIVE % {"state": state, "hand": h, "quiesce": q}
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d, "t.cpp")
        f.write_text(src)
        r = subprocess.run([CC, "-std=gnu++17", "-O1", "-pthread", "-w", "-o", str(pathlib.Path(d, "t")), str(f)], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr[:2000])
        return subprocess.run([str(pathlib.Path(d, "t"))], capture_output=True).returncode


class ExitQuiesce(unittest.TestCase):
    def test_model(self):
        self.assertEqual(build(), 0)

    def test_mutants_are_caught(self):
        state, h, q = pieces()
        muts = [
            ("hand: no flag check at all", h.replace("if (g_quiesce.load(std::memory_order_acquire)) { buf.clear(); return; }", "", 1).replace("if (g_quiesce.load(std::memory_order_acquire)) { g_semEmpty.Post(); buf.clear(); return; }", "", 1), q),
            ("hand: no check after the wait", h.replace("if (g_quiesce.load(std::memory_order_acquire)) { g_semEmpty.Post(); buf.clear(); return; }", "", 1), q),
            ("quiesce: busy ignored", h, q.replace("g_beBusy.load(std::memory_order_acquire) == 0", "true")),
            ("quiesce: pending ignored", h, q.replace("g_pending.load(std::memory_order_acquire) == 0 &&", "")),
            ("quiesce: not bounded", h, q.replace("&& spent() < ms) std::this_thread", ") std::this_thread")),
            ("quiesce: flag never raised", h, q.replace("g_quiesce.store(true, std::memory_order_release);", "")),
        ]
        for name, hh, qq in muts:
            self.assertTrue(hh != h or qq != q, name)
            try:
                rc = build(hand=hh, quiesce=qq)
            except RuntimeError:
                rc = 99
            self.assertNotEqual(rc, 0, name)

    def test_real_loop_follows_the_protocol(self):
        i = SRC.index("g_beBusy.store(1, std::memory_order_release);")
        j = SRC.index("g_pending.store(0, std::memory_order_release);", i)
        k = SRC.index("g_beBusy.store(0, std::memory_order_release);", j)
        m = SRC.index("g_semEmpty.Post();", k)
        self.assertTrue(i < j < k < m)
        self.assertIn("interp::Submit(g_slot);", SRC[j:k])

    def test_exit_paths_call_it(self):
        for rel in ("native/platform/harness/src/np_harness.cpp", "native/platform/vita/src/np_vita_platform.cpp", "native/platform/vita/src/np_vita_main.cpp",
                    "native/platform/vita/src/np_vita_settings.cpp", "native/platform/sdk/src/os_core.cpp"):
            self.assertIn("VgxQuiesceForExit(200)", (ROOT / rel).read_text(), rel)
        req = (ROOT / "native/platform/vita/vita_require.mk").read_text()
        self.assertIn("VgxQuiesceForExit", req)
        self.assertIn("VgxHostQuiesceGpu", req)


if __name__ == "__main__":
    unittest.main()
