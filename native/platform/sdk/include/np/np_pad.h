// np_pad.h: controller details for game-side patches (needs the SDK headers; np_sdk.h does not).
#ifndef NP_PAD_H
#define NP_PAD_H

#include <revolution/pad.h>

#ifdef __cplusplus
extern "C" {
#endif

// The player's controller on `chan` as a GameCube pad (buttons placed by position on the Vita: cross =
// A, square = B, circle = X, triangle = Y, L/R digital + full analog, select = Z, start = START),
// sticks in the GameCube's +-72 units. Returns 0 when no controller is connected on `chan`.
// The emulated Wii Remote tags its button word with NP_GC_TAG | (chan << NP_GC_CHAN_SHIFT) (bits no
// Wii button uses) so that a patched DetInput can recognise "this player holds a gamepad" and switch
// to the engine's GameCube controls, as the recompiled build did; see docs/sdk.md ("Input").
#define NP_GC_TAG 0x0080
#define NP_GC_CHAN_SHIFT 5
int NpPad_ReadGameCube(int chan, PADStatus* out);
// W5 CTL: the raw controls behind the same sample (one per retrace and channel) that NpPad_ReadGameCube and the emulated
// remote were made from, after a script replaced them (NpRawPad.source says so). Returns 0 when no controller is connected.
struct NpRawPad;
int NpPad_ReadRaw(int chan, struct NpRawPad* out);

#ifdef __cplusplus
}
#endif
#endif
