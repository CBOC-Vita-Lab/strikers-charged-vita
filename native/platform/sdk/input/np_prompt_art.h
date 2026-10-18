// np_prompt_art.h: the game's button pictures in the Vita's buttons (W5 CTL).
//
// The front end draws some prompts as pictures: the title screen's "PRESS (A) TO START" is the text "PRESS " + a texture of the Wii
// Remote's A button + " TO START"; the HUD and the menus show the B trigger, the D-pad, the + and - buttons and a Wii Remote; the pause
// menu's controller map is a photo of the Wii Remote and Nunchuk with labels. When the game loads one of those textures (glx_MakeTexture,
// by the hash of its name) this module gives a replacement of the same size, format (CMPR) and mip levels, drawn by np_art.h: the original
// button with a PlayStation symbol instead of its letter, or a new picture. The pictures are our own drawings (no Sony artwork).
// Pure code: no game headers (np_prompts.cpp is the glue). MSC_PROMPTS=0 keeps every original.
#ifndef NP_PROMPT_ART_H
#define NP_PROMPT_ART_H

#include <stddef.h>
#include <stdint.h>

#include "np_control_table.h"

namespace NpPromptArt {

// The game's GXTextureHeader (0x20 bytes, native-endian when the game makes the texture); the texels follow it.
struct TexHeader {
    uint32_t numLevels;
    uint32_t format;   // eGXTextureFormat (2 = CMPR)
    uint8_t numBits[4];
    uint8_t missingTexture;
    uint8_t pad0D;
    uint16_t width;
    uint16_t height;
    uint8_t pad12[2];
    uint32_t numEntries;
    uint32_t pad18[2];
};
static_assert(sizeof(TexHeader) == 0x20, "GXTextureHeader is 0x20 bytes");

// What a replaced texture shows (for the log and the tests).
const char* Describe(uint32_t hash);
// True when `hash` is one of the textures this module replaces (whatever the layout).
bool Known(uint32_t hash);

// The replacement of texture `hash` for the player's controls (layout, rear touchpad, touch switches: np_control_table.h Config): a malloc'd
// blob (header + texels, `*bytes` long, the same size as the original's) the caller frees, or 0 to keep the original (not one of ours, or
// not the size/format we know). `header` and `texels` are the original.
uint8_t* Build(uint32_t hash, const TexHeader* header, const uint8_t* texels, const NpControlTable::Config& config, size_t* bytes);

}  // namespace NpPromptArt

#endif  // NP_PROMPT_ART_H
