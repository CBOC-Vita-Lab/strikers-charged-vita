// vgx native library on the PS Vita: what the platform layer (NPV: np/platform/vita) plugs into the renderer's Vita host.
//
// The host (vita/vgx_host_vita.cpp, compiled with -DVGX_NATIVE) starts vitaGL, owns the render thread and presents the picture. It needs a few
// services from the platform layer: the text console that takes the display on a fatal error, the core plan for its threads, the pad for
// its debug chord (SELECT+START+D-pad) and a boot-progress / memory log. All are optional; without them the host runs standalone
// (threads on cores 1 and 2, no console, no debug chord).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

enum { VGXN_ROLE_COLLECTOR = 0, VGXN_ROLE_DISPLAY_QUEUE = 1, VGXN_ROLE_PLAYER = 2, VGXN_ROLE_RENDER = 3 };

typedef struct vgxn_vita_services {
    unsigned int size;                                  // sizeof(vgxn_vita_services)
    void (*boot_stage)(const char* what);               // progress line for the platform's boot screen / log
    void (*mem_checkpoint)(const char* what);           // memory report point
    int (*core_mask)(int role);                         // SceKernel CPU affinity mask for the role (0 = leave to the system)
    void (*pin_current_thread)(int role);               // move the calling thread onto the role's core
    int (*console_owns_display)(void);                  // a text console / fatal screen has the display: do not touch it
    void (*console_set_display_owner)(int console);     // hand the display to the console (1) or the renderer (0)
    const unsigned int* (*console_pixels)(void);        // 960 x 544 ARGB picture of the console (shown with the debug chord), or NULL
    int (*read_buttons)(unsigned int* sce_ctrl_buttons); // raw SCE_CTRL_* buttons of the pad, 1 when valid
    // W5 BOOT (appended; an older platform's smaller structure leaves them NULL): the platform's loading screen.
    int (*boot_compose)(void* framebuffer);             // display callback: draw the loading screen into this 960x544 buffer; 1 = drawn
    void (*boot_progress)(int kind, unsigned done, unsigned total);   // VGXN_BOOT_*: seed copy, shader warm-up, presents
} vgxn_vita_services;
enum { VGXN_BOOT_SEED_COPY = 1, VGXN_BOOT_WARMUP = 2, VGXN_BOOT_PRESENT = 3 };

// Before vgxn_init. The structure is copied.
void vgxn_vita_set_services(const vgxn_vita_services* services);

#ifdef __cplusplus
}
#endif
