#pragma once
// The surface of the AX mix (ax_mix.cpp) towards the DSP / AI layer of platform/sdk (dsp_hle.cpp).
// Plain types only: this directory does not include the Wii SDK headers.
#include <cstddef>
#include <cstdint>

namespace AxDspHle {

// Resets the mixer and the mail state machine; queues the DSP's "initialised" mail.
void Init();
// Detects the microcode flavour (parameter block and command list layouts) from its image.
void ConfigureFromIram(const uint8_t* iram, uint32_t length);
void SendMailToDSP(uint32_t mail);
uint32_t CheckMailToDSP();
uint32_t CheckMailFromDSP();
uint32_t ReadMailFromDSP();
// The DSP asks the CPU for a resume callback (what the DSP's yield interrupt does after a mix).
void QueueResumeCallback();
// Runs `resume` once per queued resume callback.
void SetResumeCallback(void (*resume)());
void ServiceDeferredCallbacks();
// Mixes the command list the last mail pair delivered (if any) and publishes the aux outputs.
// Called by the audio thread after the AI callback returned, outside the interrupt lock.
void RunPendingMix();
void Stop();

} // namespace AxDspHle
