// Host test for the conversion-point switch in include/port/endian.h and chunk_endian.h.
//   g++ -O1 -I<np>/include -o /tmp/dm1 tests/disc_mode_test.cpp && /tmp/dm1
//   g++ -O1 -DNP_DISC_HOST_ORDER -I<np>/include -o /tmp/dm2 tests/disc_mode_test.cpp && /tmp/dm2
// Default mode: parsers see big-endian bytes and swap in place. NP_DISC_HOST_ORDER (NPA's load hook
// already converted the buffer): port_disc* read host order and the swaps do nothing, so running the
// parser on converted data does not swap twice.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "port/chunk_endian.h"
static int fails;
#define EXPECT(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
int main()
{
    // Disc image: container 0x80017000 (size 16) holding a leaf 0x00017001 (size 4, one word 0x01020304).
    const uint8_t disc[] = { 0x80,0x01,0x70,0x00, 0,0,0,16,  0x00,0x01,0x70,0x01, 0,0,0,4,  1,2,3,4 };
    uint8_t buf[sizeof disc];
    memcpy(buf, disc, sizeof buf);
#ifdef NP_DISC_HOST_ORDER
    // what the load hook would have produced: headers and the payload word in host order
    uint32_t w;
    w = 0x80017000u; memcpy(buf, &w, 4);
    w = 16;          memcpy(buf + 4, &w, 4);
    w = 0x00017001u; memcpy(buf + 8, &w, 4);
    w = 4;           memcpy(buf + 12, &w, 4);
    w = 0x01020304u; memcpy(buf + 16, &w, 4);
    EXPECT(PORT_DISC_IS_BE == 0);
#else
    EXPECT(PORT_DISC_IS_BE == 1);
#endif
    // The parser's sequence: read the top header, convert the tree, convert the leaf word.
    EXPECT(port_disc32(buf) == 0x80017000u);
    EXPECT(port_disc32(buf + 4) == 16u);
    EXPECT(port_disc_chunk_swap_tree(buf, sizeof buf) != 0);
    port_disc32_array(buf + 16, 1);
    uint32_t id, size, leaf;
    memcpy(&id, buf, 4); memcpy(&size, buf + 4, 4); memcpy(&leaf, buf + 16, 4);
    EXPECT(id == 0x80017000u); EXPECT(size == 16u); EXPECT(leaf == 0x01020304u);
    // a second pass over the converted buffer in host mode must be a no-op
#ifdef NP_DISC_HOST_ORDER
    port_disc32_array(buf + 16, 1);
    memcpy(&leaf, buf + 16, 4);
    EXPECT(leaf == 0x01020304u);
#endif
    // half-word and float reads
    const uint8_t f[] = { 0x3f,0x80,0,0 };  // 1.0f big-endian
    uint8_t g[4]; memcpy(g, f, 4);
#ifdef NP_DISC_HOST_ORDER
    float one = 1.0f; memcpy(g, &one, 4);
#endif
    EXPECT(port_discf32(g) == 1.0f);
    if (fails)
        printf("disc_mode_test: %d FAILED\n", fails);
    else
        printf("disc_mode_test: ok (%s)\n", PORT_DISC_IS_BE ? "big-endian parse-time" : "host-order load-time");
    return fails != 0;
}
