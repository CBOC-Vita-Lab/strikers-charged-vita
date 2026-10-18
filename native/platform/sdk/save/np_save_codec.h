// np_save_codec.h: the game's own save format on a little-endian host.
//
// The game's save files (Strikers2 = settings and progress, Online = friend slots, banner.bin = the Wii Menu banner) are
// memory images of its structures, written by a big-endian machine whose compiler packs bit-fields from the most significant
// bit. The native game builds the same images from little-endian memory. The codec converts those images, in place, between
// the host representation and the file's: after NpSave*ToWii the buffer is byte for byte what the Wii's game wrote, and
// NpSave*FromWii is its inverse. Pure C++ (no game headers), so it can be tested against real saves without the game.
// The layouts come from tools/gen_save_layout.py (np_save_layout.inc, generated from the decomp's headers).
#ifndef NP_SAVE_CODEC_H
#define NP_SAVE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The settings/progress payload ("Strikers2", after the 8 byte SaveFileHeader): 35578 bytes. The Online payload: ten slots.
size_t NpSaveSettingsPayloadSize(void);
size_t NpSaveOnlinePayloadSize(void);
void NpSaveSettingsToWii(void* payload);
void NpSaveSettingsFromWii(void* payload);
void NpSaveOnlineToWii(void* payload);
void NpSaveOnlineFromWii(void* payload);

// SaveFileHeader words are big-endian in the file.
uint32_t NpSaveBe32(uint32_t v);

// banner.bin: NAND_BANNER_SIZE(8) = 61600 bytes. `host` is the game's NANDBanner (host byte order, 16 bit wchar_t),
// `out` receives the file image (a copy: the game keeps filling its own buffer).
#define NP_SAVE_BANNER_SIZE 61600
void NpSaveBannerToWii(const void* host, void* out);

#ifdef __cplusplus
}
#endif
#endif
