// fi_replay: executes captured command buffers (vgx_capture.h) on the GL back end without
// the game, through the frame interpolation (vgx_interp.h), and writes every picture as a
// PNG. Workstream FI: the place where pairing, blending and the cut tests are developed and
// looked at (seconds per run, no game link), and, built for armhf over the stub GL, where
// their cost is counted under qemu.
//
//   fi_replay [options] <file.vgxcap> <out-dir>
//     -m <mode>     0 off, 1 one in-between per frame (default), 2 two, 32 three pictures per two frames
//     -s WxH        render size (default 960x544; the capture's game ran with that surface)
//     -f a-b        write pictures only for frames a..b of the file (default all)
//     -n            write no pictures (statistics / timing only)
//     -l            leave-one-out: of every two frames of the file, the first is fed to the
//                   interpolation and the second is only executed (its picture is the truth
//                   for the in-between of its neighbours): for a capture of the game at
//                   TWICE the built rate. Pictures: f<n>_t.png truth, f<n>_i0.png in-between.
//     -q            qemu instruction marks (tools/qemu-plugin) around each phase, see MarkLog
//     -g <k>        gap test: of every k frames of the file (a capture of the game at k times the built rate) the
//                   first is fed to the interpolation, the other k-1 are only executed (their pictures are the
//                   truth). For every fed frame B with predecessor A (k frames before): pictures f<B>_g<j>.png
//                   at t = j/k, j = 1..k-1 (interpolated between A and B); with -x also f<B>_x<j>.png at
//                   t = 1 + j/k (extrapolated past B), f<B>.png B itself; truth f<n>_t.png for the others and
//                   f<n>.png for the fed frames, so the error against the truth at every t can be measured
//                   (tools/fi_loo.py).
//     -c            only count what the frames hold (draws on the EFB and inside offscreen passes, passes, copies): one line per
//                   file, no GL needed
//     -x            with -g: also the extrapolated pictures
//     -i            idempotency check: every frame is executed twice (the second time as a
//                   replay) and the two pictures are compared; no interpolation
// Pictures: f<frame>_i<slot>.png in-between pictures, f<frame>.png the frame itself;
// f<frame>_same.txt (empty) when the frame was shown without an in-between.
#include "vgx_capture.h"
#include "vgx_interp.h"

#define XXH_INLINE_ALL
#include <xxhash.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x)
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace vgx {
bool LogEnabled() {
    static const bool on = EnvInt("MSC_VGX_LOG", 0) != 0;
    return on;
}
uint64_t HashKey(const void* key, size_t size) { return XXH3_64bits(key, size); }
} // namespace vgx

using namespace vgx;

namespace {
std::string g_out;
bool g_write = true, g_marks = false;
uint64_t g_first = 0, g_last = ~0ull;
uint64_t g_curFrame = 0;

double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void WritePng(const char* name) {
    int w = 0, h = 0;
    const std::vector<uint8_t>& px = backend::LastDisplayCopy(w, h);
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2];
    }
    const std::string path = g_out + "/" + name;
    stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
}
bool Wanted(uint64_t frame) { return g_write && frame >= g_first && frame <= g_last; }

void Picture(uint64_t frame, int slot, bool drawn, float t) {
    if (!Wanted(g_curFrame)) return;
    char name[96];
    if (!drawn) {
        std::snprintf(name, sizeof name, "%s/f%06llu_same.txt", g_out.c_str(), static_cast<unsigned long long>(g_curFrame));
        if (std::FILE* f = std::fopen(name, "w")) std::fclose(f);
        return;
    }
    if (t < 1.0f) std::snprintf(name, sizeof name, "f%06llu_i%d.png", static_cast<unsigned long long>(g_curFrame), slot);
    else std::snprintf(name, sizeof name, "f%06llu.png", static_cast<unsigned long long>(g_curFrame));
    WritePng(name);
}

// qemu: the plugin prints every thread's instruction count at each mark; MarkLog names them.
void Mark(long id) {
#if defined(__linux__)
    if (g_marks) syscall(0x7f00, id);
#endif
}
} // namespace

int main(int argc, char** argv) {
    int mode = 1, w = 960, h = 544;
    bool loo = false;
    int opt;
    bool idem = false;
    int gap = 0;
    bool extrap = false;
    bool census = false;
    while ((opt = getopt(argc, argv, "m:s:f:nlqig:xc")) != -1) {
        switch (opt) {
        case 'm': mode = std::atoi(optarg); break;
        case 's': std::sscanf(optarg, "%dx%d", &w, &h); break;
        case 'f': {
            unsigned long long a = 0, b = 0;
            if (std::sscanf(optarg, "%llu-%llu", &a, &b) == 2) { g_first = a; g_last = b; }
            break;
        }
        case 'n': g_write = false; break;
        case 'l': loo = true; break;
        case 'q': g_marks = true; break;
        case 'i': idem = true; mode = 0; break;
        case 'g': gap = std::atoi(optarg); break;
        case 'x': extrap = true; break;
        case 'c': census = true; break;
        default: return 2;
        }
    }
    if (argc - optind < 2) {
        std::fprintf(stderr, "usage: fi_replay [-m mode] [-s WxH] [-f a-b] [-n] [-l] [-q] <file.vgxcap> <out-dir>\n");
        return 2;
    }
    g_out = argv[optind + 1];
    mkdir(g_out.c_str(), 0755);
    capture::File cap;
    if (!capture::Load(argv[optind], cap)) {
        std::fprintf(stderr, "cannot read %s\n", argv[optind]);
        return 1;
    }
    std::fprintf(stderr, "[replay] %s: %u frames (%llu..%llu), preamble %u textures %u buffers %u shaders\n", argv[optind],
                 static_cast<unsigned>(cap.frames.size() - 1), static_cast<unsigned long long>(cap.frames[1].frame),
                 static_cast<unsigned long long>(cap.frames.back().frame), static_cast<unsigned>(cap.frames[0].uploads.size()),
                 static_cast<unsigned>(cap.frames[0].bufUploads.size()), static_cast<unsigned>(cap.frames[0].shaders.size()));
    if (census) {
        double nd = 0, dp = 0, np = 0, nct = 0, ncd = 0, nfr = 0, upl = 0, uniF = 0, vtxB = 0, npdraw = 0;
        for (size_t i = 1; i < cap.frames.size(); ++i) {
            const CmdBuf& b = cap.frames[i];
            bool inPass = false;
            uint32_t run = 0;
            for (const Cmd& c : b.cmds) {
                if (c.type == CMD_PASS_BEGIN) { inPass = true; run = 0; ++np; }
                else if (c.type == CMD_DRAW) { ++nd; if (inPass) ++run; }
                else if (c.type == CMD_COPY_TEX) { ++nct; if (inPass && !b.copyTex[c.arg].srcTex) { dp += run; inPass = false; run = 0; } }
                else if (c.type == CMD_COPY_DISP) ++ncd;
                else if (c.type == CMD_TEX_UPLOAD) ++upl;
            }
            uniF += b.uni.size(); vtxB += b.vtx.size(); ++nfr;
            if (std::getenv("FI_CENSUS_FRAMES")) {
                size_t nb = 0, bb = 0, nfree = 0, nstat = 0;
                for (const Cmd& c : b.cmds) {
                    if (c.type == CMD_BUF_UPLOAD) { ++nb; bb += b.bufUploads[c.arg].vtx.size() + b.bufUploads[c.arg].idx.size() * 2; }
                    else if (c.type == CMD_BUF_FREE) ++nfree;
                }
                for (const DrawRec& d : b.draws) if (d.vbuf) ++nstat;
                std::printf("  frame %llu: %zu draws (%zu static), static buffer uploads %zu (%zu KB), frees %zu, texture uploads %zu\n", static_cast<unsigned long long>(b.frame), b.draws.size(), nstat, nb, bb / 1024, nfree, b.uploads.size());
            }
        }
        (void)npdraw;
        std::printf("census %s: %.0f frames, per frame: draws %.1f (inside offscreen passes %.1f = %.0f%%), passes %.1f, copies to texture %.1f, display copies %.1f, texture uploads %.2f, uniform floats %.0f, vertex arena %.0f KB\n",
                    argv[optind], nfr, nd / nfr, dp / nfr, nd ? 100.0 * dp / nd : 0.0, np / nfr, nct / nfr, ncd / nfr, upl / nfr, uniF / nfr, vtxB / nfr / 1024.0);
        return 0;
    }
    if (!host::CreateContext() || !backend::Init(w, h)) {
        std::fprintf(stderr, "no GL context\n");
        return 1;
    }
    backend::RequestCapture(g_write);
    interp::SetMode(mode);
    interp::SetPictureHook(&Picture);
    // The preamble: resources only. Through Submit so that the interpolation sees the shaders.
    if (mode) interp::Submit(cap.frames[0]);
    else backend::Execute(cap.frames[0]);
    double tSubmit = 0;
    uint32_t n = 0;
    uint64_t seq = 1; // (leave-one-out: the interpolation must see consecutive frame numbers)
    Mark(1);
    for (size_t i = 1; i < cap.frames.size(); ++i) {
        CmdBuf& buf = cap.frames[i];
        g_curFrame = buf.frame;
        if (loo && (i & 1) == 0) {
            // The frame in between: executed as it is; its picture is the truth.
            backend::Execute(buf);
            if (Wanted(g_curFrame)) {
                char name[64];
                std::snprintf(name, sizeof name, "f%06llu_t.png", static_cast<unsigned long long>(g_curFrame));
                WritePng(name);
            }
            continue;
        }
        if (gap > 1) {
            // gap test (see the header)
            const uint64_t orig = buf.frame;
            if ((i - 1) % static_cast<size_t>(gap) != 0) {
                backend::Execute(buf); // (truth frame: executed as it is)
                if (Wanted(orig)) {
                    char name[64];
                    std::snprintf(name, sizeof name, "f%06llu_t.png", static_cast<unsigned long long>(orig));
                    WritePng(name);
                }
                continue;
            }
            buf.frame = seq++;
            g_curFrame = orig;
            const double t0g = NowMs();
            const bool ok = interp::Begin(buf);
            auto name = [&](const char* fmt, int j) {
                static char b[96];
                std::snprintf(b, sizeof b, fmt, static_cast<unsigned long long>(orig), j);
                return b;
            };
            if (ok) {
                for (int j = 1; j < gap; ++j) {
                    interp::Blend(buf, static_cast<float>(j) / static_cast<float>(gap), j == 1, false);
                    if (Wanted(orig)) WritePng(name("f%06llu_g%d.png", j));
                }
                interp::Self(buf, false, false);
            } else {
                interp::Self(buf, true, false);
                if (Wanted(orig)) {
                    char nm[96];
                    std::snprintf(nm, sizeof nm, "%s/f%06llu_same.txt", g_out.c_str(), static_cast<unsigned long long>(orig));
                    if (std::FILE* f = std::fopen(nm, "w")) std::fclose(f);
                }
            }
            if (Wanted(orig)) WritePng(name("f%06llu%.0d.png", 0));
            if (extrap && ok) {
                for (int j = 1; j < gap; ++j) {
                    interp::Blend(buf, 1.0f + static_cast<float>(j) / static_cast<float>(gap), false, false);
                    if (Wanted(orig)) WritePng(name("f%06llu_x%d.png", j));
                }
            }
            interp::End(buf);
            tSubmit += NowMs() - t0g;
            ++n;
            continue;
        }
        const double t0 = NowMs();
        if (mode) {
            if (loo) buf.frame = seq++;
            Mark(static_cast<long>(100 + i));
            interp::Submit(buf);
            if (g_marks) {
                const interp::FrameStats& st = interp::LastStats();
                std::fprintf(stderr, "[marklog] %u frame %llu pictures %u draws %u paired %u floats %u\n", static_cast<unsigned>(100 + i),
                             static_cast<unsigned long long>(g_curFrame), st.pictures, st.draws, st.paired, st.floatsAppended);
            }
        } else if (idem) {
            // Is a second execution of the frame (ExecOpts::replay) the same picture as the first?
            backend::RequestCapture(true);
            backend::Execute(buf);
            int w1 = 0, h1 = 0;
            const std::vector<uint8_t> first = backend::LastDisplayCopy(w1, h1);
            backend::ExecOpts o;
            o.replay = true;
            backend::ExecuteEx(buf, o);
            int w2 = 0, h2 = 0;
            const std::vector<uint8_t>& second = backend::LastDisplayCopy(w2, h2);
            size_t diff = 0;
            int worst = 0;
            if (first.size() == second.size()) {
                for (size_t k = 0; k < first.size(); k += 4) {
                    int d = 0;
                    for (int ch = 0; ch < 3; ++ch) d = std::max(d, std::abs(static_cast<int>(first[k + ch]) - static_cast<int>(second[k + ch])));
                    if (d) ++diff;
                    if (d > worst) worst = d;
                }
            } else {
                diff = ~static_cast<size_t>(0);
            }
            std::fprintf(stderr, "[idem] frame %llu: %zu pixels differ between the first and the second execution (largest difference %d)\n",
                         static_cast<unsigned long long>(g_curFrame), diff, worst);
            if (diff && Wanted(g_curFrame)) {
                char name[64];
                std::snprintf(name, sizeof name, "f%06llu_second.png", static_cast<unsigned long long>(g_curFrame));
                WritePng(name);
            }
        } else {
            Mark(static_cast<long>(100 + i));
            backend::Execute(buf);
            if (Wanted(g_curFrame)) {
                char name[64];
                std::snprintf(name, sizeof name, "f%06llu.png", static_cast<unsigned long long>(g_curFrame));
                WritePng(name);
            }
        }
        tSubmit += NowMs() - t0;
        ++n;
    }
    Mark(2);
    const interp::Totals& tot = interp::GetTotals();
    std::fprintf(stderr, "[replay] mode %d: %u frames, %.2f ms per built frame on this machine (rendering and PNG files included)\n", mode, n, n ? tSubmit / n : 0.0);
    if (mode)
        std::fprintf(stderr, "[replay] interpolation: %llu frames, %llu with in-between pictures; without: first %llu, not replayable %llu, too few pairs %llu, cut %llu, gap %llu, EFB state %llu; draws %llu, paired %llu (%.1f %%), jumps %llu; %.0f floats appended per frame\n",
                     static_cast<unsigned long long>(tot.frames), static_cast<unsigned long long>(tot.withInBetween), static_cast<unsigned long long>(tot.cuts[1]),
                     static_cast<unsigned long long>(tot.cuts[2]), static_cast<unsigned long long>(tot.cuts[3]), static_cast<unsigned long long>(tot.cuts[4]),
                     static_cast<unsigned long long>(tot.cuts[6]), static_cast<unsigned long long>(tot.cuts[7]), static_cast<unsigned long long>(tot.draws), static_cast<unsigned long long>(tot.paired),
                     tot.draws ? 100.0 * static_cast<double>(tot.paired) / static_cast<double>(tot.draws) : 0.0, static_cast<unsigned long long>(tot.jumps),
                     tot.frames ? static_cast<double>(tot.floats) / static_cast<double>(tot.frames) : 0.0);
    interp::PrintTiming();
    std::fflush(stderr);
    _exit(0); // (no teardown of the GL context)
}
