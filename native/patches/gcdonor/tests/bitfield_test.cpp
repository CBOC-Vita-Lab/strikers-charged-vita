// Host test for include/port/bitfield.h and the reordered unions (patches/gcdonor/include/...). The unions
// are repeated here as in the patched headers; expected positions are what Metrowerks (first declared =
// most significant) gives and what the game's raw masks assume.
//   g++ -O1 -I<np>/include -o /tmp/bf tests/bitfield_test.cpp && /tmp/bf
#include <stdio.h>
#include <stdint.h>
#include "port/bitfield.h"
typedef uint32_t u32; typedef int32_t s32;
union RenderSnapshotFlags { u32 raw; struct {
#if NP_BITFIELDS_LSB_FIRST
 u32 reserved:22; u32 thwomps:1; u32 windDebris:1; u32 diddyBanana:1; u32 daisyFists:1; u32 birdoEgg:1; u32 koopaShell:1; u32 bulletBills:1; u32 hammers:1; u32 flyingCameras:1; u32 yoshiEgg:1;
#else
 u32 yoshiEgg:1; u32 flyingCameras:1; u32 hammers:1; u32 bulletBills:1; u32 koopaShell:1; u32 birdoEgg:1; u32 daisyFists:1; u32 diddyBanana:1; u32 windDebris:1; u32 thwomps:1; u32 reserved:22;
#endif
} bits; };
union DrawableBallFlags { u32 value; struct {
#if NP_BITFIELDS_LSB_FIRST
 u32 unused:7; u32 passTargetIndex:5; u32 previousOwnerIndex:5; u32 ownerIndex:5; u32 lastTouchIndex:5; u32 transient:4; u32 visible:1;
#else
 u32 visible:1; u32 transient:4; u32 lastTouchIndex:5; u32 ownerIndex:5; u32 previousOwnerIndex:5; u32 passTargetIndex:5; u32 unused:7;
#endif
} bits; };
union XS { u32 w; struct {
#if NP_BITFIELDS_LSB_FIRST
 u32 field_0FFF:12; u32 field_1000:1; u32 zeroVelocity:1; u32 updatePending:1; u32 positionIsPointer:1; s32 references:16;
#else
 s32 references:16; u32 positionIsPointer:1; u32 updatePending:1; u32 zeroVelocity:1; u32 field_1000:1; u32 field_0FFF:12;
#endif
} count; };
union SP { u32 w; struct {
#if NP_BITFIELDS_LSB_FIRST
 u32 m_UnknownFlags09:23; u32 m_SquareRootPan:1; u32 m_InterauralDelay:8;
#else
 u32 m_InterauralDelay:8; u32 m_SquareRootPan:1; u32 m_UnknownFlags09:23;
#endif
} s; };
static int fails;
#define EXPECT(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
int main()
{
    RenderSnapshotFlags f;
    f.raw = 0; f.bits.yoshiEgg = 1; EXPECT(f.raw == 0x80000000u);
    f.raw = 0; f.bits.flyingCameras = 1; EXPECT(f.raw == 0x40000000u);
    f.raw = 0; f.bits.hammers = 1; EXPECT(f.raw == 0x20000000u);
    f.raw = 0; f.bits.bulletBills = 1; EXPECT(f.raw == 0x10000000u);
    f.raw = 0; f.bits.koopaShell = 1; EXPECT(f.raw == 0x08000000u);
    f.raw = 0; f.bits.birdoEgg = 1; EXPECT(f.raw == 0x04000000u);
    f.raw = 0; f.bits.daisyFists = 1; EXPECT(f.raw == 0x02000000u);
    f.raw = 0; f.bits.diddyBanana = 1; EXPECT(f.raw == 0x01000000u);
    f.raw = 0; f.bits.windDebris = 1; EXPECT(f.raw == 0x00800000u);
    f.raw = 0; f.bits.thwomps = 1; EXPECT(f.raw == 0x00400000u);
    DrawableBallFlags d; d.value = 0; d.bits.visible = 1; EXPECT(d.value == 0x80000000u);
    d.value = 0; d.bits.ownerIndex = 1; EXPECT(d.value == (1u << 17));
    d.value = 0; d.bits.passTargetIndex = 1; EXPECT(d.value == (1u << 7));
    XS x; x.w = 0; x.count.positionIsPointer = 1; EXPECT(x.w == 0x8000u);
    x.w = 0; x.count.field_1000 = 1; EXPECT(((x.w >> 12) & 1) == 1);
    x.w = 0; x.count.references = 3; EXPECT(x.w == 0x00030000u);
    SP s; s.w = 0; s.s.m_SquareRootPan = 1; EXPECT(s.w == 0x00800000u);
    s.w = 0; s.s.m_InterauralDelay = 5; EXPECT((int)s.w >> 24 == 5);
    printf("bitfield order %s, fails=%d\n", NP_BITFIELDS_LSB_FIRST ? "LSB-first (reversed)" : "MSB-first (as decompiled)", fails);
    return fails != 0;
}
