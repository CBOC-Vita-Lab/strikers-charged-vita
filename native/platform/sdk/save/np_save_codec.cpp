// np_save_codec.cpp: see np_save_codec.h. The layout tables are generated (np_save_layout.inc).
#include "np_save_codec.h"

#include <string.h>

namespace {

struct NpSaveRun {
    uint16_t off;
    uint16_t count;
    uint8_t kind;   // 2 / 4 / 8: values to byte-swap; 'h' / 'w': one 16 / 32 bit bit-field unit
    uint8_t bf;     // index into kBitUnits for the bit-field units
};

#include "np_save_layout.inc"

inline uint16_t ld16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t ld32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline void st16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
inline void st32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
inline uint32_t bs32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t bs16(uint16_t v) { return __builtin_bswap16(v); }

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define NP_SAVE_HOST_IS_FILE_ORDER 1
#else
#define NP_SAVE_HOST_IS_FILE_ORDER 0
#endif

void swap_n(uint8_t* p, unsigned size, unsigned count) {
    if (NP_SAVE_HOST_IS_FILE_ORDER) return;
    for (unsigned i = 0; i < count; ++i, p += size) {
        if (size == 2) st16(p, bs16(ld16(p)));
        else if (size == 4) st32(p, bs32(ld32(p)));
        else {
            uint8_t t[8];
            for (unsigned k = 0; k < size; ++k) t[k] = p[size - 1 - k];
            memcpy(p, t, size);
        }
    }
}

// A bit-field unit: the host (clang, little endian) allocates the fields from the least significant bit in declaration
// order; the Wii's compiler (Metrowerks) from the most significant bit, and the unit is stored big-endian.
void bf_to_wii(uint8_t* p, unsigned unit, const uint8_t* widths) {
    const uint32_t v = unit == 4 ? ld32(p) : ld16(p);
    uint32_t w = 0;
    unsigned pos = 0, top = unit * 8;
    for (unsigned i = 0; i < 12 && widths[i]; ++i) {
        const unsigned wd = widths[i];
        const uint32_t field = (v >> pos) & ((1u << wd) - 1u);
        top -= wd;
        w |= field << top;
        pos += wd;
    }
    if (unit == 4) st32(p, bs32(w)); else st16(p, bs16((uint16_t)w));
}
void bf_to_host(uint8_t* p, unsigned unit, const uint8_t* widths) {
    const uint32_t w = unit == 4 ? bs32(ld32(p)) : bs16(ld16(p));
    uint32_t v = 0;
    unsigned pos = 0, top = unit * 8;
    for (unsigned i = 0; i < 12 && widths[i]; ++i) {
        const unsigned wd = widths[i];
        top -= wd;
        const uint32_t field = (w >> top) & ((1u << wd) - 1u);
        v |= field << pos;
        pos += wd;
    }
    if (unit == 4) st32(p, v); else st16(p, (uint16_t)v);
}

void apply(uint8_t* base, const NpSaveRun* runs, size_t n, bool to_wii) {
    for (size_t i = 0; i < n; ++i) {
        const NpSaveRun& r = runs[i];
        uint8_t* p = base + r.off;
        if (r.kind == 'h' || r.kind == 'w') {
            const unsigned unit = r.kind == 'h' ? 2 : 4;
            if (NP_SAVE_HOST_IS_FILE_ORDER) continue;   // (a big-endian host keeps the file's order and bit order)
            if (to_wii) bf_to_wii(p, unit, kBitUnits[r.bf]); else bf_to_host(p, unit, kBitUnits[r.bf]);
        } else {
            swap_n(p, r.kind, r.count);
        }
    }
}

#define APPLY(base, table, to) apply((base), table, sizeof(table) / sizeof(table[0]), (to))

// One cup series as SerializeData writes it: BaseCup's six fields one after the other (int, 3 x int, int, s16, s16, u16:
// 26 bytes, no padding), the match descriptors, the table, the previous table.
uint8_t* cup(uint8_t* p, bool to_wii, unsigned games, unsigned teams) {
    swap_n(p, 4, 5);
    swap_n(p + 20, 2, 3);
    p += 26;
    for (unsigned i = 0; i < games; ++i, p += kBasicGameInfoSize) APPLY(p, kRunsBasicGameInfo, to_wii);
    for (unsigned i = 0; i < teams + 1; ++i, p += kTeamStatsSize) APPLY(p, kRunsTeamStats, to_wii);
    return p;
}

constexpr unsigned kFireGames = 8 * (4 / 2), kFireTeams = 4;
constexpr unsigned kCrystalGames = 12 * (6 / 2), kCrystalTeams = 6;
constexpr unsigned kStrikerGames = 11 * (10 / 2), kStrikerTeams = 10;
constexpr size_t kCupBytes(unsigned games, unsigned teams) { return 26 + (size_t)games * 296 + (size_t)(teams + 1) * 112; }
constexpr size_t kSettingsBytes = 0x80 + 0x90 + kCupBytes(kFireGames, kFireTeams) + kCupBytes(kCrystalGames, kCrystalTeams) +
                                  kCupBytes(kStrikerGames, kStrikerTeams) + 928 + 52;
constexpr size_t kOnlineBytes = 10 * 2832;

void settings(uint8_t* p, bool to_wii) {
    APPLY(p, kRunsUserInfo, to_wii);        // mUserInfo, 0x80 bytes
    p += 0x80;
    swap_n(p, 4, 36);                       // mRulesTable: 12 x CupSidekicks (3 ints)
    p += 0x90;
    p = cup(p, to_wii, kFireGames, kFireTeams);
    p = cup(p, to_wii, kCrystalGames, kCrystalTeams);
    p = cup(p, to_wii, kStrikerGames, kStrikerTeams);
    APPLY(p, kRunsCupProgress, to_wii);     // CupManager mState .. mCurrentMode, 928 bytes
    p += 928;
    APPLY(p, kRunsChallengeUnlockRecord, to_wii);   // StrikerChallenge: 12 completion dates + the unlocked mask
}

void online(uint8_t* p, bool to_wii) {
    for (unsigned i = 0; i < 10; ++i, p += kGameInfoSaveSlotSize) APPLY(p, kRunsGameInfoSaveSlot, to_wii);
}

static_assert(kUserInfoSize == 0x80, "UserInfo is 0x80 bytes");
static_assert(kBasicGameInfoSize == 296, "BasicGameInfo is 0x128 bytes");
static_assert(kTeamStatsSize == 112, "TeamStats is 0x70 bytes");
static_assert(kCupProgressSize == 928, "CupManager::GetProgressDataSize is 0x3A0");
static_assert(kChallengeUnlockRecordSize == 52, "ChallengeUnlockRecord is 0x34 bytes");
static_assert(kGameInfoSaveSlotSize == 2832, "GameInfoSaveSlot is 0xB10 bytes");
static_assert(kSettingsBytes == 35578, "the Strikers2 payload of the retail game is 35578 bytes");

}  // namespace

extern "C" {

size_t NpSaveSettingsPayloadSize(void) { return kSettingsBytes; }
size_t NpSaveOnlinePayloadSize(void) { return kOnlineBytes; }
void NpSaveSettingsToWii(void* payload) { settings((uint8_t*)payload, true); }
void NpSaveSettingsFromWii(void* payload) { settings((uint8_t*)payload, false); }
void NpSaveOnlineToWii(void* payload) { online((uint8_t*)payload, true); }
void NpSaveOnlineFromWii(void* payload) { online((uint8_t*)payload, false); }

uint32_t NpSaveBe32(uint32_t v) { return NP_SAVE_HOST_IS_FILE_ORDER ? v : bs32(v); }

// NANDBanner: u32 magic, u32 flags, u16 iconSpeed, u8 reserved[0x16], u16 title[32], u16 subtitle[32], then the textures
// (bytes: the game copies the TPL payloads, which stay in file order). 0xA0 bytes of header.
void NpSaveBannerToWii(const void* host, void* out) {
    memcpy(out, host, NP_SAVE_BANNER_SIZE);
    uint8_t* p = (uint8_t*)out;
    swap_n(p, 4, 2);
    swap_n(p + 8, 2, 1);
    swap_n(p + 0x20, 2, 64);   // title and subtitle
}

}  // extern "C"
