// Unit tests of the W5 CTL controls (platform/sdk/input/np_controls.{h,cpp}): the MODERN and CLASSIC layouts as the engine's
// GameCube bits, the Wii Remote view, the context buttons, the rear touchpad's taps, the tap latch; and the SDK side
// (input_native.cpp): a Vita-button script and a GameCube-pad script reach the raw sample with their source, the rear fingers
// are carried, NpPad_ReadRaw returns the sample the GameCube view was made from.
#include <revolution/os.h>
#include <revolution/pad.h>
#include <revolution/wpad.h>

#include <stdio.h>
#include <string.h>

#include "np/np_pad.h"
#include "np/np_sdk.h"

#include "../input/np_controls.cpp"
#include "../input/np_control_table.h"

using namespace NpControls;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

// ---- a scripted pad for the SDK part (np_harness.h NpHarnessRaw / NpPadState layouts) ----
struct HarnessRaw {
    uint32_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    int touchCount;
    struct { int id; float x, y; } touch[4];
    int rearCount;
    struct { int id; float x, y; } rear[4];
};
struct HarnessPad {
    uint16_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerL, triggerR;
};
static int g_vitaScript = 0, g_gcScript = 0;
static HarnessRaw g_raw;
static uint16_t g_gcButtons;
extern "C" int NpHarnessFillRaw(HarnessRaw* raw) {
    if (!g_vitaScript) return 0;
    *raw = g_raw;
    return 1;
}
extern "C" int NpHarnessFillPad(HarnessPad* pad) {
    if (!g_gcScript) return 0;
    pad->buttons = g_gcButtons;
    return 1;
}
static NpRawPad g_fake;
static int src(int pad, NpRawPad* out) {
    if (pad != 0) return 0;
    *out = g_fake;
    return 1;
}
static void settle() { OSSleepTicks(OSMillisecondsToTicks(5)); }   // past the 3 ms sample cache

static uint16_t map1(Mapper& m, uint32_t vita, bool have, bool shake = false) {
    Context c;
    c.possession = have;
    c.shakedown = shake;
    return m.Map(vita, 0, 0, c);
}

int main() {
    // ---- CLASSIC: by position, every button
    CHECK(ClassicGc(kCross) == kGcA);
    CHECK(ClassicGc(kSquare) == kGcB);
    CHECK(ClassicGc(kCircle) == kGcX);
    CHECK(ClassicGc(kTriangle) == kGcY);
    CHECK(ClassicGc(kL) == kGcL);
    CHECK(ClassicGc(kR) == kGcR);
    CHECK(ClassicGc(kSelect) == kGcZ);
    CHECK(ClassicGc(kStart) == kGcStart);
    CHECK(ClassicGc(kUp | kDown | kLeft | kRight) == (kGcUp | kGcDown | kGcLeft | kGcRight));

    // ---- MODERN: every action in both contexts
    {
        Mapper m;
        CHECK(map1(m, kCross, true) == kGcA);            // pass
        CHECK(map1(m, 0, true) == 0);
        CHECK(map1(m, kCross, false) == kGcA);           // switch player
        CHECK(map1(m, 0, false) == 0);
        CHECK(map1(m, kCircle, true) == kGcB);           // shoot
        CHECK(map1(m, kCircle, false) == kGcB);          // slide tackle
        CHECK(map1(m, 0, false) == 0);
        CHECK(map1(m, kSquare, true) == (kGcL | kGcA));  // lob pass
        CHECK(map1(m, kSquare, false) == (kGcL | kGcA)); // ... kept while held, even if the ball is lost meanwhile
        CHECK(map1(m, 0, false) == 0);
        CHECK(map1(m, kSquare, false) == kGcY);          // big hit
        CHECK(map1(m, kSquare, true) == kGcY);           // ... kept while held
        CHECK(map1(m, 0, true) == 0);
        CHECK(map1(m, kTriangle, true) == kGcX);         // use item
        CHECK(map1(m, 0, true) == 0);
        CHECK(map1(m, kR, true) == kGcL);                // turbo / modifier
        CHECK(map1(m, kR | kCross, true) == (kGcL | kGcA));    // R + Cross: lob pass
        CHECK(map1(m, kR | kCircle, true) == (kGcL | kGcB));   // R + Circle: chip shot
        CHECK(map1(m, 0, true) == 0);
        CHECK(map1(m, kL, true) == kGcL);                // L with the ball: modifier
        CHECK(map1(m, kL, false) == kGcL);               // ... kept while held
        CHECK(map1(m, 0, false) == 0);
        CHECK(map1(m, kL, false) == kGcA);               // L without the ball: switch player
        CHECK(map1(m, kL, true) == kGcA);                // ... kept (no stray modifier release = no chip ahead)
        CHECK(map1(m, 0, true) == 0);
        CHECK(map1(m, kSelect, true) == kGcZ);           // cycle item
        CHECK(map1(m, kStart, true) == kGcStart);
        CHECK(map1(m, kUp | kLeft, false) == (kGcUp | kGcLeft));   // D-pad: deke / slide
        CHECK(map1(m, 0, false) == 0);
        // shakedown context: Square and Triangle are Z (mash), Select too
        CHECK(map1(m, kSquare, false, true) == kGcZ);
        CHECK(map1(m, 0, false, true) == 0);
        CHECK(map1(m, kTriangle, false, true) == kGcZ);
        CHECK(map1(m, 0, false, true) == 0);
        CHECK(map1(m, kSelect, false, true) == kGcZ);
        // R never reaches the engine as GameCube R in MODERN (the CLASSIC special)
        for (uint32_t b = 0; b < 0x10000; b += 0x0101) CHECK((map1(m, b, (b >> 3) & 1) & kGcR) == 0);
    }
    // ---- rear taps through the mapper: pulses of two captures, a gap, never a pass
    {
        Mapper m;
        Context have = {true, false}, none = {false, false};
        int right = 1, left = 0;
        CHECK(m.Map(0, &right, &left, none) == kGcA);
        CHECK(right == 0);
        CHECK(m.Map(0, &right, &left, none) == kGcA);
        CHECK(m.Map(0, &right, &left, none) == 0);
        right = 2;
        CHECK(m.Map(0, &right, &left, none) == kGcA);    // the gap is over: next pulse
        CHECK(m.Map(0, &right, &left, none) == kGcA);
        CHECK(m.Map(0, &right, &left, none) == 0);       // the gap
        CHECK(m.Map(0, &right, &left, none) == kGcA);    // the second tap
        CHECK(m.Map(0, &right, &left, none) == kGcA);
        CHECK(m.Map(0, &right, &left, none) == 0);
        CHECK(right == 0);
        right = 1;
        CHECK(m.Map(0, &right, &left, have) == 0);       // with the ball: dropped
        CHECK(right == 0);
        CHECK(m.Map(0, &right, &left, have) == 0);
        left = 3;
        int pulses = 0;
        uint16_t prev = 0;
        for (int i = 0; i < 12; ++i) {
            const uint16_t g = m.Map(0, &right, &left, have);
            if ((g & kGcZ) && !(prev & kGcZ)) ++pulses;
            prev = g;
        }
        CHECK(pulses == 3);                               // three taps = three Z presses (three shakes)
        CHECK(left == 0);
    }
    // ---- the Wii Remote view
    {
        const uint32_t base = 0x80;   // the controller tag survives
        CHECK(WiiHold(kClassic, kSquare | kCircle, base | kWB | kWC, false) == (base | kWB | kWC));
        CHECK(WiiHold(kModern, kCircle, base | kWC, false) == (base | kWB));          // Circle = B (back, the meter's shoot)
        CHECK(WiiHold(kModern, kSquare, base | kWB, false) == (base | kWC));          // Square = C
        CHECK(WiiHold(kModern, kCross, base | kWA, false) == (base | kWA));           // Cross = A
        CHECK(WiiHold(kModern, kSquare, base | kWB, true) == (base | kW2));           // bare remote: Square = 2
        CHECK(WiiHold(kModern, 0, base | 0x0200, false) == (base | 0x0200));          // others untouched
    }
    // ---- the rear touchpad
    {
        RearPad r;
        RearPoint p;
        // a tap on the right half
        p = {1, 700, 270};
        CHECK(r.Update(&p, 1, 1000) == 0);
        CHECK(r.Update(&p, 1, 1050) == 0);
        CHECK(r.Update(&p, 0, 1100) == 2u);
        // a tap on the left half
        p = {2, 200, 300};
        CHECK(r.Update(&p, 1, 2000) == 0);
        CHECK(r.Update(&p, 0, 2120) == 1u);
        // a resting finger: never (also when it lifts later)
        p = {3, 700, 270};
        unsigned ev = 0;
        for (uint32_t t = 3000; t < 5000; t += 33) ev |= r.Update(&p, 1, t);
        ev |= r.Update(&p, 0, 5000);
        CHECK(ev == 0);
        // the edge margin and the middle strip
        p = {4, 20, 270};
        r.Update(&p, 1, 6000);
        CHECK(r.Update(&p, 0, 6050) == 0);
        p = {5, 480, 270};
        r.Update(&p, 1, 6100);
        CHECK(r.Update(&p, 0, 6150) == 0);
        p = {6, 700, 10};
        r.Update(&p, 1, 6200);
        CHECK(r.Update(&p, 0, 6250) == 0);
        // a grip: three fingers on the back spoil each of them
        RearPoint g[3] = {{7, 200, 300}, {8, 700, 300}, {9, 300, 200}};
        r.Update(g, 3, 7000);
        CHECK(r.Update(g, 2, 7050) == 0);   // one lifted: was part of the grip
        CHECK(r.Update(g, 0, 7100) == 0);
        // a finger that moves is not a tap; on the left a swipe counts at once (shakedown), once
        p = {10, 600, 270};
        r.Update(&p, 1, 8000);
        p.x = 700;
        CHECK(r.Update(&p, 1, 8050) == 0);
        CHECK(r.Update(&p, 0, 8100) == 0);
        p = {11, 150, 270};
        CHECK(r.Update(&p, 1, 9000) == 0);
        p.x = 210;
        CHECK(r.Update(&p, 1, 9050) == 1u);
        p.x = 300;
        CHECK(r.Update(&p, 1, 9100) == 0);
        CHECK(r.Update(&p, 0, 9150) == 0);
        // a second finger tapping while one rests: the tap counts
        RearPoint two[2] = {{12, 200, 300}, {13, 700, 300}};
        r.Update(two, 1, 10000);                  // the resting one
        r.Update(two, 2, 11000);                  // the tap lands
        CHECK(r.Update(two, 1, 11080) == 2u);     // and lifts
        CHECK(r.Update(two, 0, 12000) == 0);      // the resting one lifts: no tap
        // a slow press is not a tap
        p = {14, 700, 300};
        r.Update(&p, 1, 13000);
        CHECK(r.Update(&p, 0, 13000 + RearPad::kTapMs + 1) == 0);
        // NaN never counts
        p = {15, 0.0f / 0.0f, 300};
        r.Update(&p, 1, 14000);
        CHECK(r.Update(&p, 0, 14050) == 0);
    }
    // ---- the tap latch
    {
        uint64_t last = 0;
        uint32_t b[4] = {0, kCross, 0, 0};
        uint64_t t[4] = {100, 116, 133, 150};
        CHECK(LatchTaps(b, t, 4, &last) == 0);           // the first read: only the newest
        CHECK(last == 150);
        uint32_t b2[4] = {kCross, 0, kSquare, kSquare};
        uint64_t t2[4] = {133, 150, 166, 183};
        CHECK(LatchTaps(b2, t2, 4, &last) == kSquare);   // Cross was in an old sample (before 150): not again
        uint32_t b3[4] = {kSquare, kCircle, 0, 0};
        uint64_t t3[4] = {183, 200, 216, 233};
        CHECK(LatchTaps(b3, t3, 4, &last) == kCircle);   // Circle down and up between two reads: reported once
        uint32_t b4[4] = {kCircle, 0, 0, kR};
        uint64_t t4[4] = {200, 250, 266, 283};
        CHECK(LatchTaps(b4, t4, 4, &last) == kR);        // not twice
        uint32_t b5[2] = {kL, 0};
        uint64_t t5[2] = {300, 290};                     // any order
        CHECK(LatchTaps(b5, t5, 2, &last) == kL);
    }

    // ---- the SDK: sources, rear fingers, the raw view of the sample
    {
        NpSdkConfig cfg;
        NpSdk_ConfigDefaults(&cfg);
        NpSdk_Init(&cfg);
        NpSdk_SetInputSource(src);
        WPADInit();
        memset(&g_fake, 0, sizeof g_fake);
        g_fake.lx = g_fake.ly = g_fake.rx = g_fake.ry = 128;
        g_fake.buttons = NP_BTN_CIRCLE;
        g_fake.rear_count = 1;
        g_fake.rear[0].id = 5;
        g_fake.rear[0].x = 700;
        g_fake.rear[0].y = 300;
        settle();
        NpRawPad raw;
        PADStatus pad;
        CHECK(NpPad_ReadRaw(0, &raw) == 1);
        CHECK(raw.source == NP_RAW_LIVE);
        CHECK(raw.buttons == NP_BTN_CIRCLE);
        CHECK(raw.rear_count == 1 && raw.rear[0].id == 5 && raw.rear[0].x == 700.0f);
        CHECK(NpPad_ReadGameCube(0, &pad) == 1 && pad.button == PAD_BUTTON_X);   // the GameCube view is by position (CLASSIC)
        // a Vita-button script replaces the controls, fingers included
        g_vitaScript = 1;
        memset(&g_raw, 0, sizeof g_raw);
        g_raw.buttons = NP_BTN_SQUARE | NP_BTN_R;
        g_raw.stickX = 72;
        g_raw.rearCount = 1;
        g_raw.rear[0].id = 9;
        g_raw.rear[0].x = 200;
        g_raw.rear[0].y = 250;
        g_raw.touchCount = 1;
        g_raw.touch[0].id = 3;
        g_raw.touch[0].x = 480;
        g_raw.touch[0].y = 272;
        settle();
        CHECK(NpPad_ReadRaw(0, &raw) == 1);
        CHECK(raw.source == NP_RAW_SCRIPT_VITA);
        CHECK(raw.buttons == (NP_BTN_SQUARE | NP_BTN_R));
        CHECK(raw.lx > 220 && raw.ly == 128);
        CHECK(raw.rear_count == 1 && raw.rear[0].id == 9 && raw.rear[0].x == 200.0f);
        CHECK(raw.touch_count == 1 && raw.touch[0].x == 480.0f);
        g_vitaScript = 0;
        // a GameCube-pad script: by position, no fingers, marked as such
        g_gcScript = 1;
        g_gcButtons = PAD_BUTTON_B | PAD_TRIGGER_Z;
        settle();
        CHECK(NpPad_ReadRaw(0, &raw) == 1);
        CHECK(raw.source == NP_RAW_SCRIPT_GC);
        CHECK(raw.buttons == (NP_BTN_SQUARE | NP_BTN_SELECT));
        CHECK(raw.rear_count == 0 && raw.touch_count == 0);
        CHECK(NpPad_ReadGameCube(0, &pad) == 1 && pad.button == (PAD_BUTTON_B | PAD_TRIGGER_Z));
        g_gcScript = 0;
        settle();
        CHECK(NpPad_ReadRaw(0, &raw) == 1 && raw.source == NP_RAW_LIVE);
    }

    // ---- the controls table (np_control_table.h): every line's input gives the engine bits the line claims, in its context, through
    // the mapping above; the texts fit the Settings page; the roles the prompts and pictures use name the same buttons as the lines.
    {
        using namespace NpControlTable;
        // the SDK's Wii Remote view by position (input_native.cpp emu_sample, the Nunchuk profile)
        auto classicWii = [](uint32_t v) {
            uint32_t w = 0;
            if (v & kCross) w |= 0x0800;      // A
            if (v & kSquare) w |= 0x0400;     // B
            if (v & kCircle) w |= 0x4000;     // C
            if (v & kL) w |= 0x2000;          // Z
            if (v & kStart) w |= 0x0200;      // 1
            if (v & kSelect) w |= 0x1000;     // -
            if (v & kTriangle) w |= 0x0010;   // +
            return w;
        };
        int tested = 0, scriptOnly = 0;
        for (int i = 0; i < LineCount(); ++i) {
            const Line& l = LineAt(i);
            CHECK(l.input && *l.input);
            if (!IsSentence(l)) {
                CHECK(strlen(l.input) <= 16);   // the Settings page's first column
                CHECK(l.withBall && l.withoutBall && strlen(l.withBall) <= 29 && strlen(l.withoutBall) <= 29);
            } else {
                char buf[160];
                Config all = {0, true, true, true, true, true};
                CHECK(strlen(l.input) + 2 + strlen(Sentence(l, all, buf, sizeof buf)) <= 76);
                all.layout = 1;
                CHECK(strlen(l.input) + 2 + strlen(Sentence(l, all, buf, sizeof buf)) <= 76);
            }
            CHECK(l.tests[0].drive != 0);   // every line has a check
            for (int t = 0; t < 6 && l.tests[t].drive; ++t) {
                const Test& x = l.tests[t];
                CHECK((x.layouts & ~l.layouts) == 0);
                CHECK(x.counter && *x.counter);
                for (int layout = 0; layout < 2; ++layout) {
                    if (!(x.layouts & LayoutBit(layout))) continue;
                    if (x.expect == kScriptOnly) {
                        ++scriptOnly;
                        continue;
                    }
                    ++tested;
                    if (x.expect == kWii) {
                        const uint32_t w = WiiHold(layout == 1 ? kModern : kClassic, x.vita, classicWii(x.vita), false);
                        if ((w & x.bits) != x.bits) printf("  line %d (%s) test %d: Wii view 0x%04x, the line says 0x%04x\n", i, l.input, t, w, x.bits);
                        CHECK((w & x.bits) == x.bits);
                        continue;
                    }
                    const bool have = x.ctx == kAttack || x.ctx == kCaptain;
                    uint16_t gc;
                    if (layout == 1) {
                        Mapper m;
                        Context c;
                        c.possession = have;
                        c.shakedown = x.ctx == kShake;
                        int right = x.rearTap == 2 ? 1 : 0, left = x.rearTap == 1 ? 1 : 0;
                        gc = m.Map(x.vita, &right, &left, c);
                    } else {
                        gc = ClassicGc(x.vita);
                        if (x.rearTap) {   // CLASSIC with the rear touchpad on: only the taps go through the mapper
                            Mapper m;
                            Context c;
                            c.possession = have;
                            c.shakedown = x.ctx == kShake;
                            int right = x.rearTap == 2 ? 1 : 0, left = x.rearTap == 1 ? 1 : 0;
                            gc |= m.Map(0, &right, &left, c) & (kGcA | kGcZ);
                        }
                    }
                    if (gc != x.bits) printf("  line %d (%s) test %d %s: engine bits 0x%04x, the line says 0x%04x\n", i, l.input, t, layout ? "MODERN" : "CLASSIC", gc, x.bits);
                    CHECK(gc == x.bits);
                }
            }
        }
        CHECK(tested > 40 && scriptOnly > 0);
        // the roles name the buttons of the lines (prompts and pictures say what the controls page says)
        for (int layout = 0; layout < 2; ++layout) {
            const Layout L = layout ? kModern : kClassic;
            Config cfg = {layout, true, true, true, true, true};
            auto lineFor = [&](const char* name) {
                for (int i = 0; i < LineCount(); ++i)
                    if (Shown(LineAt(i), cfg) && !strcmp(LineAt(i).input, name)) return &LineAt(i);
                return (const Line*)0;
            };
            const Line* a = lineFor(RoleName(layout, kRoleA));
            CHECK(a && !strcmp(a->withBall, "PASS"));
            const Line* b = lineFor(RoleName(layout, kRoleB));
            CHECK(b && !strncmp(b->withBall, "SHOOT", 5));
            const Line* c = lineFor(RoleName(layout, kRoleC));
            CHECK(c && strstr(c->withBall, "ITEM"));
            const Line* z = lineFor(RoleName(layout, kRoleZ));
            CHECK(z && strstr(z->withBall, "CHIP AHEAD"));
            const Line* hit = lineFor(RoleName(layout, kRoleShake));
            CHECK(hit && !strcmp(hit->withoutBall, "BIG HIT"));
            const Line* flick = lineFor(RoleName(layout, kRoleFlick));
            CHECK(flick && strstr(flick->withBall, "ITEM"));
            const Line* one = lineFor(RoleName(layout, kRoleOne));
            CHECK(one && !strcmp(one->withBall, "PAUSE"));
            CHECK(lineFor(RoleName(layout, kRoleDpad)) != 0);
            // and the roles' buttons do what the original's control did
            Context have = {true, false}, without = {false, false};
            Mapper m;
            const uint32_t A = RoleButtons(layout, kRoleA), B = RoleButtons(layout, kRoleB), C = RoleButtons(layout, kRoleC);
            const uint32_t Z = RoleButtons(layout, kRoleZ), S = RoleButtons(layout, kRoleShake), F = RoleButtons(layout, kRoleFlick);
            auto gcOf = [&](uint32_t v, const Context& cx) {
                if (layout == 0) return ClassicGc(v);
                m.Reset();
                return m.Map(v, 0, 0, cx);
            };
            CHECK(gcOf(A, have) == kGcA);
            CHECK(gcOf(B, have) == kGcB);
            CHECK(gcOf(C, have) == kGcX);
            CHECK(gcOf(Z, have) == kGcL);
            CHECK(gcOf(S, without) == kGcY);
            CHECK(gcOf(F, have) == kGcZ);
            CHECK(gcOf(RoleButtons(layout, kRoleOne), have) == kGcStart);
            CHECK((WiiHold(L, A, classicWii(A), false) & 0x0800) && (WiiHold(L, B, classicWii(B), false) & 0x0400));
            CHECK(RoleSymbol(layout, kRoleA) == 0 && RoleSymbol(layout, kRoleB) == (layout ? 1 : 2));
        }
    }

    printf("test_controls: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
