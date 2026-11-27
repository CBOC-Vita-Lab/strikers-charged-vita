// np_delay19.h: the congestion multiplier's session start / floor / ceiling / step and the two switches.
// g++ -std=c++11 -I. [-DDELAY19_HEADER='"path"'] tools/net/delay19_test.cpp -o /tmp/delay19_test && /tmp/delay19_test
#ifdef DELAY19_HEADER
#include DELAY19_HEADER
#else
#include "platform/sdk/net/np_delay19.h"
#endif
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
int main() {
    int bad = 0;
    auto eq = [&](long a, long b, const char* w) { if (a != b) { std::printf("FAIL %s: %ld != %ld\n", w, a, b); ++bad; } };
    using namespace NpDelay19;
    const float fMin = 2.0f, fDef = 4.0f, fMax = 10.0f, inc = 0.3f, dec = 0.06f;
    // no choice: the game's own constants
    eq(SessionStart(0, fDef) == 4.0f, 1, "no choice starts at 4"); eq(SessionFloor(0, fMin) == 2.0f, 1, "no choice floor 2"); eq(SessionMax(0, fMax) == 10.0f, 1, "no choice max 10");
    // a choice is the start and the floor
    for (int d : {1, 2, 3, 4, 6, 10, 12, 20}) {
        eq(SessionStart(d, fDef) == float(d), 1, "start = choice"); eq(SessionFloor(d, fMin) == float(d), 1, "floor = choice");
        eq(SessionMax(d, fMax) == (d > 10 ? float(d) : 10.0f), 1, "max never below the choice");
        // 1000 calm messages: the limit (u32 of the multiplier) stays at the choice exactly
        float m = SessionStart(d, fDef);
        for (int i = 0; i < 1000; ++i) m = Step(m, false, SessionFloor(d, fMin), SessionMax(d, fMax), inc, dec);
        eq((unsigned)m, (unsigned)d, "calm: limit holds at the choice"); eq(m == float(d), 1, "calm: multiplier exactly the choice");
    }
    // the game's own: 4.0 decays to 2.0 in about 34 calm messages, limit 3 at once and 2 from the 17th
    { float m = 4.0f; unsigned first2 = 0; for (unsigned i = 1; i <= 100; ++i) { m = Step(m, false, 2.0f, 10.0f, inc, dec); if (!first2 && (unsigned)m == 2) first2 = i; }
      eq(first2 >= 16 && first2 <= 18, 1, "unset: limit 2 from about the 17th calm message"); eq(m == 2.0f, 1, "unset: floor 2"); }
    // congestion raises it, the ceiling holds, a calm run brings it back to the choice
    { const int d = 3; float m = 3.0f; for (int i = 0; i < 100; ++i) m = Step(m, true, 3.0f, 10.0f, inc, dec); eq(m == 10.0f, 1, "congested: ceiling 10");
      for (int i = 0; i < 1000; ++i) m = Step(m, false, 3.0f, 10.0f, inc, dec); eq(m == 3.0f, 1, "back to the choice"); (void)d; }
    { float m = 6.0f; for (int i = 0; i < 4; ++i) m = Step(m, true, 6.0f, 10.0f, inc, dec); eq((unsigned)m, 7, "6 + 4 congested messages = 7.2 -> limit 7"); }
    { float m = 12.0f; m = Step(m, true, 12.0f, SessionMax(12, 10.0f), inc, dec); eq(m == 12.0f, 1, "a choice above the game's ceiling is not cut down (and has no headroom)"); }
    // two consoles fed the same flags end with the same value (the claim of np_delay19.h)
    { float a = SessionStart(4, fDef), b = SessionStart(4, fDef); unsigned s = 12345;
      for (int i = 0; i < 100000; ++i) { s = s * 1664525u + 1013904223u; const bool c = (s >> 24) % 5 == 0;
          a = Step(a, c, SessionFloor(4, fMin), SessionMax(4, fMax), inc, dec); b = Step(b, c, SessionFloor(4, fMin), SessionMax(4, fMax), inc, dec); if (a != b) { eq(0, 1, "consoles differ"); break; } }
      eq(a >= 4.0f && a <= 10.0f, 1, "stays within [choice, max]"); }
    // switches
    eq(ParseDrain(nullptr), 0, "drain unset off"); eq(ParseDrain(""), 0, "drain empty off"); eq(ParseDrain("0"), 0, "drain 0 off"); eq(ParseDrain("1"), 1, "drain 1 on");
    eq(ParseRetryUs(nullptr), 0, "retry unset off"); eq(ParseRetryUs(""), 0, "retry empty off"); eq(ParseRetryUs("0"), 0, "retry 0 off"); eq(ParseRetryUs("3000"), 3000, "retry 3000");
    eq(ParseRetryUs("99999"), 8000, "retry capped"); eq(ParseRetryUs("-5"), 0, "retry negative off"); eq(ParseRetryUs("x"), 0, "retry junk off");
    std::printf("delay19_test: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
