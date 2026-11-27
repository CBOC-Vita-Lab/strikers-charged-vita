// On-device LiveArea & Home-screen bubble branding from the player's own disc files.
// Generates the four 8-bit indexed PNGs matching tools/brand-from-disc.py (art33):
//   icon0.png   128 x 128   circular-safe Home bubble
//   bg.png      840 x 500   Mario vs Bowser LiveArea background
//   startup.png 280 x 158   steel-and-energy START gate
//   pic0.png    960 x 544   widescreen launch splash
// Nothing from the game is ever shipped in the package or repository.
#ifndef MSC_VITA_LIVEAREA_ART_H
#define MSC_VITA_LIVEAREA_ART_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VitaLiveAreaArt {

constexpr uint32_t kFormatVersion = 1;

struct BuiltArt {
    std::vector<uint8_t> icon0;
    std::vector<uint8_t> bg;
    std::vector<uint8_t> startup;
    std::vector<uint8_t> pic0;
};

// IEEE CRC-32 (used for PNG chunks and generic-art identity checks).
uint32_t Crc32(const uint8_t* data, size_t len);

// True when `data` matches one of the shipped generic CBOC-Vita icon0.png files (never true for custom art).
bool IsGenericIconBytes(const uint8_t* data, size_t len);

// Build all four 8-bit indexed PNGs from an extracted disc's `files` directory.
// Returns true when all four PNGs were generated and validated; `why` explains any failure.
bool BuildFromFilesDir(const char* filesDir, BuiltArt& out, char* why, size_t whyBytes, const volatile bool* cancel = nullptr);

#if defined(__vita__)
// Fast check (<1 ms) whether auto-branding should run on this boot.
bool NeedsApply();

// Build and apply the four 8-bit indexed PNGs on the console (runs on the low-priority boot-art thread).
// Respects MSC_LIVEAREA_ART=0 (off) and MSC_LIVEAREA_ART=force, and never overwrites custom user artwork.
void ApplyIfNeeded(const volatile bool* cancel);
#endif

}  // namespace VitaLiveAreaArt

#endif
