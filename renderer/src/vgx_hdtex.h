// HD texture replacement: a pack of higher-resolution, GPU-ready (DXT1/DXT5, with their own
// mip chains) versions of selected guest textures, keyed by a hash of the original texels.
// The texture cache (vgx_texcache.cpp) asks once per upload of a guest texture; nothing is
// done per frame or per bind. Pack format and rebuild command: tools/hdtex/README.md.
#pragma once
#include <cstdint>

namespace vgx::hdtex {

// Opens the pack and loads its index (the host calls this once at start-up, before the first
// frame; `log` is the host's line logger, may be null). Reads the switches:
//   MSC_HDTEX=0            off (the guest textures as they are)
//   MSC_HDTEX=1            on when the pack exists (default)
//   MSC_HDTEX=2            dry run: look every texture up and count the hits, upload the originals
//   MSC_HDTEX_PATH=<file>  the pack (default ux0:data/mscharged/hdtex.pack on the Vita, ./hdtex.pack elsewhere)
//   MSC_HDTEX_BUDGET=<MiB> most HD texture bytes alive in the back end at once; past it the
//                          original is used (default 8)
//   MSC_HDTEX_PRELOAD=<MiB> read the whole pack into memory at start-up when it is no larger
//                          (default 8: the 4.5 MiB pack is preloaded; 0: read each texture from
//                          the card when it is first uploaded)
//   MSC_HDTEX_TRILINEAR=1  sample replaced textures with GL_LINEAR_MIPMAP_LINEAR (default 0:
//                          GL_LINEAR_MIPMAP_NEAREST, the cost of the original bilinear fetch)
//   MSC_HDTEX_LOG=<n>      log the first n replacements by name (default 48)
//   MSC_HDTEX_FACES=1      replace the blink's eye frames too (<c>_eye0/1/2; default 0: the game's own, see Install)
void Install(void (*log)(const char* fmt, ...) __attribute__((format(printf, 1, 2))));
bool IsEyeFrameName(const char* names);   // (for the host test) the pack's name list of an entry names an eye frame

struct Counters {
    uint64_t hashed = 0;    // uploads whose format and size occur in the pack (their texels were hashed)
    uint64_t hits = 0;      // replaced (or, MSC_HDTEX=2, would have been)
    uint64_t refused = 0;   // found but over MSC_HDTEX_BUDGET
    uint64_t bytesRead = 0; // pack bytes read for the hits
    uint64_t lookupNs = 0;  // hashing + index search of the hashed uploads
    uint64_t readNs = 0, readMaxNs = 0; // reading the hits' data
    uint64_t peakLive = 0;  // most HD bytes alive at once
    uint32_t entries = 0;   // textures in the pack (0: no pack / off)
};
void GetCounters(Counters& c);
int Requested();   // MSC_HDTEX as read: 0 off, 1 on, 2 dry run (the run header)
int Mode();        // what is in force: 0 off or no usable pack, 1 replacing, 2 dry run

} // namespace vgx::hdtex
