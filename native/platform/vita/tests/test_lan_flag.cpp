// nxr6b: NpLanFlag, the published "a session exists" flag the render thread reads instead of following g_pNetworkSession (NEON round 6 finding 2).
// A reader thread must only ever see a value the writer published (0 or 1, never torn), must see the last one after the writer is done, and nothing is read through a pointer.
#include "platform/sdk/net/np_lan_flag.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    assert(!NpLanFlag::Read());                 // before any session: off
    NpLanFlag::Publish(true);
    assert(NpLanFlag::Read());
    NpLanFlag::Publish(false);
    assert(!NpLanFlag::Read());
    std::atomic<bool> stop{false};
    std::atomic<long> bad{0}, ones{0};
    std::thread reader([&] { while (!stop.load()) { const int v = __atomic_load_n(NpLanFlag::Cell(), __ATOMIC_ACQUIRE); if (v != 0 && v != 1) ++bad; if (v == 1) ++ones; } });
    for (int i = 0; i < 200000; ++i) NpLanFlag::Publish((i & 1) == 0);
    NpLanFlag::Publish(true);
    stop.store(true);
    reader.join();
    assert(bad.load() == 0);
    assert(NpLanFlag::Read());                  // the last published value is what a reader sees
    NpLanFlag::Publish(false);
    assert(!NpLanFlag::Read());
    std::printf("test_lan_flag: ok\n");
    return 0;
}
