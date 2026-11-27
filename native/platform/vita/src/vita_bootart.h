// The boot loading screen's own picture, made on the console from the player's own game files (art25).
//
// A port of the loading picture's path of tools/brand-from-disc.py (make_loading): the start-screen picture of files/Art/fe/mainui.dmn
// (a bundle of GX textures) and the logo of files/opening.bnr (IMET banner, U8, IMD5, LZ77, TPL), cropped, scaled, blurred and composited
// into the 960 x 544 picture of boot/loading.bin (see vita_bootscreen.cpp). The arithmetic follows Pillow's (the resampling filters with
// their 22-bit coefficients, the box-blur Gaussian, the 8-bit paste and alpha compositing), so the result is byte for byte the tool's.
//
// Pure code: no console calls, no files of its own (the caller gives readers), so the host builds and tests it. Nothing of the game is in
// this file or the repository; the picture exists only in memory and in the player's own cache file.
#pragma once

#include <cstddef>
#include <cstdint>

namespace VitaBootArt {

constexpr int kWidth = 960, kHeight = 544;
constexpr size_t kHeaderBytes = 12;                                    // "NPBS", u16 w, u16 h, u32 format 1 (RGB565 little-endian)
constexpr size_t kPixelBytes = static_cast<size_t>(kWidth) * kHeight * 2;
constexpr size_t kPictureBytes = kHeaderBytes + kPixelBytes;           // the whole of boot/loading.bin
constexpr size_t kStampBytes = 16;                                     // the cache file's trailer: "NPAS", version, sizes of the two game files
constexpr size_t kCacheBytes = kPictureBytes + kStampBytes;
constexpr uint32_t kFormatVersion = 1;                                 // bump when the picture the code makes changes

// A game file read in ranges. read() returns false when the bytes are not all there.
struct Source {
    bool (*read)(void* context, uint64_t offset, void* destination, size_t length) = nullptr;
    void* context = nullptr;
    uint64_t size = 0;
};

struct Stats {
    size_t peakBytes = 0;   // the most heap the build held at one moment (all temporaries; the result is the caller's buffer)
};

// Builds the picture into out (outBytes >= kPictureBytes: the header and the pixels; a cache buffer of kCacheBytes also takes the stamp).
// False with a short reason in why (never empty) when a file is missing or not what it should be, when memory is short, or when *cancel
// turns true (checked between steps and every few rows).
bool Build(const Source& bundle, const Source& banner, uint8_t* out, size_t outBytes, char* why, size_t whyBytes, Stats* stats = nullptr,
           const volatile bool* cancel = nullptr);

// The cache file's stamp: out[kStampBytes]. The cache is current when the stamp of the game files now equals the one stored.
void WriteStamp(uint8_t* out, uint64_t bundleSize, uint64_t bannerSize);
bool StampMatches(const uint8_t* trailer, uint64_t bundleSize, uint64_t bannerSize);

} // namespace VitaBootArt
