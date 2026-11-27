// The pacing rules of the movie player's decode lanes as pure functions (no threads, no clock, no SDK): the delay line
// (MSC_THP_DELAY), the picture a call shows, the order the workers take pictures in. Ported from Galaxy's movie36
// (thp_video.cpp thpDue / thpChooseShow / thpJobStale / thpPickJob), adapted to Strikers: the sequence number of a picture
// is the movie's FRAME NUMBER (a skipped packet leaves a gap, which is movie time passing), and a call that has nothing new
// to show answers -1 ("the picture on the screen stays": the textures are written in place, so nothing needs to be shown again).
// tests/movie/thp_delay.cpp drives these on a model of a decoder and checks the real lanes with them.
//
// Terms: a "call" is one NpThpVideoDecode (one packet, one movie frame, due every 2 retraces). The call that asks for frame
// `seq` shows frame `seq - delay` (delay 0: the frame asked for, the behaviour before the delay line).
#ifndef NP_THP_PACING_H
#define NP_THP_PACING_H
#include <stdint.h>

namespace np_thp_pace {

// The frame that is due at the call that asks for `seq`, in a movie whose first frame is `base`: the first frame until `delay`
// calls have passed, then seq - delay.
inline int64_t Due(int64_t base, int64_t seq, int64_t delay) { return seq >= base + delay ? seq - delay : base; }

// Which finished frame the call that asks for `seq` shows. seqs[i] / done[i] describe the lanes' slots (done: decoded without
// an error). Returns the slot index, or -1: nothing new, the picture on the screen stays.
// A frame is a candidate when it is newer than the one shown last (never a step back) and not newer than the due one (never
// early: a movie does not run ahead of its sound); of the candidates the newest is shown, so a frame that comes late is
// passed over for the one due next instead of being shown after it (no repeat, no drift).
inline int ChooseShow(const int64_t* seqs, const bool* done, int n, int64_t base, int64_t seq, int64_t delay, bool anyShown, int64_t lastShown) {
    const int64_t due = Due(base, seq, delay);
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!done[i] || seqs[i] < base || seqs[i] > due) continue;
        if (anyShown && seqs[i] <= lastShown) continue;
        if (best < 0 || seqs[i] > seqs[best]) best = i;
    }
    return best;
}

// A queued frame nobody will show on time any more: older than the frame due at the NEXT call (`nextSeq` is that call's
// frame), or not newer than the one shown last. A worker drops it instead of decoding it.
inline bool JobStale(int64_t jobSeq, int64_t base, int64_t nextSeq, int64_t delay, bool anyShown, int64_t lastShown) {
    return jobSeq < Due(base, nextSeq, delay) || (anyShown && jobSeq <= lastShown);
}

// Which queued frame a worker takes: the OLDEST one that can still be finished before the call it is due at (earliest deadline
// first), and when none can, the newest (the one with the most time left). Frame s is due at the call that asks for s + delay;
// the last call asked for nextSeq - 1, `usSinceCall` microseconds ago, calls come every `periodUs`; a decode takes `needUs`.
// With a decoder that keeps up this is "in order"; with one that does not, the frames it can no longer make are left (and are
// dropped as stale at the next call) and the frames shown are spread evenly instead of in bursts.
inline int PickJob(const int64_t* seqs, int n, int64_t nextSeq, int64_t delay, int64_t usSinceCall, int64_t periodUs, int64_t needUs) {
    int oldestOk = -1, newest = -1;
    for (int i = 0; i < n; i++) {
        if (newest < 0 || seqs[i] > seqs[newest]) newest = i;
        const int64_t callsAhead = seqs[i] + delay + 1 - nextSeq;   // calls from now until the one the frame is due at
        const int64_t slackUs = callsAhead * periodUs - usSinceCall;
        if (slackUs >= needUs && (oldestOk < 0 || seqs[i] < seqs[oldestOk])) oldestOk = i;
    }
    return oldestOk >= 0 ? oldestOk : newest;
}

// Consecutive calls on which a frame was due that no lane had finished (nothing was shown although something new should
// have been): the "freeze" counter of MSC_THP_RECOVER. A call that showed a picture, or on which no new frame was due, resets it.
inline int StarveNext(int starved, bool newDue, bool shownNow) { return shownNow || !newDue ? 0 : starved + 1; }

} // namespace np_thp_pace
#endif
