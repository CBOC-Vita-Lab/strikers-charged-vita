"""Multiplayer layer, wave 9 (helper mpcore): host unit tests of the small header-only pieces, built and run here."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
N = ROOT / "native"


def build_run(test):
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "t"
        c = subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-I", str(N), str(N / "platform/sdk/tests" / test), "-o", str(exe)], capture_output=True, text=True)
        if c.returncode:
            return c
        return subprocess.run([str(exe)], capture_output=True, text=True)


@unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
class MpCoreNative(unittest.TestCase):
    def test_found_games(self):
        r = build_run("test_found_games.cpp")
        self.assertEqual(r.returncode, 0, (r.stdout or "") + (r.stderr or ""))
        self.assertIn("0 failed", r.stdout)

    def test_lobby_uses_the_found_games_rule_and_no_early_drop_when_full(self):
        text = (N / "src/Game/LANLobby.cpp").read_text()
        body = text[text.index("void LANLobby::ProcessFoundGame"):text.index("void LANLobby::SendGamePeerAdded")]
        self.assertIn("NpFound::Decide(", body)
        self.assertNotRegex(body, r"if \(mFoundGameCount >= 10\)\s*return;")
        self.assertIn("memcpy(slot->mAddress", body)

    def test_online_save_rule(self):
        r = build_run("test_online_save.cpp")
        self.assertEqual(r.returncode, 0, (r.stdout or "") + (r.stderr or ""))
        self.assertIn("0 failed", r.stdout)

    def test_online_exit_uses_the_rule_and_every_error_path_sets_the_latch(self):
        ns = (N / "src/Game/NetworkSession.cpp").read_text()
        body = ns[ns.index("void NetworkSession::ShutdownOnline"):ns.index("mFriendsMatchProcessingSuspended = 0;", ns.index("void NetworkSession::ShutdownOnline"))]
        self.assertIn("NpOnlineSave::Wanted(always, changed, error)", body)
        self.assertIn("MSC_ONLINE_SAVE_ALWAYS", body)
        self.assertIn("if (save)", body)
        self.assertNotRegex(body, r"\{\s*SaveLoad::StartSave\(true\);\s*\}")   # no unconditional save left
        for needle in ("mStatsError", "ErrorLatch()", "IsLost()"):
            self.assertIn(needle, body)
        self.assertIn("NpOnlineSave::Begin();", ns)
        lan = (N / "platform/sdk/net/np_lan.cpp").read_text()
        self.assertRegex(lan, r"void NpLanLinkLost\(const char\* why\) \{[\s\S]{0,400}?NpOnlineSave::NoteError\(\)")
        self.assertIn('!strncmp(format, "DESYNC", 6) || !strncmp(format, "connection lost", 15)) NpOnlineSave::NoteError', lan)
        https = (N / "platform/sdk/net/np_https.cpp").read_text()
        self.assertRegex(https, r"NP_HTTPS_E_CERT \|\| e == NP_HTTPS_E_CLOCK\) NpOnlineSave::NoteError")
        # LAN never saves
        lan_fn = ns[ns.index("void NetworkSession::ShutdownLAN"):]
        self.assertNotIn("StartSave", lan_fn[:lan_fn.index("\n}\n")])

    def test_idle_gate(self):
        r = build_run("test_idle_gate.cpp")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("0 failed", r.stdout)

    def test_keep_awake_ticks_in_a_session_only_and_the_mutants_are_caught(self):
        r = build_run("test_keep_awake.cpp")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("0 failed", r.stdout)
        for m in ("MUTANT_ALWAYS", "MUTANT_NEVER", "MUTANT_EVERY_CALL"):
            with tempfile.TemporaryDirectory() as d:
                exe = Path(d) / "t"
                c = subprocess.run(["c++", "-std=c++11", "-D" + m, "-I", str(N), str(N / "platform/sdk/tests/test_keep_awake.cpp"), "-o", str(exe)], capture_output=True, text=True)
                self.assertEqual(c.returncode, 0, c.stderr)
                self.assertNotEqual(subprocess.run([str(exe)], capture_output=True, text=True).returncode, 0, m + " was not caught")

    def test_the_vita_link_frame_ticks_through_the_keep_awake_gate_only(self):
        text = (N / "platform/sdk/net/vlink_vita.cpp").read_text()
        body = text[text.index('extern "C" void np_vita_lan_frame(void) {'):]
        self.assertRegex(body[:1500], r"g_keepAwake\.Step\(KeepAwakeOn\(\), onlineUp \|\| NpLanActive\(\) \|\| NpOnlineFlowActive\(\)")
        self.assertEqual(text.count("sceKernelPowerTick"), 1)
        self.assertIn("[mp] keep awake on", text)
        self.assertIn("[mp] keep awake off", text)

    def test_certificate_dates_are_a_clock_fault(self):
        r = build_run("test_cert_clock.cpp")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("0 failed", r.stdout)

    def test_the_vita_link_frame_uses_the_gate_before_the_power_callback_call(self):
        """np_vita_lan_frame runs every picture in single player: its idle branch must reach sceKernelCheckCallback only through the gate."""
        text = (N / "platform/sdk/net/vlink_vita.cpp").read_text()
        body = text[text.index('extern "C" void np_vita_lan_frame(void) {'):]
        idle = body[body.index("if (!wifiUp && !radioUp) {"):]
        idle = idle[:idle.index("return;")]
        self.assertRegex(idle, r"g_powerCb >= 0 && g_idleGate\.Due\(")
        self.assertNotRegex(idle, r"if \(g_powerCb >= 0\) \{\s*sceKernelCheckCallback")

    def test_a_date_only_certificate_fault_fails_as_a_clock_fault(self):
        text = (N / "platform/sdk/net/np_https.cpp").read_text()
        self.assertIn("np_https_cert_is_date_fault(s->res.cert_flags) ? NP_HTTPS_E_CLOCK : NP_HTTPS_E_CERT", text)


    def test_network_pools_are_not_made_at_boot(self):
        """Single player held 0.9 MB of the virtual arena for two pools nothing in this port uses (DWC_Init and SOInit ignore their allocators)."""
        sess = (N / "src/Game/NetworkSession.cpp").read_text()
        init = sess[sess.index("void NetworkSession::Initialize(bool first)"):]
        init = init[:init.index("mDWCInitialized = 0;")]
        self.assertNotIn("nlMalloc", init)
        self.assertIn("if (NpNetPoolsAtBoot())", init)
        self.assertRegex(sess, r"static void\* NetworkAlloc\([^)]*\)\s*\{\s*EnsureNetworkPool\(\);")
        sock = (N / "src/NL/plat/SocketNetwork.cpp").read_text()
        mem = sock[sock.index("void SocketNetworkInitializeMemory()"):]
        mem = mem[:mem.index("\n}\n")]
        self.assertIn("if (NpNetPoolsAtBoot())", mem)
        self.assertRegex(sock, r"void\* SocketAlloc\([^)]*\)\s*\{\s*InitializeSocketAllocatorImpl\(\);")
        # the allocators the port hands to the libraries are ignored by them: if that ever changes, the pools must come back at boot
        dwc = (N / "platform/sdk/dwc/np_dwc_login.cpp").read_text()
        self.assertIn("DWC_Init(DWC_AuthServer, const char*, unsigned long gameCode, DWCAllocEx, DWCFreeEx)", dwc)
        so = (N / "platform/sdk/net/so_net.cpp").read_text()
        self.assertIn("int SOInit(SOLibraryConfig*)", so)


if __name__ == "__main__":
    unittest.main()
