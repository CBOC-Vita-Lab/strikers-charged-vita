// (1.3, MSC_AUDIO_STREAM_KEEP, default on) A streamed sound (music, commentary) whose reads fell behind used to be stopped for good ("STARVE":
// more than two reads pending when the next block is due). Reads are issued and completed by the game thread once per frame, so one long frame
// (stadium load, shader hitch, the console waking, a late disc read) starved every stream. Now the voices are paused where they are, nothing
// plays from a ring that was not refilled, and the stream resumes from the same place once every read it asked for has completed. After
// kGiveUpTicks updates (3 ms each, about 6 s) without that, a read was lost and the old stop applies.
#ifndef AUDIO_STREAM_KEEP_H
#define AUDIO_STREAM_KEEP_H

struct AudioStreamKeep
{
    enum Act { kNone, kPause, kResume, kGiveUp };
    static const unsigned kGiveUpTicks = 2000;
    bool starved;
    unsigned ticks;

    AudioStreamKeep() : starved(false), ticks(0) {}
    void Reset() { starved = false; ticks = 0; }

    // The place where the old code stopped the stream.
    Act Starve(bool keep)
    {
        if (!keep)
            return kGiveUp;
        if (starved)
            return kNone;
        starved = true;
        ticks = 0;
        return kPause;
    }

    // Every update of a stream: while starved, resume when no read is pending any more.
    Act Tick(int pendingReads)
    {
        if (!starved)
            return kNone;
        if (pendingReads <= 0)
        {
            starved = false;
            return kResume;
        }
        if (++ticks >= kGiveUpTicks)
        {
            starved = false;
            return kGiveUp;
        }
        return kNone;
    }
};

#endif
