// Host check of platform/vita/src/np_picture_presets.h: g++ -std=c++17 -I platform/vita/src tools/look/preset_test.cpp -o /tmp/preset_test && /tmp/preset_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include "np_picture_presets.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    std::map<std::string, std::string> env;
    auto isSet = [&](const char* n) { return env.count(n) != 0; };
    auto set = [&](const char* n, const char* v) { env[n] = v; };
    CHECK(np_picture::Expand("quality", isSet, set) == 1 && env["MSC_VITA_RENDER"] == "960x544");
    env.clear();
    CHECK(np_picture::Expand("performance", isSet, set) == 2 && env["MSC_VITA_RENDER"] == "640x362" && env["MSC_PERFORMANCE_MODE"] == "1");
    env.clear();
    env["MSC_VITA_RENDER"] = "800x448"; // a key the file sets itself wins
    CHECK(np_picture::Expand("performance", isSet, set) == 1 && env["MSC_VITA_RENDER"] == "800x448" && env["MSC_PERFORMANCE_MODE"] == "1");
    env.clear();
    CHECK(np_picture::Expand("default", isSet, set) == 0 && env.empty());
    CHECK(np_picture::Expand("nonsense", isSet, set) == -1 && env.empty());
    CHECK(np_picture::Expand("", isSet, set) == -1);
    std::printf(fails ? "FAILED\n" : "preset_test ok\n");
    return fails ? 1 : 0;
}
