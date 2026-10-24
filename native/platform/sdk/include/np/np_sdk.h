// np_sdk.h: the interface between the application (main, harness, Vita platform layer) and the
// Wii SDK layer in platform/sdk. The game itself only calls the Revolution SDK functions
// (declared by the decomp's headers); this header is for whoever starts the program.
//
//   NpSdk_ConfigDefaults(&cfg);  // reads NP_* environment variables on top of the defaults
//   cfg.disc_dir = "...";        // optional overrides
//   NpSdk_Init(&cfg);            // before the game's main: OS, memory arenas, timers
//   ... run the game ...
//   NpSdk_Shutdown();
//
// Module hooks (audio sink, input source, video presenter) are registered with the NpSdk_Set*
// functions declared further down; every one has a working default (silent audio, keyboard-less
// "no controller", frame counter only), so a program that registers nothing still runs.
#ifndef NP_SDK_H
#define NP_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NpSdkConfig {
    // The extracted disc: <disc_dir>/files is the DVD file system root, <disc_dir>/sys/fst.bin (when
    // present) gives the retail entry numbers and disc offsets. NP_DISC_DIR overrides the default
    // ("ux0:data/mscharged/game" on the Vita, "game" elsewhere).
    const char* disc_dir;
    // Writable data: the NAND file system (saves, settings) under <data_dir>/NAND. NP_DATA_DIR
    // ("ux0:data/mscharged" on the Vita, "appdata" elsewhere).
    const char* data_dir;
    // Memory arenas of the console (OSGetMEM1ArenaLo/Hi, OSGetMEM2ArenaLo/Hi): the game takes all of
    // both for its two allocators. NP_MEM1_MB / NP_MEM2_MB. Defaults 24 and 64 MiB.
    uint32_t mem1_size;
    uint32_t mem2_size;
    // Where the arenas are mapped (0 = anywhere). Sample data for the audio mixer must have bit 31 of
    // its address set (see docs/sdk.md, "AX addresses"): the defaults 0x80000000 / 0x90000000 give the
    // console's own layout where the host allows it, and are ignored on the Vita (its user memory is
    // above 0x80000000 anyway).
    uintptr_t mem1_base;
    uintptr_t mem2_base;
    // System settings (SC): language 0 = Japanese 1 = English 2 = German 3 = French 4 = Spanish
    // 5 = Italian 6 = Dutch (the SDK's numbering), aspect 0 = 4:3 1 = 16:9, progressive, EuRGB60, TV
    // format 0 = NTSC 1 = PAL. NP_LANG / NP_WIDESCREEN / NP_PROGRESSIVE / NP_EURGB60 / NP_TVFORMAT.
    int language;
    int widescreen;
    int progressive;
    int eurgb60;
    int tv_format;
} NpSdkConfig;

void NpSdk_ConfigDefaults(NpSdkConfig* cfg);
int NpSdk_Init(const NpSdkConfig* cfg);   // 0 on success; safe to call twice (second call ignored)
void NpSdk_Shutdown(void);

// What "leave the game" means: OSShutdownSystem / OSReturnToMenu / OSRestart / OSRebootSystem call
// the hook with the reason (OS_SHUTDOWN_*). Default: flush and exit(0). NpSdk_QuitRequested() is
// non-zero once one of them was called (the harness and the Vita layer poll it).
void NpSdk_SetQuitHook(void (*hook)(int reason));
int NpSdk_QuitRequested(void);
// The platform's power button: runs the callback the game registered with OSSetPowerCallback.
void NpSdk_PressPower(void);
const NpSdkConfig* NpSdk_GetConfig(void);

// ---- video (VI) -----------------------------------------------------------------------------------
// The SDK layer owns the retrace clock (59.94 Hz, 50 Hz for PAL) and the game's VI calls; the picture is
// the renderer's business. After every retrace (once the game's pre/post retrace callbacks ran) the hook
// is called from the retrace thread, outside the interrupt lock, with what the console would show now:
// the external frame buffer the game last latched with VISetNextFrameBuffer + VIFlush, and whether the
// game blanked the screen (VISetBlack). With NpSdk_SetViExternalRetrace(1) the internal clock is off and
// whoever owns the display (the Vita's vblank callback) calls NpVI_Retrace() instead.
typedef struct NpViFrame {
    void* xfb;               // external frame buffer (YUV422 on the console), NULL before the first latch
    int black;               // VISetBlack(1)
    uint32_t retrace_count;  // VIGetRetraceCount() after this retrace
    uint32_t tv_info;        // GXRenderModeObj.tvInfo of the current mode (format << 2 | scan mode)
    uint32_t fb_width, xfb_height, vi_width, vi_height;
    uint32_t vi_x_origin, vi_y_origin;
    int progressive;
    double refresh_hz;       // 59.94 / 50
} NpViFrame;
typedef void (*NpViRetraceHook)(const NpViFrame* frame);
void NpSdk_SetViRetraceHook(NpViRetraceHook hook);
void NpSdk_SetViExternalRetrace(int external);
void NpVI_Retrace(void);                       // one retrace (external clock only)
void NpVI_GetFrame(NpViFrame* out);

// ---- audio ---------------------------------------------------------------------------------------
// The AX library runs unchanged (compiled from the decomp); its DSP is the mixer in src/ax and the
// AI "DMA" is a thread that delivers one 3 ms block (96 stereo frames at 32 kHz) at a time. The sink
// is where the blocks go. write() may block until the device has room: that blocking is the audio
// clock, and every AX frame callback of the game (sound updates, reverb) follows it. The default
// sink discards the audio but keeps real time (and writes a WAV file when NP_AUDIO_WAV is set).
typedef struct NpAudioSink {
    int (*open)(uint32_t sample_rate);                           // returns 0 on success
    void (*write)(const int16_t* stereo_lr, uint32_t frames);    // interleaved left, right
    void (*close)(void);
} NpAudioSink;
void NpSdk_SetAudioSink(const NpAudioSink* sink);
typedef struct NpAudioStats {
    uint64_t blocks;             // 3 ms blocks delivered
    uint64_t mix_ns_total;       // time spent in the AX callback + mix (excluding sink waits)
    uint64_t mix_ns_max;
    uint32_t sample_rate;
} NpAudioStats;
void NpSdk_GetAudioStats(NpAudioStats* out);

// ---- controllers ----------------------------------------------------------------------------------
// The game reads Wii Remote + Nunchuk through WPAD/KPAD (and a GameCube pad only for the soft reset
// combination). The SDK layer emulates that controller from whatever the platform delivers as a
// "raw" pad, described in the Vita's terms (the same bits as SCE_CTRL_*, sticks 0..255 with 128 at
// the centre and y growing downwards, touch in display pixels of a 960x544 screen). The Vita
// platform layer installs a source that reads sceCtrl/sceTouch; the harness installs one driven by
// a script; with no source installed no controller is connected.
enum {
    NP_BTN_SELECT = 0x0001, NP_BTN_START = 0x0008, NP_BTN_UP = 0x0010, NP_BTN_RIGHT = 0x0020,
    NP_BTN_DOWN = 0x0040, NP_BTN_LEFT = 0x0080, NP_BTN_L = 0x0100, NP_BTN_R = 0x0200,
    NP_BTN_TRIANGLE = 0x1000, NP_BTN_CIRCLE = 0x2000, NP_BTN_CROSS = 0x4000, NP_BTN_SQUARE = 0x8000
};
#define NP_MAX_TOUCH 6
typedef struct NpRawPad {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;          // 128 = centre
    int touch_count;                 // front touch panel
    struct { int id; float x, y; } touch[NP_MAX_TOUCH];   // display pixels (960 x 544)
    int has_motion;
    float accel[3];                  // g, device frame (rest: 0, 0, -1 with the screen up)
    float gyro[3];                   // rad/s
    // W5 CTL: the rear touch panel (scaled to the front's 960 x 544, x growing to the right as seen from the front; 0 fingers
    // when it is off) and where the sample came from (NP_RAW_*: the player's controls, a GameCube-pad script, a Vita script).
    int rear_count;                  // -1 unavailable; zero is a verified release
    struct { int id; float x, y; } rear[NP_MAX_TOUCH];
    int source;
} NpRawPad;
enum { NP_RAW_LIVE = 0, NP_RAW_SCRIPT_GC = 1, NP_RAW_SCRIPT_VITA = 2, NP_RAW_BLOCKED = 3 };
// Fills `out` for pad number `pad` (0 = the player's own controller) and returns 1, or returns 0 when
// no such controller is connected. Called from the game thread, at most a few times per frame.
typedef int (*NpInputSourceFn)(int pad, NpRawPad* out);
void NpSdk_SetInputSource(NpInputSourceFn fn);
// Rumble requests of the game (WPADControlMotor / PADControlMotor): pad number, on/off.
typedef void (*NpRumbleFn)(int pad, int on);
void NpSdk_SetRumbleSink(NpRumbleFn fn);

// ---- the game-aware layer (platform/sdk/input, compiled with the game) ---------------------------------------
// Every time the emulated Wii Remote is sampled (once per VI retrace and channel; WPADRead and KPADRead of one
// frame share the sample) the SDK builds the plain sample from the raw pad and offers it to the symbol
// `void NpPadAdjust(NpPadSample*)` (weak: absent = the plain behaviour: the right stick moves the pointer, a touch
// places it and presses A). The game-aware layer lives in platform/sdk/input: menu navigation (D-pad and sticks
// move between the game's buttons), the touch minigames, and the controller-kind tag.
typedef struct NpPadSample {
    int chan;
    float dt;                  // seconds since this channel's previous sample (at most 0.1)
    const NpRawPad* raw;       // the controller this sample is made from (read only)
    uint32_t hold;             // in/out: WPAD button bits, including the controller tag (np_pad.h)
    float stick[2];            // in/out: Nunchuk stick, -1..1, +y up
    float free_stick[2];       // in: the right stick (radial dead zone applied), -1..1, +y up
    float cursor[2];           // in/out: the pointer that stays where it is left, KPAD space -1..1, +y down
    int pointer_hidden;        // out: report "no IR pointer" this sample
} NpPadSample;
// 1 = the platform owns the display (text console, system dialog): the controller stays connected but reads neutral.
void NpSdk_SetInputBlocked(int blocked);

#ifdef __cplusplus
}
#endif
#endif
