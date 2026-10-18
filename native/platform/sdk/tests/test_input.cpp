// Unit tests of PAD / WPAD / KPAD: the game's start-up sequence (PlatPadManager) against a fake pad.
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

static NpRawPad fake;
static int fake_present = 1;
static int src(int pad, NpRawPad* out) {
    if (pad != 0 || !fake_present) return 0;
    *out = fake;
    return 1;
}
static int connected[4];
static void on_connect(WPADChannel chan, WPADResult r) { if (r == WPAD_ERR_OK) connected[chan] = 1; }
static void* alloc(u32 n) { return 0; }
static BOOL freefn(void*) { return TRUE; }
static int rumble_calls = 0, rumble_on = 0;
static void on_rumble(int pad, int on) { rumble_calls++; rumble_on = on; }

static void settle() { OSSleepTicks(OSMillisecondsToTicks(5)); }   // past the 3 ms sample cache

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    memset(&fake, 0, sizeof fake);
    fake.lx = fake.ly = fake.rx = fake.ry = 128;
    NpSdk_SetInputSource(src);
    NpSdk_SetRumbleSink(on_rumble);

    // PlatPadManager::Initialize
    WPADRegisterAllocator(alloc, freefn);
    KPADInit();
    CHECK(WPADGetStatus() == WPAD_LIB_STATUS_3);
    for (int ch = 0; ch < WPAD_MAX_CONTROLLERS; ++ch) {
        KPADSetPosParam(ch, 0.02f, 0.95f);
        KPADSetBtnRepeat(ch, 0.75f, 0.25f);
        WPADSetConnectCallback(ch, on_connect);
    }
    CHECK(connected[0] == 1 && connected[1] == 0 && connected[2] == 0 && connected[3] == 0);

    // PlatPadManager::UpdateChannel
    WPADDeviceType dev;
    CHECK(WPADProbe(0, &dev) == WPAD_ERR_OK && dev == WPAD_DEV_FREESTYLE);
    CHECK(WPADProbe(1, &dev) == WPAD_ERR_NO_CONTROLLER);
    CHECK(WPADSetDataFormat(0, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_OK);
    CHECK(WPADSetDataFormat(1, WPAD_FMT_FS_BTN_ACC_DPD) == WPAD_ERR_NO_CONTROLLER);
    CHECK(WPADControlDpd(0, WPAD_DPD_BASIC, 0) == WPAD_ERR_OK);

    WPADFSStatus fs;
    KPADStatus k[KPAD_MAX_SAMPLES];
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK(fs.err == WPAD_ERR_OK && fs.dev == WPAD_DEV_FREESTYLE);
    CHECK(fs.fsStickX == 0 && fs.fsStickY == 0 && fs.button == (0x0080));   // only the tag
    CHECK(fs.accZ == 106 && fs.fsAccZ == 204);   // 1 g at rest
    CHECK(KPADRead(0, k, 1) == 1);
    CHECK(k[0].dev_type == WPAD_DEV_FREESTYLE && k[0].wpad_err == 0 && k[0].dpd_valid_fg == 2);

    // buttons, sticks, edges
    fake.buttons = NP_BTN_CROSS | NP_BTN_LEFT;
    fake.lx = 255; fake.ly = 128;   // full right
    settle();
    WPADRead(0, (WPADStatus*)&fs);
    CHECK((fs.button & 0x0800) && (fs.button & 0x0001) && !(fs.button & 0x0400));
    CHECK(fs.fsStickX == 100 && fs.fsStickY == 0);
    CHECK(KPADRead(0, k, 1) == 1);
    CHECK((k[0].hold & 0x0800) && (k[0].trig & 0x0800) && k[0].release == 0);
    CHECK(k[0].ex_status.fs.stick.x > 0.9f);
    settle();
    CHECK(KPADRead(0, k, 1) == 1);
    CHECK((k[0].hold & 0x0800) && !(k[0].trig & 0x0800));   // held: no new press
    fake.buttons = 0;
    fake.lx = fake.ly = 128;
    settle();
    CHECK(KPADRead(0, k, 1) == 1);
    CHECK((k[0].release & 0x0800) && k[0].hold == (0x0080));

    // auto repeat: held for longer than the delay -> trig fires again
    KPADSetBtnRepeat(0, 0.02f, 0.01f);
    fake.buttons = NP_BTN_RIGHT;
    settle();
    KPADRead(0, k, 1);
    CHECK(k[0].trig & 0x0002);
    int repeats = 0;
    for (int i = 0; i < 20; ++i) {
        OSSleepTicks(OSMillisecondsToTicks(6));
        KPADRead(0, k, 1);
        if (k[0].trig & 0x0002) repeats++;
    }
    printf("  %d repeat trigs in 20 reads\n", repeats);
    CHECK(repeats >= 3);
    fake.buttons = 0;

    // pointer: the right stick moves it, a touch places it and presses A
    WPADControlDpd(0, WPAD_DPD_BASIC, 0);   // centres
    settle();
    KPADRead(0, k, 1);
    CHECK(fabsf(k[0].pos.x) < 0.01f && fabsf(k[0].pos.y) < 0.01f);
    fake.rx = 255; fake.ry = 128;
    for (int i = 0; i < 10; ++i) { OSSleepTicks(OSMillisecondsToTicks(10)); KPADRead(0, k, 1); }
    CHECK(k[0].pos.x > 0.1f);
    fake.rx = fake.ry = 128;
    fake.touch_count = 1;
    fake.touch[0].x = 480.0f + 240.0f;
    fake.touch[0].y = 136.0f;
    settle();
    KPADRead(0, k, 1);
    CHECK(fabsf(k[0].pos.x - 0.5f) < 0.01f && fabsf(k[0].pos.y + 0.5f) < 0.01f && (k[0].hold & 0x0800));
    fake.touch_count = 0;

    // rumble and the speaker stub
    WPADControlMotor(0, WPAD_MOTOR_RUMBLE);
    CHECK(rumble_calls == 1 && rumble_on == 1);
    WPADControlMotor(0, WPAD_MOTOR_STOP);
    CHECK(rumble_on == 0);
    CHECK(WPADControlSpeaker(0, WPAD_SPEAKER_ON, 0) == WPAD_ERR_NO_CONTROLLER);

    // GameCube view
    fake.buttons = NP_BTN_SQUARE | NP_BTN_R | NP_BTN_UP;
    fake.lx = 0; fake.ly = 128;
    PADStatus pads[4];
    settle();   // the GameCube view is the sample of the current retrace (3 ms epoch without video), like the remote's
    PADRead(pads);
    CHECK(pads[0].err == PAD_ERR_NONE && (pads[0].button & PAD_BUTTON_B) && (pads[0].button & PAD_TRIGGER_R) && (pads[0].button & PAD_BUTTON_UP));
    CHECK(pads[0].triggerRight == 255 && pads[0].stickX == -72);
    CHECK(pads[1].err == PAD_ERR_NO_CONTROLLER);

    // unplugged
    fake_present = 0;
    settle();
    CHECK(WPADProbe(0, &dev) == WPAD_ERR_NO_CONTROLLER);
    CHECK(KPADRead(0, k, 1) == 0 && k[0].wpad_err == WPAD_ERR_NO_CONTROLLER);

    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_input: ok\n");
    return 0;
}
