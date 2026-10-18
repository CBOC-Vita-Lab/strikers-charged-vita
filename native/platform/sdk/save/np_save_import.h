// First start of the native game: take over the save of the recompiled build.
//
// Both builds keep the same save format and the same NAND layout (docs/wave2-NPS2.md section 3), but the native build's data directory (NP_DATA_DIR,
// ux0:data/mscharged/np-data on the console) is not the recompiled build's (ux0:data/mscharged), so the native game starts with no save. When it has none and the
// recompiled build has one, NpSaveImportRecompiled COPIES it (Strikers2 and banner.bin of NAND/title/00010000/52345145/data) into the native data directory: the
// original is only ever opened for reading, never changed or moved; the native build keeps its own copy from then on (the two saves part ways afterwards). The
// source must pass the game's own file checks first (size, header, CRC-32), so a damaged file is not copied into a place where it would make the game offer to
// delete it. Each file is written under a temporary name and renamed when complete, Strikers2 last: a copy cut short leaves no Strikers2, and the next start tries again.
#ifndef NP_SAVE_IMPORT_H
#define NP_SAVE_IMPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum NpSaveImportResult {
    NP_SAVE_IMPORT_NOT_NEEDED = 0,   // the native save exists, or both builds already use one data directory
    NP_SAVE_IMPORT_NO_SOURCE = 1,    // the recompiled build has no save
    NP_SAVE_IMPORT_DONE = 2,         // copied
    NP_SAVE_IMPORT_BAD_SOURCE = 3,   // its Strikers2 fails the checks: nothing was copied
    NP_SAVE_IMPORT_FAILED = 4        // a file could not be written
};

// sourceRoot: the directory that holds the recompiled build's NAND/ (ux0:data/mscharged); destRoot: NP_DATA_DIR. message (may be null) gets one log line.
int NpSaveImportRecompiled(const char* sourceRoot, const char* destRoot, char* message, size_t messageSize);

#ifdef __cplusplus
}
#endif
#endif
