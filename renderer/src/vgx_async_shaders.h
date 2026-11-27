// 1.3 MSC_VGX_ASYNC_SHADERS (after Galaxy r101 GXV_ASYNC_SHADERS): the shader compile worker's queue and the rules a draw follows while
// one of its shaders is being compiled. No GL, no vitaGL: the back end (vgx_backend_gl.cpp) owns the policy's inputs, the Vita host
// (vita/vgx_host_vita.cpp) the thread, vitaGL patch 0027 the compile into the shader cache. Test: renderer/tests/run_async_shaders_test.sh.
//
// What happens. A shader a running game meets for the first time (CMD_SHADER after the warm-up frames) whose compiled form is in neither
// the package's pack, the card's cache nor the seed is not compiled on the render thread: its text goes to this queue and a worker
// compiles it into vitaGL's cache (vglCompileShaderToCache: compiled, written, never registered; vitaGL's compiler lock is taken around
// every compile, the render thread's own glCompileShader included). When a draw needs a shader that is not ready:
//  * a draw that may be skipped (Skippable below: a colour draw of the main scene that nothing later in the frame copies) waits at most
//    MSC_VGX_ASYNC_SHADERS_MAXWAIT_MS for the worker and is not drawn if the shader is still not ready (that frame lacks it);
//  * any other draw (render-target passes, EFB copies' sources, depth-only draws, warm-ups, anything the back end cannot classify) takes
//    the blocking path: a queued job is taken back and compiled on the render thread as before, a job the worker is compiling is waited for;
//  * a skippable draw of a job older than kMaxSkipAgeMs takes the blocking path too ("waited too long"): an object is never missing for
//    more than about that long, whatever the queue.
// Once the worker is done, the render thread's glCompileShader finds the file in the cache (a load, not a compile).
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vgx::async {

enum class JobState : uint8_t { None, Queued, Running, Done, Failed };

// A job: one shader (vertex or fragment, by its back-end slot) and the exact text glShaderSource gets after the version line.
struct Job {
    uint32_t key = 0;   // ShaderKey
    std::string text;
    double submittedMs = 0;
};
inline uint32_t ShaderKey(bool fragment, uint32_t id) { return (fragment ? 0x80000000u : 0u) | (id & 0x7FFFFFFFu); }

// The decision for one draw whose program lacks a compiled shader (the back end's GetProgram, before it compiles anything).
enum class Action : uint8_t {
    CompileNow,    // nothing queued or running for it (or the worker is done or failed): the render thread loads / compiles as before
    WaitThenSkip,  // skippable: wait for the worker at most maxWaitMs; still not ready: the draw is not made this time
    Block,         // must not be skipped: take queued jobs back, wait for running ones, then compile / load on the render thread
};
constexpr double kMaxSkipAgeMs = 400.0;   // a job older than this: its draws take the blocking path ("waited too long")

// Pure: the rule the back end applies. pending = a job of the draw's shaders is queued or running; ageMs = the oldest such job's age.
inline Action Decide(bool pending, bool skippable, double ageMs) {
#if defined(VGX_ASYNC_BREAK) && VGX_ASYNC_BREAK == 1
    (void)skippable; (void)ageMs;
    return pending ? Action::WaitThenSkip : Action::CompileNow;   // deliberate break: a draw that must not be skipped may be
#elif defined(VGX_ASYNC_BREAK) && VGX_ASYNC_BREAK == 2
    (void)skippable;
    return pending && ageMs < kMaxSkipAgeMs ? Action::WaitThenSkip : (pending ? Action::Block : Action::CompileNow);   // deliberate break: class ignored
#elif defined(VGX_ASYNC_BREAK) && VGX_ASYNC_BREAK == 3
    (void)ageMs;
    return pending && skippable ? Action::WaitThenSkip : (pending ? Action::Block : Action::CompileNow);   // deliberate break: no age bound
#else
    if (!pending) return Action::CompileNow;
    if (!skippable || ageMs >= kMaxSkipAgeMs) return Action::Block;
    return Action::WaitThenSkip;
#endif
}

// What a draw is (the back end fills it from its own state).
struct DrawClass {
    bool inPass = false;          // a render-target pass (g_inPass): its texture is read later, maybe in later frames
    bool fxTarget = false;        // the additive-effects target (composited later)
    bool writesColor = false;     // colour or alpha write on (false: a depth-only draw)
    bool copyLater = false;       // an EFB copy to a texture comes after this draw in the frame's commands (it would copy the hole)
    bool warmup = false;          // a warm-up, a pass memo, a crowd run or any other caller than the plain draw
};
// Pure: may this draw be left out of one frame? Only plain colour draws of the main scene that nothing later in the frame copies.
inline bool Skippable(const DrawClass& d) {
#if defined(VGX_ASYNC_BREAK) && VGX_ASYNC_BREAK == 4
    return !d.warmup && d.writesColor;   // deliberate break: passes and copied draws counted as skippable
#else
    return !d.warmup && !d.inPass && !d.fxTarget && d.writesColor && !d.copyLater;
#endif
}

struct Stats {
    uint64_t compiled = 0, failed = 0, skipped = 0, blockMust = 0, blockAge = 0, waitedOwn = 0, found = 0, probes = 0, cancelled = 0;
    double compileMs = 0, longestMs = 0, longestWaitMs = 0, probeMs = 0;
};

// The queue. All members may be called from the render thread; Run() is the worker's loop. Compile(key, text) returns true when the
// shader is now in the cache.
class Queue {
public:
    using Clock = std::chrono::steady_clock;
    static double NowMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }

    // Render thread: queue a shader (once; a key already known is left as it is). Returns false when it was not queued (known already).
    bool Submit(uint32_t key, const std::string& text) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(key);
        if (it != state_.end() && it->second != JobState::Failed && it->second != JobState::None) return false;
        state_[key] = JobState::Queued;
        jobs_.push_back(Job{key, text, NowMs()});
        cv_.notify_all();
        return true;
    }
    JobState State(uint32_t key) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(key);
        return it == state_.end() ? JobState::None : it->second;
    }
    bool Pending(uint32_t key) const { const JobState s = State(key); return s == JobState::Queued || s == JobState::Running; }
    double SubmittedMs(uint32_t key) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const Job& j : jobs_) if (j.key == key) return j.submittedMs;
        auto it = runningSince_.find(key);
        return it == runningSince_.end() ? NowMs() : it->second;
    }
    // Render thread, blocking path: a job not started yet is taken back (the caller compiles it). True when it was.
    bool Cancel(uint32_t key) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
            if (it->key == key) {
                jobs_.erase(it);
                state_[key] = JobState::None;
                ++stats_.cancelled;
                return true;
            }
        return false;
    }
    // Render thread: wait until none of the keys is queued or running, at most timeoutMs (< 0: no limit). True when none is.
    bool Wait(const uint32_t* keys, int n, double timeoutMs) {
        std::unique_lock<std::mutex> lk(mtx_);
        auto busy = [&] {
            for (int i = 0; i < n; ++i) {
                auto it = state_.find(keys[i]);
                if (it != state_.end() && (it->second == JobState::Queued || it->second == JobState::Running)) return true;
            }
            return false;
        };
        if (!busy()) return true;
        if (timeoutMs < 0) {
            cv_.wait(lk, [&] { return !busy() || stop_; });
            return !busy();
        }
        if (timeoutMs == 0) return false;
        return cv_.wait_for(lk, std::chrono::duration<double, std::milli>(timeoutMs), [&] { return !busy() || stop_; }) && !busy();
    }
    // Render thread: the shader was compiled or loaded there; the record goes (a later identical key is a new job).
    void Forget(uint32_t key) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(key);
        if (it != state_.end() && it->second != JobState::Queued && it->second != JobState::Running) state_.erase(it);
    }
    unsigned Size() const { std::lock_guard<std::mutex> lk(mtx_); return static_cast<unsigned>(jobs_.size()); }
    void Stop() { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; cv_.notify_all(); }

    // Worker thread.
    template <typename CompileFn> void Run(CompileFn compile) {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                state_[job.key] = JobState::Running;
                runningSince_[job.key] = job.submittedMs;
            }
            const double t0 = NowMs();
            const bool ok = compile(job.key, job.text);
            const double ms = NowMs() - t0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                state_[job.key] = ok ? JobState::Done : JobState::Failed;
                runningSince_.erase(job.key);
                if (ok) ++stats_.compiled; else ++stats_.failed;
                stats_.compileMs += ms;
                if (ms > stats_.longestMs) stats_.longestMs = ms;
            }
            cv_.notify_all();
        }
    }
    // Counters the render thread adds (under the same lock as the worker's).
    template <typename F> void Note(F f) { std::lock_guard<std::mutex> lk(mtx_); f(stats_); }
    Stats Snapshot() const { std::lock_guard<std::mutex> lk(mtx_); return stats_; }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::unordered_map<uint32_t, JobState> state_;
    std::unordered_map<uint32_t, double> runningSince_;
    Stats stats_;
    bool stop_ = false;
};

} // namespace vgx::async
