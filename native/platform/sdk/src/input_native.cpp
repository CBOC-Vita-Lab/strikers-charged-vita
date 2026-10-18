// PAD, WPAD and KPAD: the controllers of the Wii SDK, emulated from the platform's raw pad
// (np_sdk.h, NpRawPad). Logic and mapping follow the recomp's HLE (hle/input/{pad,wpad,kpad}.cpp and
// vita_input.cpp), minus the game-aware menu navigation (the pointer is moved by the right stick or
// placed by the touch panel) and the guest-memory layer.
//
// The emulated controller is a Wii Remote with a Nunchuk (WPAD_DEV_FREESTYLE), as the recompiled
// build presented one (NP_PAD_PROFILE=remote makes it a bare remote):
//   cross -> A, square -> B, circle -> C, L -> Z, start -> "1" (pause), select -> "-", triangle -> "+",
//   D-pad -> D-pad, left stick -> Nunchuk stick, right stick -> the pointer (it stays where it is left),
//   a finger on the touch panel -> the pointer at that spot, held down it presses A.
// The accelerometers read 1 g at rest (remote lying flat, Nunchuk upright). The player's channel is
// tagged in the button word (NP_GC_TAG | chan << 5) for game-side GameCube-controls patches.
#include <revolution/kpad.h>
#include <revolution/os.h>
#include <revolution/pad.h>
#include <revolution/vi.h>
#include <revolution/wpad.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "np/np_pad.h"
#include "np_internal.h"

// The nav-script harness (platform/harness, weak: absent in builds without it). Its pad is in GameCube terms
// (np_harness.h: NpPadState, same layout); it replaces the controller of channel 0 while a script presses something.
struct NpSdkHarnessPad {
    uint16_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerL, triggerR;
};
extern "C" int NpHarnessFillPad(NpSdkHarnessPad* pad) __attribute__((weak));
// W5 CTL: a script's pad in the Vita's own terms (np_harness.h NpHarnessRaw, same layout): buttons as SCE_CTRL bits, sticks as
// +-72 units like the GameCube scripts, front and rear fingers. Returns 1 while such a command runs.
struct NpSdkHarnessRaw {
    uint32_t buttons;
    int8_t stickX, stickY, substickX, substickY;
    int touchCount;
    struct { int id; float x, y; } touch[4];
    int rearCount;
    struct { int id; float x, y; } rear[4];
};
extern "C" int NpHarnessFillRaw(NpSdkHarnessRaw* raw) __attribute__((weak));
extern "C" uint64_t NpHarnessFrameCount(void) __attribute__((weak));   // presented frames counted by the harness
// The Vita platform layer (platform/vita, np_vita.h: NpVitaInput, same layout): sceCtrl + the front touch panel. When no input
// source was registered the SDK layer reads it by itself, so a Vita build has its controls without any registration code.
struct NpSdkVitaInput {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    int touchCount;
    struct { int id; float x, y; } touch[8];
    int rearCount;                               // W5 CTL: the rear panel (np_vita.h)
    struct { int id; float x, y; } rear[8];
};
extern "C" int NpVitaReadInput(NpSdkVitaInput* out) __attribute__((weak));
// The game-aware layer (platform/sdk/input, weak, see np_sdk.h).
extern "C" void NpPadAdjust(NpPadSample* sample) __attribute__((weak));
extern "C" void NpLatPadUse(uint64_t rawReadNs, int chan) __attribute__((weak));   // W5 RT (np/latency.h): the input-delay record of the frame

namespace {

// WPAD button bits (WPAD_BUTTON_*)
enum : uint32_t {
    W_LEFT = 0x0001, W_RIGHT = 0x0002, W_DOWN = 0x0004, W_UP = 0x0008, W_PLUS = 0x0010, W_2 = 0x0100,
    W_1 = 0x0200, W_B = 0x0400, W_A = 0x0800, W_MINUS = 0x1000, W_Z = 0x2000, W_C = 0x4000, W_HOME = 0x8000
};

NpInputSourceFn g_source = 0;
NpRumbleFn g_rumble = 0;
volatile int g_blocked = 0;

// GameCube pad bits as the harness has them
enum : uint32_t {
    GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008, GC_Z = 0x0010, GC_R = 0x0020, GC_L = 0x0040,
    GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000
};

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Radial dead zone and scaling of a Vita stick (VitaInput::MapStick): 16 raw units dead, 102 full.
// Result in -1..1, +y up.
void map_stick(uint8_t rx, uint8_t ry, float* ox, float* oy) {
    const float kDead = 16.0f, kFull = 102.0f;
    float x = (float)rx - 128.0f;
    float y = 128.0f - (float)ry;
    float m = sqrtf(x * x + y * y);
    if (m <= kDead) {
        *ox = *oy = 0.0f;
        return;
    }
    float scaled = (m - kDead) / (kFull - kDead);
    if (scaled > 1.0f) scaled = 1.0f;
    float k = scaled / m;
    *ox = x * k;
    *oy = y * k;
}

// The inverse of map_stick for a stick given as +-72 GameCube units: a raw Vita stick position (0..255, 128 centre).
void gc_stick_to_raw(int sx, int sy, uint8_t* rx, uint8_t* ry) {
    const float kDead = 16.0f, kFull = 102.0f;
    float nx = (float)sx / 72.0f, ny = (float)sy / 72.0f;
    float m = sqrtf(nx * nx + ny * ny);
    if (m < 1e-4f) {
        *rx = *ry = 128;
        return;
    }
    if (m > 1.0f) {
        nx /= m;
        ny /= m;
        m = 1.0f;
    }
    float off = kDead + m * (kFull - kDead);
    float x = nx / m * off, y = ny / m * off;
    long ix = lroundf(128.0f + x), iy = lroundf(128.0f - y);
    *rx = (uint8_t)(ix < 0 ? 0 : ix > 255 ? 255 : ix);
    *ry = (uint8_t)(iy < 0 ? 0 : iy > 255 ? 255 : iy);
}

// The harness replaces the player's controller (channel 0) with its scripted pad: the platform's raw pad is
// shown to it in GameCube terms and, when the script presses something (it returns 1), what comes back is turned
// into the raw pad again (cross = A, square = B, circle = X, triangle = Y, select = Z; touch and motion off).
void apply_harness(NpRawPad* raw) {
    if (NpHarnessFillRaw) {
        // a Vita-button command (W5 CTL): the raw pad as the script gives it; the layout of the player's settings applies to it
        NpSdkHarnessRaw hr;
        memset(&hr, 0, sizeof hr);
        if (NpHarnessFillRaw(&hr)) {
            raw->buttons = hr.buttons;
            gc_stick_to_raw(hr.stickX, hr.stickY, &raw->lx, &raw->ly);
            gc_stick_to_raw(hr.substickX, hr.substickY, &raw->rx, &raw->ry);
            raw->touch_count = hr.touchCount < 0 ? 0 : hr.touchCount > 4 ? 4 : hr.touchCount;
            for (int i = 0; i < raw->touch_count; ++i) {
                raw->touch[i].id = hr.touch[i].id;
                raw->touch[i].x = hr.touch[i].x;
                raw->touch[i].y = hr.touch[i].y;
            }
            raw->rear_count = hr.rearCount < 0 ? 0 : hr.rearCount > 4 ? 4 : hr.rearCount;
            for (int i = 0; i < raw->rear_count; ++i) {
                raw->rear[i].id = hr.rear[i].id;
                raw->rear[i].x = hr.rear[i].x;
                raw->rear[i].y = hr.rear[i].y;
            }
            raw->has_motion = 0;
            raw->source = NP_RAW_SCRIPT_VITA;
            return;
        }
    }
    if (!NpHarnessFillPad) return;
    NpSdkHarnessPad hp;
    memset(&hp, 0, sizeof hp);
    {
        const uint32_t r = raw->buttons;
        uint16_t b = 0;
        if (r & NP_BTN_CROSS) b |= GC_A;
        if (r & NP_BTN_SQUARE) b |= GC_B;
        if (r & NP_BTN_CIRCLE) b |= GC_X;
        if (r & NP_BTN_TRIANGLE) b |= GC_Y;
        if (r & NP_BTN_L) b |= GC_L;
        if (r & NP_BTN_R) b |= GC_R;
        if (r & NP_BTN_SELECT) b |= GC_Z;
        if (r & NP_BTN_START) b |= GC_START;
        if (r & NP_BTN_UP) b |= GC_UP;
        if (r & NP_BTN_DOWN) b |= GC_DOWN;
        if (r & NP_BTN_LEFT) b |= GC_LEFT;
        if (r & NP_BTN_RIGHT) b |= GC_RIGHT;
        hp.buttons = b;
    }
    if (!NpHarnessFillPad(&hp)) return;
    uint32_t r = 0;
    const uint32_t b = hp.buttons;
    if (b & GC_A) r |= NP_BTN_CROSS;
    if (b & GC_B) r |= NP_BTN_SQUARE;
    if (b & GC_X) r |= NP_BTN_CIRCLE;
    if (b & GC_Y) r |= NP_BTN_TRIANGLE;
    if (b & GC_L) r |= NP_BTN_L;
    if (b & GC_R) r |= NP_BTN_R;
    if (b & GC_Z) r |= NP_BTN_SELECT;
    if (b & GC_START) r |= NP_BTN_START;
    if (b & GC_UP) r |= NP_BTN_UP;
    if (b & GC_DOWN) r |= NP_BTN_DOWN;
    if (b & GC_LEFT) r |= NP_BTN_LEFT;
    if (b & GC_RIGHT) r |= NP_BTN_RIGHT;
    raw->buttons = r;
    gc_stick_to_raw(hp.stickX, hp.stickY, &raw->lx, &raw->ly);
    gc_stick_to_raw(hp.substickX, hp.substickY, &raw->rx, &raw->ry);
    raw->touch_count = 0;
    raw->rear_count = 0;
    raw->has_motion = 0;
    raw->source = NP_RAW_SCRIPT_GC;   // GameCube semantics: the match maps it by position whatever the layout (W5 CTL)
}

bool read_raw(int pad, NpRawPad* out) {
    memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;
    bool present;
    NpSdkVitaInput vi;
    if (!g_source && pad == 0 && NpVitaReadInput && NpVitaReadInput(&vi)) {
        out->buttons = vi.buttons;
        out->lx = vi.lx; out->ly = vi.ly; out->rx = vi.rx; out->ry = vi.ry;
        out->touch_count = vi.touchCount > NP_MAX_TOUCH ? NP_MAX_TOUCH : vi.touchCount;
        for (int i = 0; i < out->touch_count; ++i) {
            out->touch[i].id = vi.touch[i].id;
            out->touch[i].x = vi.touch[i].x;
            out->touch[i].y = vi.touch[i].y;
        }
        out->rear_count = vi.rearCount > NP_MAX_TOUCH ? NP_MAX_TOUCH : vi.rearCount < 0 ? 0 : vi.rearCount;
        for (int i = 0; i < out->rear_count; ++i) {
            out->rear[i].id = vi.rear[i].id;
            out->rear[i].x = vi.rear[i].x;
            out->rear[i].y = vi.rear[i].y;
        }
        present = true;
    } else if (!g_source) {
        // No platform source installed (a plain host run): player 1 has a controller lying untouched, so the game
        // gets past its "connect a Wii Remote" screens; NP_PAD_PRESENT=0 turns that off.
        static int present_default = -1;
        if (present_default < 0) {
            const char* e = getenv("NP_PAD_PRESENT");
            present_default = !(e && *e == '0');
        }
        present = pad == 0 && present_default;
    } else {
        present = g_source(pad, out) != 0;
    }
    if (present && pad == 0) apply_harness(out);
    if (present && g_blocked) {
        // the platform owns the display: connected, but neutral
        out->buttons = 0;
        out->lx = out->ly = out->rx = out->ry = 128;
        out->touch_count = 0;
        out->rear_count = 0;
    }
    return present;
}

// ---- the emulated controller -------------------------------------------------------------------

struct Emu {
    bool present;
    uint32_t hold;          // WPAD buttons (+ tag)
    float stick[2];         // Nunchuk, -1..1, +y up
    float pointer[2];       // KPAD pos: -1..1, +y down
    bool pointer_valid;
    float acc[3], nacc[3];
    // for the GameCube view
    NpRawPad raw;
};

struct Chan {
    bool connected_reported;
    WPADConnectCallback* connect_cb;
    WPADExtensionCallback* ext_cb;
    u32 data_format;
    bool dpd_on;
    bool motor;
    float cursor[2];        // persistent pointer (right stick)
    uint64_t last_ns;
    // KPAD
    uint32_t prev_hold, prev_cl;
    uint64_t press_ns[16];  // when each button went down (for the repeat)
    uint64_t rep_ns[16];
    float prev_acc[3], prev_fsacc[3];
    float rep_delay, rep_pulse;
    // sample cache (see sample_epoch)
    uint64_t cache_ns;      // when the raw read behind the cached sample was made (W5 RT, input delay)
    uint64_t cache_epoch;
    bool cache_valid;
    bool cache_have;
    Emu cache;
};
Chan g_chan[WPAD_MAX_CONTROLLERS];
int g_profile = -1;   // 0 remote, 1 remote + nunchuk
bool g_wpad_inited = false;
bool g_kpad_inited = false;
float g_pointer_speed = 1.0f;
int g_touch_pointer = 1;
int g_gc_tag = 1;
WPADAllocFunc g_alloc = 0;
WPADFreeFunc g_free = 0;

void load_env() {
    if (g_profile >= 0) return;
    const char* p = getenv("NP_PAD_PROFILE");
    g_profile = (p && !strcmp(p, "remote")) ? 0 : 1;
    const char* s = getenv("NP_POINTER_SPEED");
    if (s && *s) g_pointer_speed = (float)atof(s);
    const char* t = getenv("NP_TOUCH_POINTER");
    if (t && *t) g_touch_pointer = atoi(t);
    const char* g = getenv("NP_GC_TAG");
    if (g && *g) g_gc_tag = atoi(g);
}

uint32_t wpad_dev(int /*chan*/) { return g_profile ? WPAD_DEV_FREESTYLE : WPAD_DEV_CORE; }

// The key a sample stays valid for: the VI retrace count once the video interface runs (WPADRead and KPADRead of one
// game frame get one sample; anything that runs a cursor integration or a menu-navigation step counts once), else
// 3 ms of the clock (unit tests without video).
uint64_t sample_epoch(uint64_t now_ns) {
    unsigned long rc = NpVI_GetRetraceCount();
    uint64_t e = rc ? ((1ull << 63) | rc) : now_ns / 3000000ull;
    // A scripted pad changes when the harness counts a presented frame: a frame that ran inside one retrace must still see it.
    if (NpHarnessFrameCount) e ^= NpHarnessFrameCount() << 40;
    return e;
}

// Without the game-aware layer: the right stick moves the pointer, a finger on the panel places it and presses A.
void default_adjust(NpPadSample* s) {
    const float speed = 1.6f * g_pointer_speed;
    auto dz = [](float v) { return fabsf(v) < 0.2f ? 0.0f : v; };
    s->cursor[0] = clampf(s->cursor[0] + dz(s->free_stick[0]) * speed * s->dt, -1.0f, 1.0f);
    s->cursor[1] = clampf(s->cursor[1] - dz(s->free_stick[1]) * speed * s->dt, -1.0f, 1.0f);
    if (g_touch_pointer && s->raw->touch_count > 0) {
        s->cursor[0] = clampf(s->raw->touch[0].x / 960.0f * 2.0f - 1.0f, -1.0f, 1.0f);
        s->cursor[1] = clampf(s->raw->touch[0].y / 544.0f * 2.0f - 1.0f, -1.0f, 1.0f);
        s->hold |= W_A;
    }
}

// Builds the emulated state of `chan` (one sample per retrace and channel, see sample_epoch).
bool emu_sample(int chan, Emu* out) {
    load_env();
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) return false;
    Chan& c = g_chan[chan];
    uint64_t now = np_time_ns();
    const uint64_t epoch = sample_epoch(now);
    if (c.cache_valid && c.cache_epoch == epoch) {
        if (NpLatPadUse) NpLatPadUse(c.cache_ns, chan);
        *out = c.cache;
        return c.cache_have;
    }
    Emu e;
    memset(&e, 0, sizeof(e));
    NpRawPad raw;
    e.present = read_raw(chan, &raw);
    c.cache_valid = true;
    c.cache_epoch = epoch;
    c.cache_ns = now;
    if (NpLatPadUse) NpLatPadUse(now, chan);
    if (!e.present) {
        c.cache_have = false;
        c.cache = e;
        *out = e;
        return false;
    }
    e.raw = raw;
    const uint32_t b = raw.buttons;
    if (b & NP_BTN_CROSS) e.hold |= W_A;
    if (b & NP_BTN_SQUARE) e.hold |= W_B;
    if (g_profile && (b & NP_BTN_CIRCLE)) e.hold |= W_C;
    if (b & NP_BTN_CIRCLE && !g_profile) e.hold |= W_2;
    if (g_profile && (b & NP_BTN_L)) e.hold |= W_Z;
    if (b & NP_BTN_START) e.hold |= W_1;
    if (b & NP_BTN_SELECT) e.hold |= W_MINUS;
    if (b & NP_BTN_TRIANGLE) e.hold |= W_PLUS;
    if (b & NP_BTN_UP) e.hold |= W_UP;
    if (b & NP_BTN_DOWN) e.hold |= W_DOWN;
    if (b & NP_BTN_LEFT) e.hold |= W_LEFT;
    if (b & NP_BTN_RIGHT) e.hold |= W_RIGHT;
    map_stick(raw.lx, raw.ly, &e.stick[0], &e.stick[1]);
    if (!g_profile) {
        // a bare remote: the left stick works the D-pad
        if (e.stick[1] > 0.6f) e.hold |= W_UP;
        if (e.stick[1] < -0.6f) e.hold |= W_DOWN;
        if (e.stick[0] < -0.6f) e.hold |= W_LEFT;
        if (e.stick[0] > 0.6f) e.hold |= W_RIGHT;
        e.stick[0] = e.stick[1] = 0.0f;
    }
    if (g_gc_tag) e.hold |= (uint32_t)NP_GC_TAG | ((uint32_t)chan << NP_GC_CHAN_SHIFT);

    // pointer, menu navigation, touch: the game-aware layer, or the plain behaviour
    NpPadSample ps;
    memset(&ps, 0, sizeof ps);
    ps.chan = chan;
    ps.raw = &raw;
    float dt = c.last_ns ? (float)(now - c.last_ns) / 1e9f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    c.last_ns = now;
    ps.dt = dt;
    ps.hold = e.hold;
    ps.stick[0] = e.stick[0];
    ps.stick[1] = e.stick[1];
    map_stick(raw.rx, raw.ry, &ps.free_stick[0], &ps.free_stick[1]);
    ps.cursor[0] = c.cursor[0];
    ps.cursor[1] = c.cursor[1];
    if (NpPadAdjust) NpPadAdjust(&ps);
    else default_adjust(&ps);
    e.hold = ps.hold;
    e.stick[0] = ps.stick[0];
    e.stick[1] = ps.stick[1];
    c.cursor[0] = ps.cursor[0];
    c.cursor[1] = ps.cursor[1];
    e.pointer[0] = c.cursor[0];
    e.pointer[1] = c.cursor[1];
    e.pointer_valid = !ps.pointer_hidden;
    // accelerometers: 1 g at rest in the KPAD frame (remote y = -1, Nunchuk y = -1)
    e.acc[1] = -1.0f;
    e.nacc[1] = -1.0f;
    c.cache_have = true;
    c.cache = e;
    *out = e;
    return true;
}

s16 wacc(float g, float per_g) {
    long v = lroundf(g * per_g);
    if (v < -512) v = -512;
    if (v > 511) v = 511;
    return (s16)v;
}

void fill_dpd(WPADStatus* st, const Emu& e) {
    for (int i = 0; i < WPAD_MAX_DPD_OBJECTS; ++i) {
        st->obj[i].x = 0x3FF;
        st->obj[i].y = 0x3FF;
        st->obj[i].size = 0;
        st->obj[i].traceId = 0;
    }
    if (!e.pointer_valid) return;
    // two sensor-bar dots 200 px apart around the point the remote "sees" for this pointer position
    float cx = (e.pointer[0] + 1.0f) * 0.5f * 1023.0f;
    float cy = (e.pointer[1] + 1.0f) * 0.5f * 767.0f;
    for (int i = 0; i < 2; ++i) {
        float x = clampf(cx + (i ? 100.0f : -100.0f), 0.0f, 1023.0f);
        st->obj[i].x = (s16)x;
        st->obj[i].y = (s16)clampf(767.0f - cy, 0.0f, 767.0f);
        st->obj[i].size = 4;
        st->obj[i].traceId = (u8)i;
    }
}

}  // namespace

extern "C" {

void NpSdk_SetInputSource(NpInputSourceFn fn) { g_source = fn; }
void NpSdk_SetInputBlocked(int blocked) { g_blocked = blocked; }
void NpSdk_SetRumbleSink(NpRumbleFn fn) { g_rumble = fn; }

// ---------------------------------------------------------------------------------------------
// PAD (GameCube pads): only the player's own controller, on channel 0 (and further sources 1..3)

static void gc_from_raw(const NpRawPad& raw, PADStatus* st) {
    uint16_t b = 0;
    const uint32_t r = raw.buttons;
    if (r & NP_BTN_CROSS) b |= PAD_BUTTON_A;
    if (r & NP_BTN_SQUARE) b |= PAD_BUTTON_B;
    if (r & NP_BTN_CIRCLE) b |= PAD_BUTTON_X;
    if (r & NP_BTN_TRIANGLE) b |= PAD_BUTTON_Y;
    if (r & NP_BTN_L) b |= PAD_TRIGGER_L;
    if (r & NP_BTN_R) b |= PAD_TRIGGER_R;
    if (r & NP_BTN_SELECT) b |= PAD_TRIGGER_Z;
    if (r & NP_BTN_START) b |= PAD_BUTTON_START;
    if (r & NP_BTN_UP) b |= PAD_BUTTON_UP;
    if (r & NP_BTN_DOWN) b |= PAD_BUTTON_DOWN;
    if (r & NP_BTN_LEFT) b |= PAD_BUTTON_LEFT;
    if (r & NP_BTN_RIGHT) b |= PAD_BUTTON_RIGHT;
    st->button = b;
    float x, y;
    map_stick(raw.lx, raw.ly, &x, &y);
    st->stickX = (s8)lroundf(x * 72.0f);
    st->stickY = (s8)lroundf(y * 72.0f);
    map_stick(raw.rx, raw.ry, &x, &y);
    st->substickX = (s8)lroundf(x * 72.0f);
    st->substickY = (s8)lroundf(y * 72.0f);
    st->triggerLeft = (r & NP_BTN_L) ? 255 : 0;
    st->triggerRight = (r & NP_BTN_R) ? 255 : 0;
    st->analogA = st->analogB = 0;
    st->err = PAD_ERR_NONE;
}

// The GameCube view of the SAME sample the Wii Remote view was made from (one per retrace and channel, emu_sample): the
// capture of a match reads this, so buttons, sticks and the remote's stick belong together, and a scripted pad is asked once.
int NpPad_ReadGameCube(int chan, PADStatus* out) {
    Emu e;
    memset(out, 0, sizeof(*out));
    out->err = PAD_ERR_NO_CONTROLLER;
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS || !emu_sample(chan, &e)) return 0;
    gc_from_raw(e.raw, out);
    return 1;
}

int NpPad_ReadRaw(int chan, NpRawPad* out) {
    Emu e;
    memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS || !emu_sample(chan, &e)) return 0;
    *out = e.raw;
    return 1;
}

BOOL PADInit(void) { return TRUE; }
BOOL PADReset(u32 mask) { (void)mask; return TRUE; }
BOOL PADRecalibrate(u32 mask) { (void)mask; return TRUE; }
BOOL PADSync(void) { return TRUE; }
void PADSetSpec(u32 spec) { (void)spec; }
PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback cb) { (void)cb; return 0; }
BOOL __PADDisableRecalibration(BOOL disable) { (void)disable; return FALSE; }
u32 __PADSpec = 5;
u32 __PADFixBits = 0;

u32 PADRead(PADStatus* status) {
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
        NpPad_ReadGameCube(i, &status[i]);
    }
    return 0;
}

void PADControlMotor(s32 chan, u32 command) {
    if (g_rumble && chan >= 0 && chan < PAD_MAX_CONTROLLERS) g_rumble(chan, command == PAD_MOTOR_RUMBLE);
}

// ---------------------------------------------------------------------------------------------
// WPAD

void WPADInit(void) {
    load_env();
    g_wpad_inited = true;
}
void WPADShutdown(void) { g_wpad_inited = false; }
WPADLibStatus WPADGetStatus(void) { return g_wpad_inited ? WPAD_LIB_STATUS_3 : WPAD_LIB_STATUS_0; }
void WPADRegisterAllocator(WPADAllocFunc a, WPADFreeFunc f) { g_alloc = a; g_free = f; }
u32 WPADGetWorkMemorySize(void) { return 0x10000; }
u8 WPADGetSensorBarPosition(void) { return SCGetWpadSensorBarPosition(); }
u8 WPADGetDpdSensitivity(void) { return 3; }
u8 WPADGetSpeakerVolume(void) { return SCGetWpadSpeakerVolume(); }
void WPADSetSpeakerVolume(u8 v) { SCSetWpadSpeakerVolume(v); }
BOOL WPADSetSensorBarPower(BOOL enable) { (void)enable; return TRUE; }
void WPADGetAddress(s32 chan, BD_ADDR_PTR addr) { memset(addr, 0, 6); ((u8*)addr)[5] = (u8)(chan + 1); }
void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAccGravityUnit* acc) {
    (void)chan;
    acc->x = 0;
    acc->y = 0;
    acc->z = (s16)(type == WPAD_ACC_GRAVITY_UNIT_FS ? 204 : 106);
}
u8 WPADGetRadioSensitivity(WPADChannel chan) { (void)chan; return 4; }
void WPADDisconnect(s32 chan) { (void)chan; }
void WPADRecalibrate(s32 chan) { (void)chan; }
BOOL WPADStartSimpleSync(void) { return TRUE; }
BOOL WPADStartFastSimpleSync(void) { return TRUE; }
BOOL WPADStopSimpleSync(void) { return TRUE; }
BOOL WPADStartClearDevice(void) { return TRUE; }
WPADSyncDeviceCallback WPADSetSimpleSyncCallback(WPADSyncDeviceCallback cb) { (void)cb; return 0; }
WPADClearDeviceCallback WPADSetClearDeviceCallback(WPADClearDeviceCallback cb) { (void)cb; return 0; }
BOOL WPADSaveConfig(WPADSaveCallback cb) { if (cb) cb(SC_STATUS_OK); return TRUE; }
WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback cb) { (void)chan; (void)cb; return 0; }
void WPADSetAutoSamplingBuf(s32 chan, void* buf, u32 len) { (void)chan; (void)buf; (void)len; }
s32 WPADReadFaceData(s32 chan, void* dst, u32 size, u32 src, WPADCallback cb) {
    (void)chan; (void)dst; (void)size; (void)src;
    if (cb) cb(chan, WPAD_ERR_NO_CONTROLLER);
    return WPAD_ERR_NO_CONTROLLER;
}

WPADResult WPADProbe(WPADChannel chan, WPADDeviceType* devType) {
    load_env();
    NpRawPad raw;
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS || !read_raw(chan, &raw)) {
        if (devType) *devType = WPAD_DEV_NOT_FOUND;
        return WPAD_ERR_NO_CONTROLLER;
    }
    if (devType) *devType = (WPADDeviceType)wpad_dev(chan);
    return WPAD_ERR_OK;
}

WPADConnectCallback* WPADSetConnectCallback(s32 chan, WPADConnectCallback* cb) {
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) return 0;
    Chan& c = g_chan[chan];
    WPADConnectCallback* old = c.connect_cb;
    c.connect_cb = cb;
    // a controller that is already connected reports itself to the new callback
    NpRawPad raw;
    if (cb && read_raw(chan, &raw)) cb(chan, WPAD_ERR_OK);
    return old;
}
WPADExtensionCallback* WPADSetExtensionCallback(s32 chan, WPADExtensionCallback* cb) {
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) return 0;
    WPADExtensionCallback* old = g_chan[chan].ext_cb;
    g_chan[chan].ext_cb = cb;
    return old;
}

u32 WPADGetDataFormat(s32 chan) {
    return (chan >= 0 && chan < WPAD_MAX_CONTROLLERS) ? g_chan[chan].data_format : 0;
}
s32 WPADSetDataFormat(s32 chan, u32 format) {
    NpRawPad raw;
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) return WPAD_ERR_INVALID;
    if (!read_raw(chan, &raw)) return WPAD_ERR_NO_CONTROLLER;
    g_chan[chan].data_format = format;
    return WPAD_ERR_OK;
}

static void fill_info(WPADInfo* info) {
    memset(info, 0, sizeof(*info));
    info->dpd = TRUE;
    info->speaker = FALSE;
    info->attach = g_profile ? TRUE : FALSE;
    info->battery = 4;
    info->led = 1;
}
WPADResult WPADGetInfo(WPADChannel chan, WPADInfo* info) {
    load_env();
    NpRawPad raw;
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS || !read_raw(chan, &raw)) return WPAD_ERR_NO_CONTROLLER;
    fill_info(info);
    return WPAD_ERR_OK;
}
s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback cb) {
    WPADResult r = WPADGetInfo(chan, info);
    if (cb) cb(chan, r);
    return r;
}

void WPADControlMotor(s32 chan, u32 command) {
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) return;
    g_chan[chan].motor = command != 0;
    if (g_rumble) g_rumble(chan, command != 0);
}
void WPADEnableMotor(BOOL enable) { SCSetWpadMotorMode(enable ? SC_MOTOR_ON : SC_MOTOR_OFF); }
BOOL WPADIsMotorEnabled(void) { return TRUE; }
s32 WPADControlLed(s32 chan, u8 flags, WPADCallback cb) {
    (void)flags;
    NpRawPad raw;
    WPADResult r = (chan >= 0 && chan < WPAD_MAX_CONTROLLERS && read_raw(chan, &raw)) ? WPAD_ERR_OK : WPAD_ERR_NO_CONTROLLER;
    if (cb) cb(chan, r);
    return r;
}
// The remote's speaker does not exist here: the game's speaker set-up sequence stops at the first
// step (result "no controller"), so no audio is streamed and its voices play on the main output only.
BOOL WPADIsSpeakerEnabled(s32 chan) { (void)chan; return FALSE; }
WPADResult WPADControlSpeaker(WPADChannel chan, u32 command, WPADCallback cb) {
    (void)command;
    if (cb) cb(chan, WPAD_ERR_NO_CONTROLLER);
    return WPAD_ERR_NO_CONTROLLER;
}
BOOL WPADCanSendStreamData(s32 chan) { (void)chan; return FALSE; }
s32 WPADSendStreamData(s32 chan, void* data, u16 len) { (void)chan; (void)data; (void)len; return WPAD_ERR_NO_CONTROLLER; }
BOOL WPADIsDpdEnabled(s32 chan) { return (chan >= 0 && chan < WPAD_MAX_CONTROLLERS) ? g_chan[chan].dpd_on : FALSE; }
s32 WPADControlDpd(s32 chan, u32 command, WPADCallback cb) {
    NpRawPad raw;
    WPADResult r;
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) r = WPAD_ERR_INVALID;
    else if (!read_raw(chan, &raw)) r = WPAD_ERR_NO_CONTROLLER;
    else {
        g_chan[chan].dpd_on = command != WPAD_DPD_DISABLE;
        if (command != 0) { g_chan[chan].cursor[0] = g_chan[chan].cursor[1] = 0.0f; }   // centred when switched on
        r = WPAD_ERR_OK;
    }
    if (cb) cb(chan, r);
    return r;
}

// WPADRead: the raw status. The game passes a WPADFSStatus when the channel's device is a Nunchuk.
void WPADRead(s32 chan, WPADStatus* st) {
    Emu e;
    memset(st, 0, sizeof(WPADStatus));
    if (!emu_sample(chan, &e)) {
        st->err = WPAD_ERR_NO_CONTROLLER;
        st->dev = WPAD_DEV_NOT_FOUND;
        return;
    }
    st->button = (u16)(e.hold & 0xFFFF);
    st->accX = wacc(-e.acc[0], 106.0f);
    st->accY = wacc(e.acc[2], 106.0f);
    st->accZ = wacc(-e.acc[1], 106.0f);
    fill_dpd(st, e);
    st->dev = (u8)wpad_dev(chan);
    st->err = WPAD_ERR_OK;
    if (g_profile) {
        WPADFSStatus* fs = (WPADFSStatus*)st;
        fs->fsAccX = wacc(-e.nacc[0], 204.0f);
        fs->fsAccY = wacc(e.nacc[2], 204.0f);
        fs->fsAccZ = wacc(-e.nacc[1], 204.0f);
        fs->fsStickX = (s8)lroundf(clampf(e.stick[0] * 100.0f, -128.0f, 127.0f));
        fs->fsStickY = (s8)lroundf(clampf(e.stick[1] * 100.0f, -128.0f, 127.0f));
    }
}

// ---------------------------------------------------------------------------------------------
// KPAD

static float vlen(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void KPADInit(void) {
    load_env();
    g_kpad_inited = true;
    for (int i = 0; i < WPAD_MAX_CONTROLLERS; ++i) {
        g_chan[i].rep_delay = 0.75f;
        g_chan[i].rep_pulse = 0.25f;
        g_chan[i].prev_acc[1] = g_chan[i].prev_fsacc[1] = -1.0f;
    }
    if (!g_wpad_inited) WPADInit();
}
void KPADSetBtnRepeat(s32 chan, f32 delay, f32 pulse) {
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS) return;
    g_chan[chan].rep_delay = delay;
    g_chan[chan].rep_pulse = pulse;
}
void KPADSetPosParam(s32, f32, f32) {}
void KPADSetHoriParam(s32, f32, f32) {}
void KPADSetDistParam(s32, f32, f32) {}
void KPADSetAccParam(s32, f32, f32) {}

// KPADRead: one sample per call (the game asks for room for 16 and uses the first). Returns the
// number of samples, 0 with wpad_err set when there is no controller.
s32 KPADRead(s32 chan, KPADStatus* out, u32 num) {
    Emu e;
    if (chan < 0 || chan >= WPAD_MAX_CONTROLLERS || !out || num == 0) return 0;
    Chan& c = g_chan[chan];
    memset(out, 0, sizeof(KPADStatus));
    if (!emu_sample(chan, &e)) {
        c.prev_hold = 0;
        out->dev_type = WPAD_DEV_CORE;
        out->wpad_err = WPAD_ERR_NO_CONTROLLER;
        return 0;
    }
    uint64_t now = np_time_ns();
    uint32_t hold = e.hold;
    out->hold = hold;
    // trig: new presses, plus the auto-repeat KPADSetBtnRepeat configures
    uint32_t trig = hold & ~c.prev_hold;
    for (int i = 0; i < 16; ++i) {
        uint32_t bit = 1u << i;
        if (trig & bit) {
            c.press_ns[i] = now;
            c.rep_ns[i] = now + (uint64_t)(c.rep_delay * 1e9f);
        } else if ((hold & bit) && c.rep_delay > 0.0f && now >= c.rep_ns[i]) {
            trig |= bit;
            c.rep_ns[i] = now + (uint64_t)((c.rep_pulse > 0.0f ? c.rep_pulse : 0.25f) * 1e9f);
        }
    }
    out->trig = trig;
    out->release = c.prev_hold & ~hold;
    c.prev_hold = hold;

    out->acc.x = e.acc[0];
    out->acc.y = e.acc[1];
    out->acc.z = e.acc[2];
    out->acc_value = vlen(e.acc);
    float d[3] = {e.acc[0] - c.prev_acc[0], e.acc[1] - c.prev_acc[1], e.acc[2] - c.prev_acc[2]};
    out->acc_speed = vlen(d);
    memcpy(c.prev_acc, e.acc, sizeof(c.prev_acc));

    if (e.pointer_valid) {   // like the recompiled build: valid whether or not the game switched the IR camera on
        out->pos.x = e.pointer[0];
        out->pos.y = e.pointer[1];
        out->horizon.x = 1.0f;
        out->horizon.y = 0.0f;
        out->dist = 1.0f;
        out->dpd_valid_fg = 2;
    }
    out->dev_type = (u8)wpad_dev(chan);
    out->wpad_err = WPAD_ERR_OK;
    out->data_format = (u8)c.data_format;
    if (g_profile) {
        out->ex_status.fs.stick.x = e.stick[0];
        out->ex_status.fs.stick.y = e.stick[1];
        out->ex_status.fs.acc.x = e.nacc[0];
        out->ex_status.fs.acc.y = e.nacc[1];
        out->ex_status.fs.acc.z = e.nacc[2];
        out->ex_status.fs.acc_value = vlen(e.nacc);
        float nd[3] = {e.nacc[0] - c.prev_fsacc[0], e.nacc[1] - c.prev_fsacc[1], e.nacc[2] - c.prev_fsacc[2]};
        out->ex_status.fs.acc_speed = vlen(nd);
        memcpy(c.prev_fsacc, e.nacc, sizeof(c.prev_fsacc));
    }
    return 1;
}

}  // extern "C"
