// MSC_DVD_KEEP_KIB (1.3, "no loading in play"): a small RAM cache of whole disc files, so that the files a match reads after kick-off (the goal and Mega Strike
// cut-scene data, the characters' animations, the defence HUD) are read from memory and not from the memory card in the middle of play.
//   NpDvdKeepBegin()      a new match: files kept for an earlier one may be dropped to make room
//   NpDvdKeepHint(path)   ask for the file to be kept; it is read whole by a background thread (the host harness' virtual clock: at once). Returns 1 if the file is
//                         one that can be kept (a .nis, .sanim or .fen within MSC_DVD_KEEP_FILE_KIB), 0 otherwise. Nothing is read on the caller's thread.
//   NpDvdKeepStats(...)   files and bytes held, reads answered, misses of keepable files; returns whether the cache is on.
// All three are harmless with the cache off (MSC_DVD_KEEP_KIB unset or 0). Reads only: no game state, no random numbers.
#ifndef NP_DVD_KEEP_H
#define NP_DVD_KEEP_H
#ifdef __cplusplus
extern "C" {
#endif
void NpDvdKeepBegin(void);
// Files dropped from the cache so far (to make room, or given back to the heap). A line-up's hand-over asked for earlier is only still whole when this has not moved.
unsigned NpDvdKeepDrops(void);
int NpDvdKeepHint(const char* path);
int NpDvdKeepHintDir(const char* path);   // every file directly inside the directory (any type), in table order; returns how many were asked for
int NpDvdKeepStats(unsigned* files, unsigned* bytes, unsigned* hits, unsigned* misses);
#ifdef __cplusplus
}
#endif
#endif
