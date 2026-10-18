// Player: consumes the record stream (vgx_rec.h) and drives the front end.
// Runs on the render thread (MSC_VGX_PLAY=thread) or on the game thread.
#include "vgx_rec.h"
#include "vgx_ext.h"
#include "vgx_interp.h"
#include "vgx_present.h"
#include "vgx_guestmem.h"
#include "vgx_sync.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <pthread.h>

namespace vgx::rec {

void RecycleChunk(Chunk* c);
Chunk* FirstChunk();
void PlayerWaitForData();
void NotifyDecoded();
void NotifyExecuted();
void AsyncDump(const char* tag, uint32_t playerStage, uint32_t backStage, uint32_t execCmd, uint64_t handed, uint64_t drawn);

namespace {
Chunk* g_rc = nullptr; // chunk being read
Chunk* g_prev = nullptr; // the one before it: kept one chunk longer, a by-value array binding may still point into it
uint32_t g_rpos = 0;

// ---- back end on its own thread (MSC_VGX_BACKEND_THREAD=1)
// The player and the back end are both about a processor core's worth of work
// per frame on the target, so they can be given a core each: the thread that
// owns the graphics context only executes finished command buffers, and the
// player runs on a thread of its own and hands each frame over (one frame may
// wait while the previous one is still being drawn; then the player blocks).
// A command buffer is self-contained, so nothing else is shared; the two
// back-end flags of R_FLAGS travel with the frame they were set for.
bool g_async = false;
// MSC_VGX_ASYNCLOG=1 (VRS): where each thread is, and a watcher that prints the hand-shake counters when nothing moves for 3 s.
// player stage: 0 reading records, 1 waiting for data, 2 waiting for the slot (HandOver), 3 handed over; back end stage: 0 waiting for a frame, 1 drawing, 2 drew.
inline bool AsyncLog() { // (read at first use: env.txt is applied after the static initialisers)
    static const bool on = [] { const char* e = std::getenv("MSC_VGX_ASYNCLOG"); return e != nullptr && e[0] == '1'; }();
    return on;
}
std::atomic<uint32_t> g_dbgPlayer{0}, g_dbgBack{0};
std::atomic<uint64_t> g_dbgHanded{0}, g_dbgDrawn{0};
void* AsyncWatcher(void*) {
    uint64_t lastH = ~0ull, lastD = ~0ull;
    int still = 0;
    for (;;) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, nullptr);
        const uint64_t h = g_dbgHanded.load(), d = g_dbgDrawn.load();
        if (h == lastH && d == lastD) ++still; else still = 0;
        lastH = h; lastD = d;
        if (still == 3 || (still > 3 && still % 5 == 0 && still < 40)) AsyncDump("STILL", g_dbgPlayer.load(), g_dbgBack.load(), backend::DbgExecCmd(), h, d);
    }
    return nullptr;
}
// (Heap objects that are never destroyed: a thread may be waiting on them when the process exits, and destroying a condition variable
// that has a waiter blocks for ever in exit(). Found by the native library's test with MSC_VGX_BACKEND_THREAD=1.)
std::mutex& g_pipeMtx = *new std::mutex;
std::condition_variable& g_pipeCv = *new std::condition_variable;
CmdBuf g_slot;           // the frame waiting for / being executed by the back end
bool g_slotFull = false;
uint32_t g_slotFlags = 0, g_curFlags = 0;

// W5 PIPE: the hand-off by two semaphores instead of the mutex / condition variable above (MSC_VGX_SYNC=cv brings the old one back). One slot, one producer
// (the player), one consumer (the back end): `g_semEmpty` (1 at the start) is posted when the back end has finished with the slot, `g_semFull` when the player has
// put a frame into it. A post cannot be lost, so there is no timed wait and no 2 ms of delay when a signal used to be missed.
inline bool SemaSync() {
    static const bool on = [] { const char* e = std::getenv("MSC_VGX_SYNC"); return !(e != nullptr && e[0] == 'c'); }();
    return on;
}
Sema& g_semEmpty = *new Sema(1, "vgx slot empty");
Sema& g_semFull = *new Sema(0, "vgx slot full");

// Waits until g_slotFull == want, with a 2 ms timeout per round. A plain wait(lk, pred) lost a wake-up in Vita3K after about 40-110 frames of
// MSC_VGX_BACKEND_THREAD=1 (the player waited for an empty slot that was empty, the back end for a full one: MSC_VGX_ASYNCLOG, VRS 16 Oct):
// the two threads wait for each other within microseconds, which is the one place where a signal can arrive between the predicate check
// and the block. The timeout turns a lost signal into a delay of at most 2 ms; a signal that arrives still wakes at once.
std::atomic<uint64_t> g_slotWaitNs{0}, g_beIdleNs{0}; // MSC_VGX_PROF: the player's wait for the slot / the back end's wait for a frame
void WaitPipe(std::unique_lock<std::mutex>& lk, bool want) {
    if (g_slotFull == want) return;
    static const bool prof = std::getenv("MSC_VGX_PROF") != nullptr && std::getenv("MSC_VGX_PROF")[0] != '0';
    const auto t0 = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    while (g_slotFull != want) g_pipeCv.wait_for(lk, std::chrono::milliseconds(2));
    if (prof)
        (want ? g_beIdleNs : g_slotWaitNs).fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                                                    std::memory_order_relaxed);
}

void HandOver(CmdBuf& buf) {
    if (AsyncLog()) g_dbgPlayer.store(2);
    if (SemaSync()) {
        static const bool prof = std::getenv("MSC_VGX_PROF") != nullptr && std::getenv("MSC_VGX_PROF")[0] != '0';
        const auto t0 = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        g_semEmpty.Wait();
        if (prof) g_slotWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        std::swap(g_slot, buf); // buf is now the (cleared) buffer of two frames ago
        g_slotFlags = g_curFlags;
        g_semFull.Post();
    } else {
        std::unique_lock<std::mutex> lk(g_pipeMtx);
        WaitPipe(lk, false);
        std::swap(g_slot, buf);
        g_slotFlags = g_curFlags;
        g_slotFull = true;
        lk.unlock();
        g_pipeCv.notify_all();
    }
    if (AsyncLog()) {
        g_dbgPlayer.store(3);
        const uint64_t n = g_dbgHanded.fetch_add(1) + 1;
        if (n <= 120) std::fprintf(stderr, "[vgx-async] handed over %llu\n", static_cast<unsigned long long>(n));
    }
}

inline int16_t Lo16(uint32_t v) { return static_cast<int16_t>(v & 0xFFFF); }
inline int16_t Hi16(uint32_t v) { return static_cast<int16_t>(v >> 16); }

// Plays one record at `r`; returns its length in words.
__attribute__((always_inline)) inline uint32_t PlayOne(const uint32_t* r) { // (inlined into the loop of PlayPendingT: a call per record is 10 of its 30 instructions)
    const uint32_t op = r[0] & 0xFF, aux = r[0] >> 8;
    switch (op) {
    case R_NOP: return 1;
    case R_RESET: ResetState(); return 1;
    case R_BP: {
        const uint8_t reg = static_cast<uint8_t>(r[1] >> 24);
        const uint32_t nv = r[1] & 0x00FFFFFFu;
        // GXD_BP_FAST: a register written with the value it holds changes nothing (for the TEV colour registers too: the half of the colour bank
        // a register feeds was set by the last write to it, which was this value; nothing else writes it) - StoreBP would find the same, after a call.
        if (g.bp[reg] == nv && Gxd(GXD_BP_FAST)) return 2;
        StoreBP(reg, nv);
        return 2;
    }
    case R_XF: {
        const uint32_t a = r[1];
        // GXD_BP_FAST: a single XF register written with its value (WriteXFWord would not mark anything). The two matrix-index registers are also
        // kept in matIdxA / matIdxB, which must agree too.
        if (aux == 1 && a - 0x1000u < 0x58u && g.xfReg[a - 0x1000u] == r[2] && (a != 0x1018 || g.matIdxA == r[2]) && (a != 0x1019 || g.matIdxB == r[2]) &&
            Gxd(GXD_BP_FAST))
            return 3;
        if (Gxd(GXD_XFREC)) { // (the same storage and flags as the multi-word cases of WriteXF)
            if (aux == 12) {
                float* dst = nullptr;
                if (a < 0xF5u) dst = &g.xfPos[0][0] + a;
                else if (a >= 0x500u && a < 0x5F5u) dst = &g.xfPost[0][0] + (a - 0x500u);
                if (dst) {
                    if (UpdateWords12(dst, r + 2)) g.xfMtxDirty = true;
                    return 14;
                }
            } else if (aux == 9 && a >= 0x400u && a + 9u <= 0x460u) {
                if (UpdateWords9(&g.xfNrm[0][0] + (a - 0x400u), r + 2)) g.xfMtxDirty = true;
                return 11;
            }
        }
        WriteXF(a, aux, r + 2);
        return 2 + aux;
    }
    case R_VCD: SetVcd(static_cast<int>(aux & 0xFF), static_cast<int>((aux >> 8) & 0xFF)); return 1;
    case R_VAT:
        SetVat(static_cast<int>(aux & 15), static_cast<int>((aux >> 4) & 0xFF), static_cast<int>((aux >> 12) & 15),
               static_cast<int>((aux >> 16) & 15), static_cast<int>(r[1]));
        return 2;
    case R_ARRAY:
        g.arrays[aux & 0xFF] = ArrayRef{static_cast<const uint8_t*>(GetPtr(r + 3)), r[1], r[2], ((aux >> 8) & 1) != 0};
        return 5;
    case R_ARRAY_COPY:
        // The bytes stay valid until the chunk is recycled, i.e. at least until the next jump record; arrays are
        // re-sent by the HLE layer before every display list that uses them.
        g.arrays[aux & 0xFF] = ArrayRef{reinterpret_cast<const uint8_t*>(r + 3), r[1], r[2], ((aux >> 8) & 1) != 0};
        return 3 + WordsFor(r[1]);
    case R_VTXSTATE: {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(r + 1);
        for (int a = 0; a <= 20; ++a) SetVcd(a, b[a]);
        for (int a = 9; a <= 20; ++a) SetVat(static_cast<int>(aux & 7), a, b[21 + (a - 9) * 3], b[22 + (a - 9) * 3], b[23 + (a - 9) * 3]);
        return 16;
    }
    case R_TEX: {
        TexSlot& s = g.tex[aux & 7];
        std::memcpy(&s.obj, r + 2, sizeof(TexObj));
        s.valid = s.obj.magic == 0x76677854u;
        s.loadSerial = r[1];
        g.texDirty = true;
        g.texSlotDirty |= static_cast<uint8_t>(1u << (aux & 7));
        return 2 + WordsFor(sizeof(TexObj));
    }
    case R_TLUT: {
        TlutSlot& s = g.tlut[aux < 20 ? aux : 19];
        std::memcpy(&s.obj, r + 1, sizeof(TlutObj));
        s.valid = true;
        g.texDirty = true;
        g.texSlotDirty = 0xFF; // (any texmap may use this palette)
        return 1 + WordsFor(sizeof(TlutObj));
    }
    case R_INVAL_TEX: InvalidateAllTextures(); return 1;
    case R_STREAM_REF: CallList(static_cast<const uint8_t*>(GetPtr(r + 2)), r[1]); return 4;
    case R_STREAM_COPY: ProcessStream(reinterpret_cast<const uint8_t*>(r + 2), r[1]); return 2 + WordsFor(r[1]);
    case R_DRAW_RAW:
        DrawStream(static_cast<uint8_t>(aux & 0xFF), static_cast<int>((aux >> 8) & 7), reinterpret_cast<const uint8_t*>(r + 3), r[1], r[2]);
        return 3 + WordsFor(r[2]);
    case R_DRAW_DEC: {
        DrawDecoded(static_cast<uint8_t>(aux & 0xFF), r[1], reinterpret_cast<const uint8_t*>(r + 3), r[2]);
        return 3 + WordsFor(r[2] * MakeLayout(r[1]).stride);
    }
    case R_COPY_TEX:
        g.texCopyFmt = r[1];
        g.texCopyHalf = (aux & 2) != 0;
        g.texCopySrc = EfbRect{Lo16(r[2]), Hi16(r[2]), Lo16(r[3]), Hi16(r[3])};
        g.texCopyDstW = static_cast<uint16_t>(r[4] & 0xFFFF);
        g.texCopyDstH = static_cast<uint16_t>(r[4] >> 16);
        QueueCopyTex(GetPtr(r + 5), (aux & 1) != 0);
        return 7;
    case R_COPY_DISP:
        g.dispCopySrc = EfbRect{Lo16(r[1]), Hi16(r[1]), Lo16(r[2]), Hi16(r[2])};
        QueueCopyDisp((aux & 1) != 0);
        return 3;
    case R_DESTROY_COPY: DestroyCopyTexture(GetPtr(r + 1)); return 3;
    case R_SET_TARGET:
        g.efbW = static_cast<uint16_t>(r[1] & 0xFFFF);
        g.efbH = static_cast<uint16_t>(r[1] >> 16);
        QueueSetTarget();
        return 2;
    case R_FLAGS:
        if (g_async) {
            g_curFlags = aux & 3;
        } else {
            backend::RequestCapture((aux & 1) != 0);
            backend::SetSkipDraws((aux & 2) != 0);
            present::NoteFlags((aux & 1) != 0, (aux & 2) != 0);
        }
        return 1;
    case R_EXT: PlayExt(aux & 0xFFu, r + 1, aux >> 8); return 1 + (aux >> 8);
    case R_PRESENT:
        if (g_mode == MODE_THREAD) {
            FrontPresent(true, &NotifyDecoded);
            if (!g_async) NotifyExecuted(); // (else the back-end thread does, once it has drawn the frame)
        } else {
            FrontPresent(false, nullptr);
        }
        return 1;
    default:
        VGX_ERR("corrupt record stream: op %u\n", op);
        std::abort();
    }
}
} // namespace

// MSC_VGX_OPSTAT=<n> (diagnostic, default off; GXD): every n presented frames one line with the mean records per frame by kind and how many of them
// changed nothing (a register written with the value it holds, a matrix with the same words, an array / texture bound again as it was, vertex state
// resent unchanged): what a recorder-side filter could drop. Looked at before the record is played.
namespace {
struct OpStat {
    uint64_t n[R_OP_COUNT] = {}, red[R_OP_COUNT] = {}, words[R_OP_COUNT] = {};
    uint64_t xfN[5] = {}, xfRed[5] = {}, xfWords[5] = {}; // pos, nrm, post, light, reg
    uint64_t bpN[256] = {}, bpRed[256] = {};
    uint64_t draws = 0, drawVerts = 0, streamBytes = 0, frames = 0;
    uint64_t texSameEpoch = 0, epoch0 = 0, texNotSame = 0; // R_TEX of an object equal to the slot's, with no write notification since the slot's last R_TEX; write notifications in the interval
    uint64_t total = 0, totalWords = 0;
} g_os;
long g_osEvery = -1;
void OpStatFrame(uint64_t frame);
void OpStatLen(uint32_t op, uint32_t len) {
    if (op >= R_OP_COUNT) return;
    g_os.words[op] += len; g_os.totalWords += len;
}
void OpStatPre(const uint32_t* r) {
    const uint32_t op = r[0] & 0xFF, aux = r[0] >> 8;
    if (op >= R_OP_COUNT) return;
    ++g_os.n[op]; ++g_os.total;
    switch (op) {
    case R_BP: {
        const uint8_t reg = static_cast<uint8_t>(r[1] >> 24);
        ++g_os.bpN[reg];
        if (reg < 0xE0 || reg > 0xE7) {
            if (g.bp[reg] == (r[1] & 0xFFFFFFu)) { ++g_os.red[op]; ++g_os.bpRed[reg]; }
        } else if (g.bp[reg] == (r[1] & 0xFFFFFFu)) { ++g_os.red[op]; ++g_os.bpRed[reg]; }
        break;
    }
    case R_XF: {
        const uint32_t a = r[1], c = aux;
        const uint32_t* cur = nullptr;
        int cls = 4;
        if (a < 0x100 && a + c <= 0x100) { cls = 0; cur = reinterpret_cast<const uint32_t*>(&g.xfPos[0][0]) + a; }
        else if (a >= 0x400 && a + c <= 0x460) { cls = 1; cur = reinterpret_cast<const uint32_t*>(&g.xfNrm[0][0]) + (a - 0x400); }
        else if (a >= 0x500 && a + c <= 0x600) { cls = 2; cur = reinterpret_cast<const uint32_t*>(&g.xfPost[0][0]) + (a - 0x500); }
        else if (a >= 0x600 && a + c <= 0x680) { cls = 3; cur = &g.xfLight[0][0] + (a - 0x600); }
        else if (a >= 0x1000 && a + c <= 0x1058) { cls = 4; cur = &g.xfReg[a - 0x1000]; }
        ++g_os.xfN[cls]; g_os.xfWords[cls] += c;
        if (cur && !std::memcmp(cur, r + 2, c * 4)) { ++g_os.red[op]; ++g_os.xfRed[cls]; }
        break;
    }
    case R_VCD:
        if (g.vcd[aux & 0xFF] == ((aux >> 8) & 0xFF)) ++g_os.red[op];
        break;
    case R_VAT: {
        const VtxAttrFmt& f = g.vat[aux & 15][(aux >> 4) & 0xFF];
        if (f.cnt == ((aux >> 12) & 15) && f.type == ((aux >> 16) & 15) && f.frac == r[1]) ++g_os.red[op];
        break;
    }
    case R_VTXSTATE: {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(r + 1);
        bool same = true;
        for (int a = 0; a <= 20 && same; ++a) same = g.vcd[a] == b[a];
        for (int a = 9; a <= 20 && same; ++a) {
            const VtxAttrFmt& f = g.vat[aux & 7][a];
            same = f.cnt == b[21 + (a - 9) * 3] && f.type == b[22 + (a - 9) * 3] && f.frac == b[23 + (a - 9) * 3];
        }
        if (same) ++g_os.red[op];
        break;
    }
    case R_ARRAY: {
        const ArrayRef& a = g.arrays[aux & 0xFF];
        if (a.data == GetPtr(r + 3) && a.size == r[1] && a.stride == r[2] && a.le == (((aux >> 8) & 1) != 0)) ++g_os.red[op];
        break;
    }
    case R_TEX: {
        static uint64_t lastEpoch[8] = {};
        const uint64_t ep = HaveGuestRam() ? GuestWriteEpoch() : ~0ull;
        const bool sameObj = !std::memcmp(&g.tex[aux & 7].obj, r + 2, sizeof(TexObj));
        if (sameObj && g.tex[aux & 7].loadSerial == r[1]) ++g_os.red[op];
        if (sameObj && lastEpoch[aux & 7] == ep) ++g_os.texSameEpoch;
        else if (sameObj) ++g_os.texNotSame;
        lastEpoch[aux & 7] = ep;
        break;
    }
    case R_STREAM_REF: case R_STREAM_COPY: g_os.streamBytes += r[1]; break;
    case R_DRAW_RAW: ++g_os.draws; g_os.drawVerts += r[1]; break;
    case R_PRESENT: OpStatFrame(CurrentFrame()); break;
    default: break;
    }
}
void OpStatFrame(uint64_t frame) {
    ++g_os.frames;
    if (g_osEvery <= 0 || g_os.frames < static_cast<uint64_t>(g_osEvery)) return;
    static const char* const nm[R_OP_COUNT] = {"nop", "jump", "reset", "bp", "xf", "vcd", "vat", "array", "arraycopy", "vtxstate", "tex", "tlut", "invaltex",
                                               "streamref", "streamcopy", "drawraw", "drawdec", "copytex", "copydisp", "destroycopy", "settarget", "flags", "present", "ext"};
    const double f = static_cast<double>(g_os.frames);
    std::fprintf(stderr, "[opstat] frame=%llu over %llu frames: records/frame %.0f (%.0f words)", static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(g_os.frames), g_os.total / f, g_os.totalWords / f);
    for (uint32_t op = 0; op < R_OP_COUNT; ++op)
        if (g_os.n[op]) std::fprintf(stderr, " | %s %.0f (same %.0f, %.0f w)", nm[op], g_os.n[op] / f, g_os.red[op] / f, g_os.words[op] / f);
    static const char* const xn[5] = {"pos", "nrm", "post", "light", "reg"};
    std::fprintf(stderr, "\n[opstat]   xf by class:");
    for (int c = 0; c < 5; ++c) std::fprintf(stderr, " %s %.0f (same %.0f, %.0f w)", xn[c], g_os.xfN[c] / f, g_os.xfRed[c] / f, g_os.xfWords[c] / f);
    const uint64_t epNow = HaveGuestRam() ? GuestWriteEpoch() : 0;
    std::fprintf(stderr, "\n[opstat]   tex bound again as it was, no write notification since the slot's last bind: %.0f/frame, with one: %.0f/frame; write notifications/frame %.0f",
                 g_os.texSameEpoch / f, g_os.texNotSame / f, static_cast<double>(epNow - g_os.epoch0) / f);
    g_os.epoch0 = epNow;
    std::fprintf(stderr, "\n[opstat]   draws/frame %.0f, vertices %.0f, stream bytes %.0f\n[opstat]   bp registers (count/same per frame):", g_os.draws / f, g_os.drawVerts / f, g_os.streamBytes / f);
    for (int k = 0; k < 14; ++k) {
        int best = -1;
        for (int i = 0; i < 256; ++i) if (g_os.bpN[i] && (best < 0 || g_os.bpN[i] > g_os.bpN[best])) best = i;
        if (best < 0) break;
        std::fprintf(stderr, " %02X:%.0f/%.0f", best, g_os.bpN[best] / f, g_os.bpRed[best] / f);
        g_os.bpN[best] = 0;
    }
    std::fprintf(stderr, "\n");
    const uint64_t keep = g_os.epoch0;
    g_os = OpStat{};
    g_os.epoch0 = keep;
}
} // namespace

template <bool STAT>
static uint32_t PlayPendingT() {
    if (!g_rc) g_rc = FirstChunk();
    uint32_t played = 0;
    for (;;) {
        // What has been committed is read once and played to its end before the game thread
        // is asked again (an atomic load with its memory barrier per record was a quarter of
        // this function: 13 000 records a frame).
        const uint32_t avail = g_rc->committed.load(std::memory_order_acquire);
        if (g_rpos >= avail) return played;
        const uint32_t* const base = g_rc->data;
        uint32_t pos = g_rpos;
        bool jumped = false;
        while (pos < avail) {
            const uint32_t* r = base + pos;
            if ((r[0] & 0xFF) == R_JUMP) {
                Chunk* next = g_rc->next.load(std::memory_order_acquire);
                if (g_prev) RecycleChunk(g_prev);
                g_prev = g_rc;
                g_rc = next;
                g_rpos = 0;
                jumped = true;
                break;
            }
            if (STAT) OpStatPre(r);
            const uint32_t len = PlayOne(r);
            if (STAT) OpStatLen(r[0] & 0xFF, len);
            pos += len;
            g_rpos = pos;
            ++played;
        }
        if (!jumped) g_rpos = pos;
    }
}

uint32_t PlayPending() {
    static const bool stat = [] {
        g_osEvery = EnvInt("MSC_VGX_OPSTAT", 0);
        return g_osEvery > 0;
    }();
    return stat ? PlayPendingT<true>() : PlayPendingT<false>();
}

static void* PlayerLoop(void*) {
    // MSC_VGX_PRESENT (vgx_present.h): between decoding slices the thread also runs the display ticks that are due.
    const bool wall = present::Wall();
    for (;;) {
        if (wall) present::Service();
        if (AsyncLog()) g_dbgPlayer.store(0);
        if (PlayPending() == 0) { // caught up with the game thread
            if (AsyncLog()) g_dbgPlayer.store(1);
            if (wall) present::Idle();
            else PlayerWaitForData();
        }
    }
    return nullptr;
}

static void (*g_playerThreadInit)() = nullptr;
void SetPlayerThreadInit(void (*fn)()) { g_playerThreadInit = fn; }
static void* PlayerThreadEntry(void*) {
    if (g_playerThreadInit) g_playerThreadInit();
    return PlayerLoop(nullptr);
}

// MSC_VGX_PROF (VRS): the waits of the two threads of MSC_VGX_BACKEND_THREAD=1 since the last call, in ms (false while that mode is off).
bool TakePipeWaitMs(double* playerSlotWaitMs, double* backEndIdleMs) {
    if (!g_async) return false;
    *playerSlotWaitMs = static_cast<double>(g_slotWaitNs.exchange(0, std::memory_order_relaxed)) / 1e6;
    *backEndIdleMs = static_cast<double>(g_beIdleNs.exchange(0, std::memory_order_relaxed)) / 1e6;
    return true;
}

// Called by the host layer on the thread that owns the graphics context.
void PlayerThreadMain() {
    static const bool async = EnvInt("MSC_VGX_BACKEND_THREAD", 0) != 0;
    if (present::Enabled() && !async) SetFrameSink(&present::Sink); // (not together with the back end thread: its loop would have to run the ticks)
    if (async) {
        g_async = true;
        SetFrameSink(&HandOver);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        pthread_t player;
        if (pthread_create(&player, &attr, &PlayerThreadEntry, nullptr) == 0) {
            pthread_attr_destroy(&attr);
            VGX_LOG("back end on its own thread: this thread executes command buffers, the player has a thread of its own\n");
            if (AsyncLog()) {
                pthread_t watcher;
                if (pthread_create(&watcher, nullptr, &AsyncWatcher, nullptr) == 0) pthread_detach(watcher);
                std::fprintf(stderr, "[vgx-async] watcher started (MSC_VGX_ASYNCLOG=1)\n");
            }
            const bool sema = SemaSync();
            static const bool prof = std::getenv("MSC_VGX_PROF") != nullptr && std::getenv("MSC_VGX_PROF")[0] != '0';
            for (;;) {
                if (AsyncLog()) g_dbgBack.store(0);
                uint32_t flags;
                if (sema) {
                    const auto t0 = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
                    g_semFull.Wait();
                    if (prof) g_beIdleNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
                    flags = g_slotFlags;
                } else {
                    std::unique_lock<std::mutex> lk(g_pipeMtx);
                    WaitPipe(lk, true);
                    flags = g_slotFlags;
                }
                if (AsyncLog()) g_dbgBack.store(1);
                backend::RequestCapture((flags & 1) != 0);
                backend::SetSkipDraws((flags & 2) != 0);
                interp::Submit(g_slot); // (backend::Execute unless MSC_VGX_INTERP is set)
                if (AsyncLog()) {
                    g_dbgBack.store(2);
                    const uint64_t n = g_dbgDrawn.fetch_add(1) + 1;
                    if (n <= 120) std::fprintf(stderr, "[vgx-async] drew %llu\n", static_cast<unsigned long long>(n));
                }
                g_slot.clear();
                if (sema) {
                    g_semEmpty.Post();
                } else {
                    std::unique_lock<std::mutex> lk(g_pipeMtx);
                    g_slotFull = false;
                    lk.unlock();
                    g_pipeCv.notify_all();
                }
                NotifyExecuted();
            }
        }
        pthread_attr_destroy(&attr);
        VGX_ERR("could not start the player thread; playing on this thread\n");
        g_async = false;
        SetFrameSink(nullptr);
    }
    PlayerLoop(nullptr);
}

} // namespace vgx::rec
