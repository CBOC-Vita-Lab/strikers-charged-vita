// np_online_save.h: when leaving internet play may save.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_online_save.cpp -o /tmp/tos && /tmp/tos
#include "platform/sdk/net/np_online_save.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    using NpOnlineSave::Wanted;
    CHECK(!Wanted(false, false, false));   // (1) nothing changed: no save (old rule saved)
    CHECK(Wanted(false, true, false));     //     something changed: save
    CHECK(!Wanted(false, true, true));     // (2) ended by error: no save even with changes
    CHECK(!Wanted(false, false, true));
    CHECK(Wanted(true, false, false));     // MSC_ONLINE_SAVE_ALWAYS=1: the old behaviour, also after an error
    CHECK(Wanted(true, false, true));
    NpOnlineSave::Begin();
    CHECK(!NpOnlineSave::ErrorLatch());
    NpOnlineSave::NoteError();
    CHECK(NpOnlineSave::ErrorLatch());
    NpOnlineSave::Begin();                 // a new session starts clean
    CHECK(!NpOnlineSave::ErrorLatch());
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
