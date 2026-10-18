// Unit test of the two input hooks: the nav-script harness pad (NpHarnessFillPad: GameCube terms, replaces the
// controller of channel 0 while it presses something) and the game-aware layer (NpPadAdjust).
#include <revolution/kpad.h>
#include <revolution/os.h>
#include <revolution/pad.h>
#include <revolution/wpad.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "np/np_pad.h"
#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

// ---- fake harness -------------------------------------------------------------------------------
struct NpSdkHarnessPad {
    uint16_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerL, triggerR;
};
static int h_active = 0;
static NpSdkHarnessPad h_pad;
static int h_calls = 0;
extern "C" int NpHarnessFillPad(NpSdkHarnessPad* p) {
    h_calls++;
    if (!h_active) return 0;
    *p = h_pad;
    return 1;
}

// ---- fake game-aware layer ----------------------------------------------------------------------
static int a_calls = 0;
static int a_mode = 0;   // 1: press "1", hide the pointer, move the cursor to (0.5, -0.25); 2: the same, pointer shown
static float a_last_dt = 0;
extern "C" void NpPadAdjust(NpPadSample* s) {
    a_calls++;
    a_last_dt = s->dt;
    if (a_mode == 1 || a_mode == 2) {
        s->hold |= 0x0200;
        s->pointer_hidden = a_mode == 1;
        s->cursor[0] = 0.5f;
        s->cursor[1] = -0.25f;
        s->stick[0] = 0.0f;
    }
}

// ---- fake Vita platform layer (NpVitaReadInput, np_vita.h)
struct FakeVitaInput {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    int touchCount;
    struct { int id; float x, y; } touch[8];
};
static int v_present = 0;
static FakeVitaInput v_in;
extern "C" int NpVitaReadInput(FakeVitaInput* out) {
    if (!v_present) return 0;
    *out = v_in;
    return 1;
}

static NpRawPad fake;
static int src(int pad, NpRawPad* out) {
    if (pad != 0) return 0;
    *out = fake;
    return 1;
}
static void* alloc(u32) { return 0; }
static BOOL freefn(void*) { return TRUE; }
static void settle() { OSSleepTicks(OSMillisecondsToTicks(5)); }   // past the 3 ms sample epoch (no video here)

static float stick_x() {
    WPADFSStatus fs;
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    return fs.fsStickX;
}

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    memset(&fake, 0, sizeof fake);
    fake.lx = fake.ly = fake.rx = fake.ry = 128;
    NpSdk_SetInputSource(src);
    WPADRegisterAllocator(alloc, freefn);
    KPADInit();

    KPADStatus k[KPAD_MAX_SAMPLES];
    WPADFSStatus fs;

    // 1. the harness is asked, answers "not pressing anything": the real pad stays
    fake.buttons = NP_BTN_SQUARE;
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK(h_calls > 0);
    CHECK(fs.button & 0x0400);   // B from square
    CHECK(!(fs.button & 0x0800));

    // 2. the script presses A (GameCube bit 0x100) and holds the stick at +72, 0: the real pad is replaced
    h_active = 1;
    memset(&h_pad, 0, sizeof h_pad);
    h_pad.buttons = 0x0100 | 0x0008 | 0x1000;   // A, UP, START
    h_pad.stickX = 72;
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK((fs.button & 0x0800) != 0);   // A
    CHECK((fs.button & 0x0008) != 0);   // D-pad up
    CHECK((fs.button & 0x0200) != 0);   // "1" = start
    CHECK(!(fs.button & 0x0400));       // the real square is gone
    CHECK(fs.fsStickX == 100 && fs.fsStickY == 0);   // full right = 100 on the Nunchuk
    CHECK(KPADRead(0, k, 1) == 1 && k[0].ex_status.fs.stick.x > 0.99f);

    // 3. half stick, diagonal and rounding: 36 of 72 -> 0.5 (+-1 raw unit of quantisation)
    h_pad.stickX = 36; h_pad.stickY = -36;
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK(fs.fsStickX >= 48 && fs.fsStickX <= 52);   // 36 of 72 on each axis = 0.5 * 100
    CHECK(fs.fsStickY <= -48 && fs.fsStickY >= -52);

    // 4. the GameCube view of the same pad (what a game-side patch reads)
    h_pad.buttons = 0x0200 | 0x0020 | 0x0040 | 0x0010;   // B, R, L, Z
    h_pad.stickX = 0; h_pad.stickY = 0;
    PADStatus pads[PAD_MAX_CONTROLLERS];
    settle();   // (the GameCube view is the sample of the retrace, like the remote's)
    PADRead(pads);
    CHECK(pads[0].err == PAD_ERR_NONE);
    CHECK((pads[0].button & (PAD_BUTTON_B | PAD_TRIGGER_R | PAD_TRIGGER_L | PAD_TRIGGER_Z)) ==
          (PAD_BUTTON_B | PAD_TRIGGER_R | PAD_TRIGGER_L | PAD_TRIGGER_Z));
    CHECK(pads[0].triggerLeft == 255 && pads[0].triggerRight == 255);
    CHECK(pads[0].stickX == 0 && pads[0].stickY == 0);

    // 5. the harness stops pressing: the real pad is back
    h_active = 0;
    fake.buttons = NP_BTN_CROSS;
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK((fs.button & 0x0800) != 0 && !(fs.button & 0x0400));

    // 6. the game-aware layer: one call per sample (WPADRead + KPADRead + a second WPADRead share it), edits stick
    // in, hold and pointer out
    a_mode = 1;
    fake.buttons = 0;
    settle();
    int before = a_calls;
    WPADRead(0, (WPADStatus*)&fs);
    KPADRead(0, k, 1);
    WPADRead(0, (WPADStatus*)&fs);
    CHECK(a_calls == before + 1);
    CHECK((fs.button & 0x0200) != 0);
    CHECK(a_last_dt > 0.0f && a_last_dt <= 0.1f);
    CHECK(k[0].hold & 0x0200);
    CHECK(k[0].dpd_valid_fg == 0);   // pointer_hidden: no IR pointer this sample
    a_mode = 2;                      // shown: the cursor the layer set
    settle();
    KPADRead(0, k, 1);
    CHECK(a_calls == before + 2);
    CHECK(k[0].dpd_valid_fg == 2);
    CHECK(fabsf(k[0].pos.x - 0.5f) < 1e-5f && fabsf(k[0].pos.y + 0.25f) < 1e-5f);
    settle();   // the cursor persists into the next sample
    a_mode = 0;
    KPADRead(0, k, 1);
    CHECK(fabsf(k[0].pos.x - 0.5f) < 1e-5f && fabsf(k[0].pos.y + 0.25f) < 1e-5f);

    // 7. blocked input: connected but neutral, also under a script
    a_mode = 0;
    h_active = 1; h_pad.buttons = 0x0100; h_pad.stickX = 72;
    NpSdk_SetInputBlocked(1);
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK(fs.err == WPAD_ERR_OK && (fs.button & 0x0800) == 0 && fs.fsStickX == 0);
    NpSdk_SetInputBlocked(0);
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK((fs.button & 0x0800) != 0);

    // 8. no input source registered, the Vita platform layer provides the controller (and the touch)
    NpSdk_SetInputSource(0);
    NpSdk_SetInputBlocked(0);
    h_active = 0;
    v_present = 1;
    memset(&v_in, 0, sizeof v_in);
    v_in.lx = v_in.ly = v_in.rx = v_in.ry = 128;
    v_in.buttons = NP_BTN_CROSS | NP_BTN_START;
    v_in.lx = 255;
    v_in.touchCount = 1;
    v_in.touch[0].id = 7; v_in.touch[0].x = 480.0f; v_in.touch[0].y = 272.0f;
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK((fs.button & 0x0800) != 0 && (fs.button & 0x0200) != 0);   // A, "1"
    CHECK(fs.fsStickX == 100);
    v_present = 0;   // sample failed: the default (an untouched controller) takes over
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK(fs.err == WPAD_ERR_OK && (fs.button & 0x0800) == 0);

    printf(g_fail ? "FAILED (%d)\n" : "test_input_hooks: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
