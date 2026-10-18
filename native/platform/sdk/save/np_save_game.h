// np_save_game.h: the game-facing half of the save codec (np_save_codec.h): hooks for src/Game/DB/SaveLoad.cpp.
#ifndef NP_SAVE_GAME_H
#define NP_SAVE_GAME_H

#include <stdint.h>

extern "C" {
// The payload of the settings (online = false) or friend-slot (online = true) file as the game's Serialize / GetMemoryCardData
// wrote it (host structures), `size` bytes; converts it in place to the file's format / back. A size that does not match the
// layout the codec was generated for is refused (nothing is converted, a line goes to the log) instead of corrupting memory.
void NpSavePayloadToWii(void* payload, int online, unsigned long size);
void NpSavePayloadFromWii(void* payload, int online, unsigned long size);
// SaveFileHeader words (Size, CRC) are big-endian in the file.
uint32_t NpSaveBe32(uint32_t v);
// A converted copy of the game's NANDBanner (61600 bytes), valid until the next call; write this, not the game's buffer.
const void* NpSaveBannerShadow(const void* hostBanner);
}

#endif
