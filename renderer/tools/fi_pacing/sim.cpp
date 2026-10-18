// Offline pacing simulator for the frame presenter (workstream FI2): runs the SAME scheduler as the game
// (src/vgx_present_sched.h) against a model of the game thread, and reports what the display would show.
//
//   sim [--tick ms] [--build const:<ms> | rand:<lo>:<hi> | norm:<mean>:<sd>:<min> | file:<path> | steps:<ms>] [--frames n] [--tail ms]
//       [--controller 0|1] [--margin ms] [--gain g] [--extrap ms] [--seed s] [--cutevery n] [--csv path] [--label text]
//
// Model of the game thread: frame i starts at g_i (its game time), takes b_i, and is handed to the render thread at
// ready_i = g_i + b_i + tail (the decode of its last records); the game thread begins frame i+1 right then
// (g_{i+1} = ready_i): it is the bottleneck, as on the console. At every display tick the frames that are ready are
// delivered to the scheduler, then the scheduler decides the picture. Reported: pictures by kind, the display delay
// (tick time minus content time shown), and how evenly the content advances (the second difference of the content time
// shown per tick; zero for a display that shows uniform motion), against a plain 30 Hz display of the newest built
// frame (no interpolation) on the same timeline.
#include "../../src/vgx_present_sched.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vgx::present;

struct Rec { double tick, c, t; Kind kind; uint64_t frame; double rate; };

struct StubBackend : Backend {
    int cutEvery = 0;
    uint64_t n = 0;
    bool Prepare(uint64_t, bool) override { ++n; return !(cutEvery && n > 1 && n % cutEvery == 0); }
    void Blend(uint64_t, float, bool) override {}
    void Self(uint64_t, bool, bool) override {}
    void Finish(uint64_t) override {}
    void Repeat() override {}
};

static double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

int main(int argc, char** argv) {
    SchedCfg cfg;
    std::string build = "const:66.67", csv, label;
    int frames = 600, cutEvery = 0;
    double tail = 4.0;
    unsigned seed = 1;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* k) { return !std::strcmp(argv[i], k) && i + 1 < argc; };
        if (is("--tick")) cfg.tickMs = std::atof(argv[++i]);
        else if (is("--build")) build = argv[++i];
        else if (is("--frames")) frames = std::atoi(argv[++i]);
        else if (is("--tail")) tail = std::atof(argv[++i]);
        else if (is("--controller")) cfg.controller = std::atoi(argv[++i]) != 0;
        else if (is("--margin")) cfg.marginMs = std::atof(argv[++i]);
        else if (is("--gain")) cfg.gain = std::atof(argv[++i]);
        else if (is("--extrap")) cfg.extrapMs = std::atof(argv[++i]);
        else if (is("--seed")) seed = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (is("--cutevery")) cutEvery = std::atoi(argv[++i]);
        else if (is("--csv")) csv = argv[++i];
        else if (is("--label")) label = argv[++i];
        else { std::fprintf(stderr, "bad argument %s\n", argv[i]); return 2; }
    }
    // build durations
    std::vector<double> b(frames);
    std::mt19937 rng(seed);
    if (!build.compare(0, 6, "const:")) { for (auto& x : b) x = std::atof(build.c_str() + 6); }
    else if (!build.compare(0, 5, "rand:")) {
        double lo, hi; std::sscanf(build.c_str() + 5, "%lf:%lf", &lo, &hi);
        std::uniform_real_distribution<double> d(lo, hi); for (auto& x : b) x = d(rng);
    } else if (!build.compare(0, 5, "norm:")) {
        double mean, sd, mn; std::sscanf(build.c_str() + 5, "%lf:%lf:%lf", &mean, &sd, &mn);
        std::normal_distribution<double> d(mean, sd); for (auto& x : b) x = std::max(mn, d(rng));
    } else if (!build.compare(0, 6, "steps:")) {
        // frame cost = fixed part + 3.6 M per simulation step at 230 M/s: the number of steps follows the elapsed time (20 ms steps)
        double fixed = std::atof(build.c_str() + 6), carry = 0, prev = fixed;
        for (auto& x : b) {
            carry += prev; int steps = static_cast<int>(carry / 20.0); if (steps > 5) steps = 5; carry -= 20.0 * steps; if (carry > 20) carry = 20;
            x = fixed + 15.6 * steps; prev = x;
        }
    } else if (!build.compare(0, 5, "file:")) {
        FILE* f = std::fopen(build.c_str() + 5, "r"); if (!f) { std::perror("file"); return 2; }
        b.clear(); double x; while (std::fscanf(f, "%lf", &x) == 1) b.push_back(x); std::fclose(f);
        if (b.empty()) { std::fprintf(stderr, "empty file\n"); return 2; }
        frames = static_cast<int>(b.size());
    } else { std::fprintf(stderr, "bad --build\n"); return 2; }

    // frames: g_i, ready_i
    std::vector<double> g(frames), ready(frames);
    double t0 = 0;
    for (int i = 0; i < frames; ++i) { g[i] = t0; ready[i] = t0 + b[i] + tail; t0 = ready[i]; }
    StubBackend be; be.cutEvery = cutEvery;
    Scheduler sch(cfg, be);
    std::vector<Rec> recs;
    const double T = cfg.tickMs;
    // the display's first tick: when the first frame is ready
    double tick = ready[0];
    int next = 0;
    const double end = ready[frames - 1];
    while (tick <= end) {
        while (next < frames && ready[next] <= tick + 1e-9) { sch.Arrive(next, g[next], ready[next]); ++next; }
        TickInfo ti = sch.Tick(tick);
        recs.push_back({tick, sch.clock(), ti.t, ti.kind, ti.frame, sch.rate()});
        tick += T;
    }
    // baseline: 30 Hz display of the newest built frame, no interpolation
    std::vector<double> base;
    { int k = 0; for (double tk = ready[0]; tk <= end; tk += T) { while (k + 1 < frames && ready[k + 1] <= tk + 1e-9) ++k; base.push_back(g[k]); } }

    const SchedStats& st = sch.stats();
    // analysis over the second half (the first frames are the buffer filling)
    size_t from = recs.size() / 5;
    std::vector<double> delay, dd2, bdd2, bdelay;
    std::vector<double> cs;
    for (size_t i = from; i < recs.size(); ++i) { cs.push_back(recs[i].c); delay.push_back(recs[i].tick - recs[i].c); }
    uint64_t nrep = 0, nblend = 0, nself = 0, nstall = 0, nhold = 0;
    for (size_t i = from; i < recs.size(); ++i) {
        switch (recs[i].kind) { case Kind::Repeat: ++nrep; break; case Kind::Blend: ++nblend; break; case Kind::Self: ++nself; break; case Kind::Stall: ++nstall; break; case Kind::Hold: ++nhold; break; default: break; }
    }
    double speedDev = 0, speedMax = 0; uint64_t zero = 0, off = 0;
    for (size_t i = from + 1; i < recs.size(); ++i) {
        const double step = recs[i].c - recs[i - 1].c;
        if (step < 0.01) ++zero;
        else if (std::fabs(step - T) > 0.2 * T) ++off;
        const double dv = std::fabs(step - T);
        speedDev += dv * dv; if (dv > speedMax) speedMax = dv;
    }
    speedDev = std::sqrt(speedDev / std::max<size_t>(1, recs.size() - from - 1));
    // baseline
    uint64_t bzero = 0; double bdev = 0, bmax = 0;
    for (size_t i = from + 1; i < base.size(); ++i) {
        const double step = base[i] - base[i - 1];
        if (step < 0.01) ++bzero;
        const double dv = std::fabs(step - T); bdev += dv * dv; if (dv > bmax) bmax = dv;
        bdelay.push_back(recs[i].tick - base[i]);
    }
    bdev = std::sqrt(bdev / std::max<size_t>(1, base.size() - from - 1));
    double meanB = 0; for (double x : b) meanB += x; meanB /= b.size();
    double sdB = 0; for (double x : b) sdB += (x - meanB) * (x - meanB); sdB = std::sqrt(sdB / b.size());
    std::printf("%s tick %.2f ms, build mean %.1f ms (%.1f fps) sd %.1f, %d frames, %zu ticks, controller %d margin %.0f\n", label.c_str(), T, meanB, 1000.0 / (meanB + tail), sdB, frames, recs.size(), cfg.controller, cfg.marginMs);
    std::printf("  pictures (steady part): blend %llu self %llu repeat %llu (stall %llu, hold-for-cut %llu) of %zu ticks; hidden frames %llu, bypass ticks %llu\n",
                (unsigned long long)nblend, (unsigned long long)nself, (unsigned long long)nrep, (unsigned long long)nstall, (unsigned long long)nhold, recs.size() - from,
                (unsigned long long)st.hidden, (unsigned long long)st.bypassTicks);
    std::printf("  display delay behind the game thread's frame start (tick - content time): mean %.1f p5 %.1f p95 %.1f max %.1f ms; one built frame = %.1f ms\n",
                [&] { double s = 0; for (double x : delay) s += x; return delay.empty() ? 0.0 : s / delay.size(); }(), Percentile(delay, 0.05), Percentile(delay, 0.95), Percentile(delay, 1.0), meanB + tail);
    std::printf("  content advance per tick vs %.1f ms ideal: rms deviation %.2f ms, worst %.1f ms, ticks without any advance %llu (%.1f%%), ticks off by more than 20%% %llu (%.1f%%)\n",
                T, speedDev, speedMax, (unsigned long long)zero, 100.0 * zero / std::max<size_t>(1, recs.size() - from), (unsigned long long)off, 100.0 * off / std::max<size_t>(1, recs.size() - from));
    std::printf("  baseline (30 Hz display of the newest built frame, no interpolation): delay mean %.1f ms, content advance rms deviation %.2f ms, worst %.1f, ticks without advance %llu (%.1f%%)\n",
                [&] { double s = 0; for (double x : bdelay) s += x; return bdelay.empty() ? 0.0 : s / bdelay.size(); }(), bdev, bmax, (unsigned long long)bzero, 100.0 * bzero / std::max<size_t>(1, base.size() - from));
    if (!csv.empty()) {
        FILE* f = std::fopen(csv.c_str(), "w");
        std::fprintf(f, "tick,c,delay,kind,frame,t,rate\n");
        for (auto& r : recs) std::fprintf(f, "%.3f,%.3f,%.3f,%d,%llu,%.3f,%.3f\n", r.tick, r.c, r.tick - r.c, (int)r.kind, (unsigned long long)r.frame, r.t, r.rate);
        std::fclose(f);
    }
    return 0;
}
