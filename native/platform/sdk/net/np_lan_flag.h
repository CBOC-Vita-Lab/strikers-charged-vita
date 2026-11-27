// nxr6b (NEON round 6 finding 2): "a LAN or online session exists" as a flag another core may read.
// NpLanActive() reads g_pNetworkSession, a plain pointer the game thread (core 0) creates and tears down; the render thread (core 1, MSC_LOCK60_NET_HOLD) must not
// dereference it. The game thread publishes the answer here every time it asks (NpLanActive() is called on it every frame by np_fps and the input code); any thread reads
// the last published value. A value one frame old is harmless for a hold that only stops steps UP.
#pragma once
// (compiler builtins, no <atomic>: this header is included ahead of the game's own operator new (nlMemory.h), which <new> would clash with)

struct NpLanFlag {
    static int* Cell() { static int v = 0; return &v; }
    static void Publish(bool active) { __atomic_store_n(Cell(), active ? 1 : 0, __ATOMIC_RELEASE); }
    static bool Read() { return __atomic_load_n(Cell(), __ATOMIC_ACQUIRE) != 0; }
};
