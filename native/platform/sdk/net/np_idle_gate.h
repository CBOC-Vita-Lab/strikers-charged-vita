// np_idle_gate.h (1.3, wave 9): the link layer's work while NO session exists must not cost the game thread a system call every picture.
// np_vita_lan_frame (vlink_vita.cpp) runs once per presented frame whatever the game does; once any session had created the power callback it
// called sceKernelCheckCallback every picture of single player as well, only to drop the notice ("a sleep with no link up is nobody's business").
// The notice is forgotten anyway, so looking once a second is as good. Header only, free of platform types (tests/test_idle_gate.cpp).
#ifndef NP_IDLE_GATE_H
#define NP_IDLE_GATE_H

namespace NpIdle {

struct Every {
    unsigned n;
    Every() : n(0) {}
    // true once in `period` calls (the first call after Reset() is not due), so an idle caller does its system call about once a second at 60 pictures
    bool Due(unsigned period) {
        if (period <= 1) return true;
        if (++n < period) return false;
        n = 0;
        return true;
    }
    void Reset() { n = 0; }
};

constexpr unsigned kIdlePeriodFrames = 64;   // about a second

}  // namespace NpIdle

#endif
