// np_galaxy16.h: the ad hoc radio's Galaxy-derived decisions (MSC_LAN_RADIO_FIX bits, dialog switches, teardown and error rules, MAC verdict).
// g++ -std=c++11 -I. tools/net/galaxy16_test.cpp -o /tmp/galaxy16_test && /tmp/galaxy16_test
#include "platform/sdk/net/np_galaxy16.h"
#include <cstdio>
#include <cstring>
int main() {
    int bad = 0;
    auto eq = [&](long a, long b, const char* w) { if (a != b) { std::printf("FAIL %s: %ld != %ld\n", w, a, b); ++bad; } };
    using namespace NpGalaxy16;
    // the switch: default off, decimal 0..15, anything else off
    eq(ParseRadioFix(nullptr), 0, "null"); eq(ParseRadioFix(""), 0, "empty"); eq(ParseRadioFix("0"), 0, "0");
    eq(ParseRadioFix("1"), 1, "1"); eq(ParseRadioFix("15"), 15, "15"); eq(ParseRadioFix("16"), 0, "16 refused"); eq(ParseRadioFix("-1"), 0, "negative");
    eq(ParseRadioFix("7x"), 0, "trailing text"); eq(ParseRadioFix("0x3"), 0, "hex refused"); eq(ParseRadioFix(" 3"), 3, "strtol skips leading blank");
    eq(kRadioSoftClose | kRadioStrayErrors | kRadioAnyPort | kRadioDiag, kRadioAll, "bits cover 15");
    // dialog switches: defaults are the 1.2 values
    eq(LanGroup(nullptr), 1, "group default"); eq(LanGroup("0"), 0, "group 0"); eq(LanGroup("1"), 1, "group 1"); eq(LanGroup("2"), 1, "group 2 refused -> default");
    eq(long(LanTimeoutUs(nullptr)), 60000000L, "timeout default"); eq(long(LanTimeoutUs("1000")), 1000000L, "timeout 1000 ms");
    eq(long(LanTimeoutUs("0")), 0, "timeout 0 passed as 0"); eq(long(LanTimeoutUs("3600001")), 60000000L, "timeout too big -> default"); eq(long(LanTimeoutUs("abc")), 60000000L, "timeout text -> default");
    // soft teardown: failures 1 and 2 retry, the third goes on
    eq(SoftCloseRetry(1), 1, "retry 1"); eq(SoftCloseRetry(2), 1, "retry 2"); eq(SoftCloseRetry(3), 0, "give up at 3"); eq(SoftCloseRetry(9), 0, "give up at 9");
    // stray errors: 199 in a row go on, the 200th ends it
    eq(StrayErrorEndsLink(1), 0, "1 stray"); eq(StrayErrorEndsLink(199), 0, "199 stray"); eq(StrayErrorEndsLink(200), 1, "200 stray");
    // source port
    eq(PortKept(false, 19021, 19021), 1, "port ok"); eq(PortKept(false, 19022, 19021), 0, "port odd dropped as in 1.2");
    eq(PortKept(true, 19022, 19021), 1, "port odd kept with the fix"); eq(PortKept(true, 0, 19021), 1, "port 0 kept with the fix");
    // MAC verdicts
    const uint8_t peers[12] = {0x02, 1, 2, 3, 4, 5, 0x06, 7, 8, 9, 10, 11};
    const uint8_t a[6] = {0x02, 1, 2, 3, 4, 5}, b[6] = {0x03, 1, 2, 3, 4, 5}, c[6] = {9, 9, 9, 9, 9, 9}, z[6] = {0, 0, 0, 0, 0, 0};
    eq(MacVerdict(a, peers, 2), 0, "mac in list"); eq(MacVerdict(b, peers, 2), 2, "first byte differs"); eq(MacVerdict(c, peers, 2), 3, "not in list");
    eq(MacVerdict(z, peers, 2), 1, "all zero"); eq(MacVerdict(a, peers, 0), 3, "empty list"); eq(MacVerdict(peers + 6, peers, 2), 0, "second peer");
    std::printf(bad ? "galaxy16_test: %d FAIL\n" : "galaxy16_test: all ok\n", bad);
    return bad ? 1 : 0;
}
