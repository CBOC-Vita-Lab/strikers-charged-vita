// 1.3 (MSC_DVD_RETRY): a disc read that fails is tried again on a freshly opened file before it ends the game.
//
// After standby (or the Home menu) a Vita can hand back a file descriptor that no longer reads: the read fails although the file is
// there and opens again once the storage is back, which can take a moment. The other GameCube Strikers port on the Vita (strikersVita)
// reopens and retries for about a second; 1.3 allows three (the first retry at once, 59 more 50 ms apart). dvd_native.cpp do_read first retried once at once; this policy makes it n attempts, the first
// at once and the rest 50 ms apart. Kept apart so that a host test can drive it with fakes (native/platform/sdk/tests/test_dvd_retry.cpp).
//
//   attempt(bool last) -> long   one read; >= 0 bytes on success, < 0 failure. `last` is true on the final try, the only one that
//                                may record the player's error message.
//   reset()                      drop the cached handle so the next attempt opens the file again.
//   pause()                      wait before that attempt (do_read: none before the first retry, 50 ms before later ones).
// `retries` is the number of extra attempts (0: one attempt, the 1.2 behaviour). A read that succeeds the first time runs exactly
// as before: no reset, no pause, the same single call. Returns the last attempt's result; *used = attempts made (when non-null).
#pragma once

namespace np {

template <class Attempt, class Reset, class Pause>
long RetryRead(int retries, Attempt attempt, Reset reset, Pause pause, int* used = nullptr) {
    if (retries < 0) retries = 0;
    long r = -1;
    int n = 0;
    for (;;) {
        const bool last = n == retries;
        r = attempt(last);
        ++n;
        if (r >= 0 || last) break;
        reset();
        pause();
    }
    if (used) *used = n;
    return r;
}

// MSC_DVD_RETRY: unset, empty or a word other than off = 60 extra attempts (about three seconds at 50 ms), 0 or off = none (1.2), n = n (capped at 200;
// 1 is the single retry of the first 1.3 design).
// (11 Nov: 60, about 3 s, was 20: the bound the Galaxy port uses for a file the game cannot start without.)
constexpr int kDvdRetryDefault = 60;
inline int DvdRetryCount(const char* env) {
    if (!env || !*env) return kDvdRetryDefault;
    if ((env[0] == 'o' || env[0] == 'O') && (env[1] == 'f' || env[1] == 'F')) return 0;
    if (env[0] < '0' || env[0] > '9') return kDvdRetryDefault;
    long v = 0;
    for (const char* p = env; *p >= '0' && *p <= '9'; ++p) {
        v = v * 10 + (*p - '0');
        if (v > 200) return 200;
    }
    return static_cast<int>(v);
}

} // namespace np
