// np_found_games.h: the lobby's found-game list decisions.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_found_games.cpp -o /tmp/tfg && /tmp/tfg
#include "platform/sdk/net/np_found_games.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Info { char name[12]; unsigned char addr[4]; int type; float since; unsigned short port; };

static NpFound::Verdict D(Info* l, int n, const char* name) {
    return NpFound::Decide(l[0].name, sizeof(Info), &l[0].since, sizeof(Info), n, 10, name, 3.5f);
}

int main() {
    Info l[10];
    memset(l, 0, sizeof l);
    for (int i = 0; i < 10; ++i) { snprintf(l[i].name, sizeof l[i].name, "host%d", i); l[i].since = 0.1f * i; }
    // a known name on a FULL list is refreshed (old rule: dropped, so all ten expired together)
    NpFound::Verdict v = D(l, 10, "host4");
    CHECK(v.action == NpFound::kRefresh && v.index == 4);
    // a new name on a full list with nothing stale: ignored
    v = D(l, 10, "newhost");
    CHECK(v.action == NpFound::kIgnore);
    // ... but when the stalest has missed half its life it makes room
    l[7].since = 2.0f;
    v = D(l, 10, "newhost");
    CHECK(v.action == NpFound::kReplace && v.index == 7);
    // a list with room appends at the end
    v = D(l, 6, "newhost");
    CHECK(v.action == NpFound::kAppend && v.index == 6);
    v = D(l, 0, "x");
    CHECK(v.action == NpFound::kAppend && v.index == 0);
    // names are compared on the 11 characters the list keeps
    Info m[1];
    memset(m, 0, sizeof m);
    memcpy(m[0].name, "abcdefghijk", 11);
    v = NpFound::Decide(m[0].name, sizeof(Info), &m[0].since, sizeof(Info), 1, 10, "abcdefghijkXYZ", 3.5f);
    CHECK(v.action == NpFound::kRefresh);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
