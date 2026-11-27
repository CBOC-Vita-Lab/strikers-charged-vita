// The lobby's connection messages against sizes and counts the wire made up: a message that claims more than its fields hold is refused (mFailed; MessageRegistry::Dispatch
// then discards it), nothing is written past the object, and well-formed messages read back. Run under AddressSanitizer: the objects are on the heap, as the factory makes them.
//   g++ -fsanitize=address,undefined ... tools/net/lan_msg_test.cpp src/Game/LANConnectionMessages.cpp src/Game/NetworkMessageSerializer.cpp
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/LANMessages.h"
#include "Game/NetworkMessageSerializer.h"

void* NetworkMessage::operator new(unsigned long size, unsigned int, bool) { return malloc(size); }   // (plain heap: AddressSanitizer sees every object's end)
void NetworkMessage::operator delete(void* p) { free(p); }

static int g_failed = 0, g_checks = 0;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_failed; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// JoinRequest: address 4, port 2, name 11, size 1, data
static void JoinRequest(int size, bool refused) {
    u8 wire[300];
    memset(wire, 0x5A, sizeof wire);
    wire[17] = u8(size);
    NetMessageJoinRequest* m = new NetMessageJoinRequest;
    NetworkMessageSerializer r(NETWORK_SERIALIZE_READ, wire, 18 + size);
    m->Serialize(&r);
    CHECK(r.mFailed == refused);
    CHECK(refused ? m->mUserMatchDataSize == 0 : m->mUserMatchDataSize == size);
    delete m;
}

// GamePeerAdded: address 4, port 2, name 11, index 1, size 1, data
static void PeerAdded(int size, bool refused) {
    u8 wire[300];
    memset(wire, 0x5A, sizeof wire);
    wire[18] = u8(size);
    NetMessageGamePeerAdded* m = new NetMessageGamePeerAdded;
    NetworkMessageSerializer r(NETWORK_SERIALIZE_READ, wire, 19 + size);
    m->Serialize(&r);
    CHECK(r.mFailed == refused);
    delete m;
}

// JoinResponse: address 4, port 2, accepted 1, name 11, size 1, data, count 1, peers (19 + their data)
static void JoinResponse(int size, int count, int peerSize, bool refused) {
    static u8 wire[8192];
    memset(wire, 0x5A, sizeof wire);
    size_t n = 18;
    wire[n++] = u8(size);
    n += size;
    wire[n++] = u8(count);
    for (int i = 0; i < count; ++i) {
        n += 18;
        wire[n++] = u8(peerSize);
        n += peerSize;
    }
    NetMessageJoinResponse* m = new NetMessageJoinResponse;
    NetworkMessageSerializer r(NETWORK_SERIALIZE_READ, wire, n);
    m->Serialize(&r);
    CHECK(r.mFailed == refused);
    if (refused) CHECK(m->mUserMatchDataSize <= 8 && m->mPeerCount <= 6);
    else CHECK(m->mPeerCount == count && m->mUserMatchDataSize == size);
    delete m;
}

int main() {
    for (int s = 0; s <= 8; ++s) { JoinRequest(s, false); PeerAdded(s, false); JoinResponse(s, 2, s, false); }
    for (int s : {9, 16, 40, 255}) { JoinRequest(s, true); PeerAdded(s, true); JoinResponse(s, 0, 0, true); JoinResponse(0, 2, s, true); }
    JoinResponse(0, 6, 8, false);
    for (int c : {7, 8, 40, 255}) JoinResponse(0, c, 8, true);   // mPeerInfoList holds 8: the host's record, up to 6 others, this console
    printf("lan_msg_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
