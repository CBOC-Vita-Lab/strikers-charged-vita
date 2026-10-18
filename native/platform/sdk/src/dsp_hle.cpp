// AI (audio interface) and DSP at the level the AX library uses them.
//
// The AX library (ref/RVL_SDK/ax, compiled unchanged) talks to the DSP through a task, mails and a
// resume callback, and to the AI through a DMA callback that fires when a 3 ms block (96 stereo
// frames at 32 kHz) has been played. Here:
//   DSPAddTask      the task's microcode image tells the mixer which AXWii flavour it is (src/ax),
//                   the init callback runs at once (the DSP's "booted" mail)
//   DSPSendMailToDSP  the AX mail protocol (command list size, command list address) goes to the
//                   mixer, which keeps the list; mixing happens after the callback (see below)
//   AIStartDMA      starts the audio thread: it hands each block to the audio sink (whose blocking
//                   is the audio clock), then plays the part of the "DMA complete interrupt": the AX
//                   frame callback runs under the interrupt lock, the pending command list is mixed
//                   OUTSIDE the lock (the game thread is never held up by the mixer), and the DSP's
//                   "yield" resume callback follows.
// Behaviour follows the recomp's HLE (hle/audio/audio.cpp: AI registers, task and mail handling).
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/os.h>
extern "C" {
#include <revolution/dsp.h>   // (no extern "C" of its own: the SDK sources that use it are C)
}

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ax/ax_dsp.h"
#include "audio_out.h"
#include "np_internal.h"

namespace {

struct Ai {
    int inited;
    int started;
    AIDMACallback cb;
    const void* buf;
    uint32_t len;
    uint32_t rate;
    uint64_t block_ns;
    NpThread* thread;
    NpAudioStats stats;
};
Ai g_ai;

DSPTask* g_task = 0;
int g_dsp_inited = 0;

void resume_trampoline() {
    DSPTask* t = g_task;
    if (t && t->resumeCallback) t->resumeCallback(t);
}

// One block: hand it to the sink, then play the part of the "DMA complete interrupt" (the AX frame callback under the
// interrupt lock, the mix outside it, the deferred DSP callbacks). Returns the block's duration in ns, 0 when there was
// no buffer (the caller waits a block's time and looks again).
uint64_t audio_block() {
    const NpAudioSink* sink = np_audio_sink();
    static int16_t out[1024 * 2];
    np_irq_enter();
    const int16_t* buf = (const int16_t*)g_ai.buf;
    uint32_t len = g_ai.len;
    AIDMACallback cb = g_ai.cb;
    g_ai.block_ns = np_time_ns();
    np_irq_leave();
    uint32_t frames = len / 4;
    if (!buf || frames == 0 || frames > 1024) return 0;
    // the mixer's AI frame is right, left, right, left, ...: the sink takes left, right
    for (uint32_t i = 0; i < frames; ++i) {
        out[i * 2] = buf[i * 2 + 1];
        out[i * 2 + 1] = buf[i * 2];
    }
    sink->write(out, frames);

    uint64_t t0 = np_time_real_ns();
    // "the DMA block finished": the AX frame callback
    np_irq_enter();
    if (cb) cb();
    np_irq_leave();
    AxDspHle::RunPendingMix();
    np_irq_enter();
    AxDspHle::ServiceDeferredCallbacks();
    np_irq_leave();
    uint64_t dt = np_time_real_ns() - t0;
    g_ai.stats.blocks++;
    g_ai.stats.mix_ns_total += dt;
    if (dt > g_ai.stats.mix_ns_max) g_ai.stats.mix_ns_max = dt;
    return (uint64_t)frames * 1000000000ull / (g_ai.rate ? g_ai.rate : 32000);
}

void* audio_thread(void*) {
    const NpAudioSink* sink = np_audio_sink();
    if (sink->open(g_ai.rate) != 0) {
        np_log("audio", "the audio sink failed to open: no sound");
    }
    for (;;) {
        if (!audio_block()) np_sleep_ns(3000000);
    }
    return 0;
}

// Virtual time: the audio blocks are an event source on the clock's grid (the sink does not pace them).
int g_vic_id = -1;
void vic_audio(void*, uint64_t due) {
    uint64_t ns = audio_block();
    if (!ns) ns = 3000000;
    uint64_t next = due + ns;
    const uint64_t now = np_vt_now();
    if (now > next + 16 * ns) next = now;   // stalled: no burst of catch-up blocks
    np_vic_arm(g_vic_id, next);
}

}  // namespace

extern "C" {

void NpSdk_GetAudioStats(NpAudioStats* out) {
    *out = g_ai.stats;
    out->sample_rate = g_ai.rate;
}

// ---- DSP ---------------------------------------------------------------------------------------

void DSPInit(void) {
    if (g_dsp_inited) return;
    g_dsp_inited = 1;
    AxDspHle::SetResumeCallback(resume_trampoline);
    AxDspHle::Init();
}
BOOL DSPCheckInit(void) { return g_dsp_inited; }

DSPTask* DSPAddTask(DSPTask* task) {
    if (!task) return 0;
    if (!g_dsp_inited) DSPInit();
    BOOL en = OSDisableInterrupts();
    AxDspHle::ConfigureFromIram((const uint8_t*)task->iramMmemAddr, task->iramMmemLen);
    g_task = task;
    task->next = task->prev = 0;
    task->flags = DSP_TASK_ACTIVE;
    task->state = DSP_TASK_STATE_1;
    __DSP_first_task = __DSP_curr_task = __DSP_last_task = task;
    OSRestoreInterrupts(en);
    // the DSP boots the microcode and mails DSP_INIT: the SDK's handler calls the init callback
    if (task->initCallback) task->initCallback(task);
    return task;
}

DSPTask* DSPAssertTask(DSPTask* task) {
    if (task) {
        BOOL en = OSDisableInterrupts();
        AxDspHle::QueueResumeCallback();
        OSRestoreInterrupts(en);
    }
    return task;
}

void DSPSendMailToDSP(DSPMail mail) { AxDspHle::SendMailToDSP((uint32_t)(uintptr_t)mail); }
BOOL DSPCheckMailToDSP(void) { return AxDspHle::CheckMailToDSP() ? TRUE : FALSE; }
BOOL DSPCheckMailFromDSP(void) { return AxDspHle::CheckMailFromDSP() ? TRUE : FALSE; }
DSPMail DSPReadMailFromDSP(void) { return (DSPMail)(uintptr_t)AxDspHle::ReadMailFromDSP(); }
void DSPAssertInt(void) {}

BOOL __DSP_rude_task_pending;
DSPTask* __DSP_rude_task;
DSPTask* __DSP_tmp_task;
DSPTask* __DSP_last_task;
DSPTask* __DSP_first_task;
DSPTask* __DSP_curr_task;

// ---- AI ----------------------------------------------------------------------------------------

void AIInit(void* stack) {
    (void)stack;
    if (g_ai.inited) return;
    np_os_lazy_init();
    g_ai.inited = 1;
    g_ai.rate = 32000;
    g_ai.cb = 0;
}
BOOL AICheckInit(void) { return g_ai.inited; }

AIDMACallback AIRegisterDMACallback(AIDMACallback cb) {
    BOOL en = OSDisableInterrupts();
    AIDMACallback old = g_ai.cb;
    g_ai.cb = cb;
    OSRestoreInterrupts(en);
    return old;
}

void AIInitDMA(void* buffer, u32 length) {
    BOOL en = OSDisableInterrupts();
    g_ai.buf = buffer;
    g_ai.len = length & ~31u;
    OSRestoreInterrupts(en);
}

void AIStartDMA(void) {
    if (g_ai.started) return;
    g_ai.started = 1;
    if (np_vt_on) {
        const NpAudioSink* sink = np_audio_sink();
        if (sink->open(g_ai.rate) != 0) np_log("audio", "the audio sink failed to open: no sound");
        g_vic_id = np_vic_add("audio", vic_audio, 0);
        np_vic_arm(g_vic_id, np_vt_now() + 3000000ull);
        return;
    }
    g_ai.thread = np_thread_create("audio", NP_ROLE_AUDIO, audio_thread, 0, 0, 262144);
    if (g_ai.thread) np_thread_detach(g_ai.thread);
    else np_log("audio", "cannot start the audio thread");
}

u32 AIGetDMABytesLeft(void) {
    uint32_t len = g_ai.len;
    if (!len || !g_ai.block_ns) return 0;
    uint64_t el = np_time_ns() - g_ai.block_ns;
    uint64_t played = el * g_ai.rate / 1000000000ull * 4;
    if (played >= len) return 0;
    return (u32)((len - played) & ~31u);
}
// The console returns a physical address and callers add 0x80000000 (THP: AIGetDMAStartAddr() + 0x80000000);
// the subtraction makes that round trip give the host pointer back whatever the buffer's address is.
u32 AIGetDMAStartAddr(void) { return (u32)(uintptr_t)g_ai.buf - 0x80000000u; }
u32 AIGetDMALength(void) { return g_ai.len; }
void AISetDSPSampleRate(u32 rate) { g_ai.rate = rate == AI_DSP_48KHZ ? 48000 : 32000; }
u32 AIGetDSPSampleRate(void) { return g_ai.rate == 48000 ? AI_DSP_48KHZ : AI_DSP_32KHZ; }
void __AIDHandler(s16 intr, OSContext* ctx) { (void)intr; (void)ctx; }

}  // extern "C"
