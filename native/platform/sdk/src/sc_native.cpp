// System configuration (SC): the values of the console's SYSCONF the game reads, taken from the
// SDK layer's configuration (np_sdk.h: language, aspect ratio, progressive scan, EuRGB60, sound mode)
// and defaults for the rest. No SYSCONF file is read or written; the real SDK code (ref/RVL_SDK/sc)
// parses a binary item table from NAND, which has no use here.
#include <revolution/os.h>
#include <revolution/sc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

namespace {
int g_motor = 1;
u8 g_speaker_vol = 89;
u32 g_dpd_sens = 3;
u8 g_sensor_bar = SC_SENSOR_BAR_TOP;
int env_country() {
    const char* v = getenv("NP_COUNTRY");
    return (v && *v) ? atoi(v) : 49;   // 49: United States in the Wii's country table
}
}  // namespace

extern "C" {

void SCInit(void) { np_os_lazy_init(); }
u32 SCCheckStatus(void) { return SC_STATUS_OK; }
void SCFlushAsync(SCFlushCallback cb) { if (cb) cb(SC_STATUS_OK); }

u8 SCGetAspectRatio(void) { return NpSdk_GetConfig()->widescreen ? SC_ASPECT_WIDE : SC_ASPECT_STD; }
s8 SCGetDisplayOffsetH(void) { return 0; }
u8 SCGetEuRgb60Mode(void) { return NpSdk_GetConfig()->eurgb60 ? SC_EURGB_60_HZ : SC_EURGB_50_HZ; }
void SCGetIdleMode(SCIdleModeInfo* mode) { mode->wc24 = 0; mode->slotLight = 0; }
u8 SCGetLanguage(void) { return (u8)NpSdk_GetConfig()->language; }
u8 SCGetProgressiveMode(void) { return NpSdk_GetConfig()->progressive ? SC_PROGRESSIVE : SC_INTERLACED; }
u8 SCGetScreenSaverMode(void) { return 0; }
u8 SCGetSoundMode(void) {
    const char* v = getenv("NP_SOUNDMODE");   // 0 mono, 1 stereo, 2 surround
    return (u8)((v && *v) ? atoi(v) : SC_SND_STEREO);
}
u32 SCGetCounterBias(void) { return 0; }
u32 SCGetSimpleAddressID(void) { return (u32)env_country() << SC_SIMPLE_ADDRESS_ID_COUNTRY; }
BOOL SCGetSimpleAddressData(SCSimpleAddress* a) {
    memset(a, 0, sizeof(*a));
    a->id = SCGetSimpleAddressID();
    return TRUE;
}

BOOL SCGetBtCmpDevInfoArray(SCBtCmpDevInfoArray* d) { memset(d, 0, sizeof(*d)); return TRUE; }
BOOL SCSetBtCmpDevInfoArray(SCBtCmpDevInfoArray* d) { (void)d; return TRUE; }
u32 SCGetBtDpdSensibility(void) { return g_dpd_sens; }
BOOL SCSetBtDpdSensibility(u32 s) { g_dpd_sens = s; return TRUE; }
void SCGetBtDeviceInfoArray(SCBtDeviceInfoArray* info) { memset(info, 0, sizeof(*info)); }
BOOL SCSetBtDeviceInfoArray(const SCBtDeviceInfoArray* info) { (void)info; return TRUE; }
u8 SCGetWpadMotorMode(void) { return (u8)g_motor; }
BOOL SCSetWpadMotorMode(u8 mode) { g_motor = mode; return TRUE; }
u8 SCGetWpadSensorBarPosition(void) { return g_sensor_bar; }
u8 SCGetWpadSpeakerVolume(void) { return g_speaker_vol; }
BOOL SCSetWpadSpeakerVolume(u8 vol) { g_speaker_vol = vol; return TRUE; }

BOOL SCFindByteArrayItem(void* dst, u32 len, SCItemID id) { (void)id; memset(dst, 0, len); return FALSE; }
BOOL SCReplaceByteArrayItem(const void* src, u32 len, SCItemID id) { (void)src; (void)len; (void)id; return TRUE; }
BOOL SCFindU8Item(u8* dst, SCItemID id) {
    switch (id) {
    case SC_ITEM_IPL_LNG: *dst = SCGetLanguage(); return TRUE;
    case SC_ITEM_IPL_AR: *dst = SCGetAspectRatio(); return TRUE;
    case SC_ITEM_IPL_E60: *dst = SCGetEuRgb60Mode(); return TRUE;
    case SC_ITEM_IPL_PGS: *dst = SCGetProgressiveMode(); return TRUE;
    case SC_ITEM_IPL_SND: *dst = SCGetSoundMode(); return TRUE;
    case SC_ITEM_BT_MOT: *dst = (u8)g_motor; return TRUE;
    case SC_ITEM_BT_BAR: *dst = g_sensor_bar; return TRUE;
    case SC_ITEM_BT_SPKV: *dst = g_speaker_vol; return TRUE;
    default: *dst = 0; return FALSE;
    }
}
BOOL SCFindS8Item(s8* dst, SCItemID id) { u8 v = 0; BOOL r = SCFindU8Item(&v, id); *dst = (s8)v; return r; }
BOOL SCFindU32Item(u32* dst, SCItemID id) {
    if (id == SC_ITEM_IPL_SADR) { *dst = SCGetSimpleAddressID(); return TRUE; }
    *dst = 0;
    return FALSE;
}
BOOL SCReplaceU8Item(u8 data, SCItemID id) { (void)data; (void)id; return TRUE; }
BOOL __SCF1(const char* type, char* buf, u32 sz) { (void)type; if (sz) buf[0] = 0; return FALSE; }
BOOL SCGetProductAreaString(char* buf, u32 sz) { snprintf(buf, sz, "USA"); return TRUE; }
s8 SCGetProductArea(void) { return SC_AREA_USA; }
char* SCGetProductCode(void) { static char code[] = "LU"; return code; }
BOOL SCGetProductSNString(char* buf, u32 sz) { snprintf(buf, sz, "000000000"); return TRUE; }
BOOL SCGetProductSN(u32* sn) { *sn = 0; return TRUE; }

}  // extern "C"
