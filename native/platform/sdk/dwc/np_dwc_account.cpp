// The DWC user-data block (the first 64 bytes of a game save slot): create, check, dirty flag, and the profile id written after login.
//
// Layout (DWCAccUserData, native/libs/RVL_SDK/include/dwc/dwc_account.h) and CRC as in the decomp's dwc_account.c: the CRC-32 (IEEE,
// poly 0xEDB88320) of the first 60 bytes taken as little-endian words. The decomp's DWCi_Np_ToLE swaps each word because the console is
// big-endian; this port's save slots live in host (little-endian) order, so the memory already is the byte sequence the CRC is taken
// over, and the value is the one the console computed for the same slot (np_save_codec converts the words to and from the file).
#include <dwc/dwc_account.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "platform/sdk/dwc/np_dwc.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the user-data CRC is taken over little-endian memory"
#endif

namespace {

static_assert(sizeof(DWCAccUserData) == 64, "DWCAccUserData is 64 bytes");
static_assert(offsetof(DWCAccUserData, gs_profile_id) == 0x1C, "profile id at 0x1C");
static_assert(offsetof(DWCAccUserData, crc32) == 0x3C, "CRC at 0x3C");

uint32_t g_table[256];
bool g_tableReady = false;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_gameCode = 0;

void InitTable() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i;
        for (int j = 0; j < 8; ++j) r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1;
        g_table[i] = r;
    }
    g_tableReady = true;
}

const u32 kFlagsShift = DWC_ACC_FLAGS_SHIFT;                  // the data type lives in the low bits of the flags, flags start at bit 11
const u32 kFlagsMask = DWC_ACC_FLAGS_MASK;

u32 DataType(const DWCAccLoginId& id) { return ((id.id_data >> kFlagsShift) & kFlagsMask) & DWC_ACC_FRIENDDATA_MASK; }

void SetDataType(DWCAccLoginId* id, u32 type) {
    u32 flags = (id->id_data >> kFlagsShift) & kFlagsMask;
    flags = (flags & ~DWC_ACC_FRIENDDATA_MASK) | type;
    id->id_data = (id->id_data & ~(kFlagsMask << kFlagsShift)) | (flags << kFlagsShift);
}

void Recompute(DWCAccUserData* user) { user->crc32 = NpDwcCrc32(user, sizeof(DWCAccUserData) - 4); }

// A random pseudo login id: not derived from anything of the console (the service identifies the player by token alone).
uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

}  // namespace

uint32_t NpDwcCrc32(const void* data, size_t size) {
    pthread_mutex_lock(&g_lock);
    if (!g_tableReady) InitTable();
    pthread_mutex_unlock(&g_lock);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ g_table[(crc ^ p[i]) & 0xFF];
    return ~crc;
}

uint32_t NpDwcGameCode() { return g_gameCode; }
void NpDwcSetGameCode(uint32_t code) { g_gameCode = code; }

bool NpDwcUserDataValid(const void* userdata) {
    if (!userdata) return false;
    const DWCAccUserData* user = static_cast<const DWCAccUserData*>(userdata);
    return NpDwcCrc32(user, sizeof(DWCAccUserData) - 4) == user->crc32;
}

bool NpDwcUserDataHasProfile(const void* userdata) {
    if (!userdata) return false;
    const DWCAccUserData* user = static_cast<const DWCAccUserData*>(userdata);
    return DataType(user->authentic) == DWC_ACC_FRIENDDATA_LOGIN_ID && user->gs_profile_id > 0;
}

int32_t NpDwcUserDataProfile(const void* userdata) {
    return userdata ? static_cast<const DWCAccUserData*>(userdata)->gs_profile_id : 0;
}

void NpDwcUserDataSetProfile(void* userdata, int32_t profileId) {
    DWCAccUserData* user = static_cast<DWCAccUserData*>(userdata);
    if (!user) return;
    user->authentic = user->pseudo;   // the login id the service now knows (DWCi_Acc_SetLoginIdToUserData)
    SetDataType(&user->authentic, DWC_ACC_FRIENDDATA_LOGIN_ID);
    user->gs_profile_id = profileId;
    user->flag |= DWC_ACC_USERDATA_DIRTY;
    Recompute(user);
}

extern "C" void DWC_CreateUserData(DWCAccUserData* user) {
    if (!user) return;
    memset(user, 0, kUserDataBytes);
    user->size = kUserDataBytes;
    user->gs_profile_id = 0;
    user->gamecode = g_gameCode;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static uint64_t counter = 0;
    const uint64_t seed = Mix(uint64_t(time(nullptr)) ^ (uint64_t(ts.tv_nsec) << 20) ^ (uint64_t(ts.tv_sec) << 40) ^ ++counter ^ uint64_t(uintptr_t(user)));
    const uint64_t other = Mix(seed);
    user->pseudo.id_data = u32((seed >> 32) & DWC_ACC_USERID_HI32_MASK);
    user->pseudo.userid_lo32 = u32(seed);
    user->pseudo.playerid = u32(other);
    SetDataType(&user->pseudo, DWC_ACC_FRIENDDATA_LOGIN_ID);
    SetDataType(&user->authentic, DWC_ACC_FRIENDDATA_NODATA);   // an authenticated id exists only after a login
    user->flag |= DWC_ACC_USERDATA_DIRTY;
    Recompute(user);
}

extern "C" int DWC_CheckUserData(const DWCAccUserData* user) { return NpDwcUserDataValid(user) ? 1 : 0; }

extern "C" BOOL DWC_CheckHasProfile(const DWCAccUserData* user) { return NpDwcUserDataHasProfile(user) ? 1 : 0; }

extern "C" void DWC_ReportUserData(const DWCAccUserData*) {}   // the original prints the block to the debug log; it holds nothing worth printing

extern "C" BOOL DWC_CheckDirtyFlag(const DWCAccUserData* user) { return user && (user->flag & DWC_ACC_USERDATA_DIRTY) ? 1 : 0; }

extern "C" void DWC_ClearDirtyFlag(DWCAccUserData* user) {
    if (!user) return;
    user->flag &= ~DWC_ACC_USERDATA_DIRTY_MASK;
    Recompute(user);
}
