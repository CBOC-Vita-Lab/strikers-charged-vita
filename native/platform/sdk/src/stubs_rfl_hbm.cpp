// The two SDK modules the native port does not carry: RFL (Mii face library) and HBM (Home Button
// Menu). Both are defined here with the behaviour of a console that has neither Miis nor a Home Menu,
// so that the game's own checks take their "nothing available" branches:
//   RFL  no Mii data: the official Mii database is empty, every other query reports "not available".
//        (A real implementation needs the face library's resource archive and NAND database and the
//        model/icon renderer; the game uses Miis only in the online lobby and in the main menu's
//        "Mii" entry, which RFLGetAvailableOfficialDataNum() == 0 hides.)
//   HBM  the menu is never shown: the first HBMCalc() after HBMCreate() "selects" the Home button, which
//        in HBMManager.cpp is the "return to the game" result. NpHbm_RequestSelection() lets a platform
//        layer answer the other buttons (reset, back to the Wii menu: HBM_SELECT_BTN1/BTN2).
#include <RVLFaceLib/RFL_Database.h>
#include <RVLFaceLib/RFL_DataUtility.h>
#include <RVLFaceLib/RFL_Icon.h>
#include <RVLFaceLib/RFL_MiddleDatabase.h>
#include <RVLFaceLib/RFL_Model.h>
#include <RVLFaceLib/RFL_System.h>
#include <revolution/hbm/HBMCommon.h>

#include "np/np_sdk.h"

extern "C" {

// ---- RFL ---------------------------------------------------------------------------------------
static char g_rfl_arc_path[] = "RFL_Res.dat";   // the one entry of the disc's RFLRes01.arc (found by ARCOpen; RFLInitRes then declines, so Miis stay off)
char* RFLGetArcFilePath(void) { return g_rfl_arc_path; }
u32 RFLGetWorkSize(BOOL deluxeTex) { return deluxeTex ? 0x20000 : 0x10000; }   // never used: RFLInitRes fails
RFLErrcode RFLInitResAsync(void*, void*, u32, BOOL) { return RFLErrcode_Fatal; }
RFLErrcode RFLInitRes(void*, void*, u32, BOOL) { return RFLErrcode_Fatal; }
void RFLExit(void) {}
BOOL RFLAvailable(void) { return FALSE; }
RFLErrcode RFLGetAsyncStatus(void) { return RFLErrcode_Success; }
s32 RFLGetLastReason(void) { return 0; }
RFLErrcode RFLWaitAsync(void) { return RFLErrcode_Success; }

BOOL RFLIsAvailableOfficialData(u16) { return FALSE; }
u16 RFLGetAvailableOfficialDataNum(void) { return 0; }
BOOL RFLSearchOfficialData(const RFLCreateID*, u16*) { return FALSE; }
RFLErrcode RFLGetStoreData(RFLStoreData*, RFLDataSource, u16) { return RFLErrcode_NotAvailable; }
RFLErrcode RFLGetAdditionalInfo(RFLAdditionalInfo*, RFLDataSource, struct RFLMiddleDB*, u16) { return RFLErrcode_NotAvailable; }
RFLErrcode RFLMakeIcon(void*, RFLDataSource, RFLMiddleDB*, u16, RFLExpression, const RFLIconSetting*) {
    return RFLErrcode_NotAvailable;
}
void RFLSetIconDrawDoneCallback(RFLCallback) {}

u32 RFLGetMiddleDBBufferSize(u16 size) { return 0x100u + (u32)size * 0x40u; }
void RFLInitMiddleDB(RFLMiddleDB*, RFLMiddleDBType, void*, u16) {}
RFLErrcode RFLUpdateMiddleDBAsync(RFLMiddleDB*) { return RFLErrcode_NotAvailable; }
RFLErrcode RFLAddMiddleDBStoreData(RFLMiddleDB*, const RFLStoreData*) { return RFLErrcode_DBNodata; }

GXColor RFLGetFavoriteColor(RFLFavoriteColor color) {
    // the twelve colours of the face library, in its order
    static const GXColor table[12] = {{0xD2, 0x1E, 0x14, 0xFF}, {0xFF, 0x6E, 0x19, 0xFF}, {0xFF, 0xD8, 0x20, 0xFF},
                                      {0x78, 0xD2, 0x19, 0xFF}, {0x00, 0x78, 0x32, 0xFF}, {0x0A, 0x48, 0xB4, 0xFF},
                                      {0x3C, 0xAA, 0xDC, 0xFF}, {0xF5, 0x5A, 0x7D, 0xFF}, {0x73, 0x28, 0xAD, 0xFF},
                                      {0x48, 0x3A, 0x18, 0xFF}, {0xE0, 0xE0, 0xE0, 0xFF}, {0x18, 0x18, 0x18, 0xFF}};
    unsigned i = (unsigned)color;
    return table[i < 12 ? i : 0];
}

// ---- HBM ---------------------------------------------------------------------------------------
static int g_hbm_created = 0;
static int g_hbm_calc_count = 0;
static int g_hbm_forced = HBM_SELECT_NULL;   // set by NpHbm_RequestSelection
static int g_hbm_sel = HBM_SELECT_NULL;

void NpHbm_RequestSelection(int selection) { g_hbm_forced = selection; }

void HBMCreate(const HBMDataInfo*) {
    g_hbm_created = 1;
    g_hbm_calc_count = 0;
    g_hbm_sel = HBM_SELECT_NULL;
}
void HBMDelete(void) {
    g_hbm_created = 0;
    g_hbm_sel = HBM_SELECT_NULL;
    g_hbm_forced = HBM_SELECT_NULL;
}
void HBMInit(void) {}
HBMSelectBtnNum HBMCalc(const HBMControllerData*) {
    if (!g_hbm_created) return HBM_SELECT_NULL;
    ++g_hbm_calc_count;
    if (g_hbm_forced != HBM_SELECT_NULL) g_hbm_sel = g_hbm_forced;
    else if (g_hbm_calc_count >= 2) g_hbm_sel = HBM_SELECT_HOMEBTN;
    return (HBMSelectBtnNum)g_hbm_sel;
}
void HBMDraw(void) {}
HBMSelectBtnNum HBMGetSelectBtnNum(void) { return (HBMSelectBtnNum)g_hbm_sel; }
void HBMSetAdjustFlag(bool) {}
void HBMStartBlackOut(void) {}
bool HBMIsReassignedControllers(void) { return false; }
void HBMCreateSound(const char*, void*, u32) {}
void HBMDeleteSound(void) {}
void HBMUpdateSound(void) {}
void HBMUpdateSoundArchivePlayer(void) {}
void HBMPlaySound(int) {}
void HBMStopSound(void) {}

}  // extern "C"
