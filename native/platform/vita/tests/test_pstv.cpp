// The PS TV's pad-only defaults (1.2, vita_plan.cpp Apply): the touch switches are set when the console is a PS TV (MSC_PSTV=1 stands in for the system's answer
// on the host), never over a line the owner's np-player.txt put in, and not at all on a handheld. Host test.
#include "np_vita_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char* const kNames[] = {"NP_TOUCH_POINTER", "MSC_TOUCH_MINIGAMES", "MSC_REAR_TOUCH"};
static void Clear() {
    for (const char* n : kNames) unsetenv(n);
    unsetenv("MSC_PSTV");
}
static bool Is(const char* name, const char* value) {
    const char* e = std::getenv(name);
    return value ? (e && !std::strcmp(e, value)) : !e;
}

int main() {
    // a handheld: nothing is set
    Clear();
    setenv("MSC_PSTV", "0", 1);
    CHECK(!VitaPlan::IsPsTv());
    VitaPlan::Apply(nullptr, 0);
    for (const char* n : kNames) CHECK(Is(n, nullptr));

    // a PS TV: the three pad-only defaults
    Clear();
    setenv("MSC_PSTV", "1", 1);
    CHECK(VitaPlan::IsPsTv());
    VitaPlan::Apply(nullptr, 0);
    CHECK(Is("NP_TOUCH_POINTER", "0") && Is("MSC_TOUCH_MINIGAMES", "0") && Is("MSC_REAR_TOUCH", "0"));

    // a line of the owner's wins, for that one switch only
    Clear();
    setenv("MSC_PSTV", "1", 1);
    setenv("MSC_REAR_TOUCH", "1", 1);
    VitaPlan::Apply(nullptr, 0);
    CHECK(Is("MSC_REAR_TOUCH", "1") && Is("NP_TOUCH_POINTER", "0") && Is("MSC_TOUCH_MINIGAMES", "0"));

    // a second application (the settings are read again) changes nothing
    VitaPlan::Apply(nullptr, 0);
    CHECK(Is("MSC_REAR_TOUCH", "1") && Is("NP_TOUCH_POINTER", "0"));
    Clear();
    std::printf("PS TV defaults: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
