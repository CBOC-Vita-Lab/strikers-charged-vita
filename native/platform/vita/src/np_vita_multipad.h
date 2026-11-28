// np_vita_multipad.h: pure PS Vita / PlayStation TV multi-controller port selection and button normalization.
//
// Header-only with zero VitaSDK dependencies so host ASan/UBSan unit tests (tests/test_multipad_cpp.py)
// exercise the exact shipped logic across handheld Vita, PSTV (1-4 DS3/DS4), MiniVitaTV, and ds34vita.
#pragma once

#include <cstdint>

namespace VitaMultiPad {

// SceCtrlExternalInputMode values (psp2common/ctrl.h).
inline constexpr uint8_t kTypeUnpaired = 0;
inline constexpr uint8_t kTypePhy      = 1;
inline constexpr uint8_t kTypeVirt     = 2;
inline constexpr uint8_t kTypeDs3      = 4;
inline constexpr uint8_t kTypeDs4      = 8;

// SceCtrlButtons bit positions (psp2common/ctrl.h).
inline constexpr uint32_t kBtnSelect   = 0x00000001u;
inline constexpr uint32_t kBtnL3       = 0x00000002u;
inline constexpr uint32_t kBtnR3       = 0x00000004u;
inline constexpr uint32_t kBtnStart    = 0x00000008u;
inline constexpr uint32_t kBtnUp       = 0x00000010u;
inline constexpr uint32_t kBtnRight    = 0x00000020u;
inline constexpr uint32_t kBtnDown     = 0x00000040u;
inline constexpr uint32_t kBtnLeft     = 0x00000080u;
inline constexpr uint32_t kBtnLTrigger = 0x00000100u;   // also SCE_CTRL_L2 in Positive2
inline constexpr uint32_t kBtnRTrigger = 0x00000200u;   // also SCE_CTRL_R2 in Positive2
inline constexpr uint32_t kBtnL1       = 0x00000400u;
inline constexpr uint32_t kBtnR1       = 0x00000800u;
inline constexpr uint32_t kBtnTriangle = 0x00001000u;
inline constexpr uint32_t kBtnCircle   = 0x00002000u;
inline constexpr uint32_t kBtnCross    = 0x00004000u;
inline constexpr uint32_t kBtnSquare   = 0x00008000u;

inline constexpr uint32_t kGameMask =
    kBtnSelect | kBtnStart | kBtnUp | kBtnRight | kBtnDown | kBtnLeft |
    kBtnLTrigger | kBtnRTrigger | kBtnTriangle | kBtnCircle | kBtnCross | kBtnSquare;

inline bool IsRealPortType(uint8_t t) {
    return t != kTypeUnpaired && t != kTypeVirt;
}

inline bool SupportsRumble(uint8_t t) {
    return t == kTypeDs3 || t == kTypeDs4;
}

struct Slot {
    int port = -1;                  // Primary Vita sceCtrl port (0..4), or -1 if no pad in this slot
    uint8_t type = kTypeUnpaired;   // SceCtrlExternalInputMode of `port`
    int extPort = -1;               // Optional mirrored BT port on slot 0 (handheld + ds34vita: read L2/R2 & drive rumble without creating a ghost P2)
    uint8_t extType = kTypeUnpaired;
};

// Maps Vita SceCtrlPortInfo::port[0..4] to 4 Wii player slots (0..3).
//
// Hardware/plugin cases:
//  - Standard handheld Vita (!isPsTv && port[0] == PHY, port[1..4] == UNPAIRED):
//      Slot 0 = port 0 (PHY); Slots 1..3 = none.
//  - Handheld Vita + ds34vita (1 BT pad mirroring port 0: port[0] == PHY, port[1] == DS3/DS4):
//      Slot 0 = port 0 (PHY) with extPort = 1 (DS3/DS4) so L2/R2 and rumble work with zero ghost Player 2.
//      Additional BT pads on ports 2..4 (if present and allowMulti) fill slots 1..3.
//  - PlayStation TV (isPsTv) or Handheld + MiniVitaTV (port[0] == VIRT/UNPAIRED):
//      port[0] is only a virtual alias of the first paired controller; real paired pads on port[1..4]
//      fill slots 0..3 in order (or slot 0 only when !allowMulti).
//      Falls back to slot 0 = port 0 if a plugin exposes no real port on 1..4 so Player 1 never loses input.
inline void SelectSlots(const uint8_t portTypes[5], bool isPsTv, bool allowMulti, Slot out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = Slot{};
    const bool handheldPrimary = !isPsTv && portTypes[0] == kTypePhy;
    if (handheldPrimary) {
        out[0].port = 0;
        out[0].type = kTypePhy;
        if (IsRealPortType(portTypes[1]) && portTypes[1] != kTypePhy) {
            out[0].extPort = 1;
            out[0].extType = portTypes[1];
        }
        if (allowMulti) {
            int nextSlot = 1;
            for (int p = 2; p <= 4 && nextSlot < 4; ++p) {
                if (IsRealPortType(portTypes[p])) {
                    out[nextSlot].port = p;
                    out[nextSlot].type = portTypes[p];
                    ++nextSlot;
                }
            }
        }
        return;
    }

    int nextSlot = 0;
    const int limit = allowMulti ? 4 : 1;
    for (int p = 1; p <= 4 && nextSlot < limit; ++p) {
        if (IsRealPortType(portTypes[p])) {
            out[nextSlot].port = p;
            out[nextSlot].type = portTypes[p];
            ++nextSlot;
        }
    }
    if (nextSlot == 0) {
        out[0].port = 0;
        out[0].type = portTypes[0] != kTypeUnpaired ? portTypes[0] : kTypeVirt;
    }
}

// Normalizes raw buttons from sceCtrlPeekBufferPositive2 (positive2 == true) or sceCtrlPeekBufferPositive
// (positive2 == false) into Strikers' standard Vita bitmask (L = 0x100, R = 0x200, Select = 0x001).
//
// In Positive2 mode:
//  - Vita L / DualShock L1 (SCE_CTRL_L1 0x400) -> kBtnLTrigger (0x100)
//  - Vita R / DualShock R1 (SCE_CTRL_R1 0x800) -> kBtnRTrigger (0x200)
//  - DualShock L2 (SCE_CTRL_L2 0x100)          -> kBtnSelect   (0x001: Cycle Item / Shakedown; replaces rear-left tap on PSTV)
//  - DualShock R2 (SCE_CTRL_R2 0x200)          -> kBtnRTrigger (0x200: Sprint / Turbo & Mega Save Punch)
// On a handheld Vita without an external pad, L2 and R2 are always 0, so output is bit-identical to Positive.
inline uint32_t NormalizeButtons(uint32_t raw, bool positive2) {
    if (!positive2) return raw & kGameMask;
    uint32_t out = raw & (kBtnSelect | kBtnStart | kBtnUp | kBtnRight | kBtnDown | kBtnLeft |
                          kBtnTriangle | kBtnCircle | kBtnCross | kBtnSquare);
    if (raw & kBtnL1) out |= kBtnLTrigger;
    if (raw & kBtnR1) out |= kBtnRTrigger;
    if (raw & kBtnLTrigger) out |= kBtnSelect;     // L2 -> Cycle Item / Shakedown
    if (raw & kBtnRTrigger) out |= kBtnRTrigger;   // R2 -> Sprint / Mega Save Punch
    return out;
}

// Extracts only the extra DualShock L2/R2 bindings from a mirrored auxiliary BT port (handheld + ds34vita).
inline uint32_t ExtraTriggersFromPositive2(uint32_t raw) {
    uint32_t out = 0;
    if (raw & kBtnLTrigger) out |= kBtnSelect;
    if (raw & kBtnRTrigger) out |= kBtnRTrigger;
    return out;
}

inline const char* TypeName(uint8_t t) {
    switch (t) {
    case kTypePhy: return "PHY";
    case kTypeVirt: return "VIRT";
    case kTypeDs3: return "DS3";
    case kTypeDs4: return "DS4";
    case kTypeUnpaired: return "NONE";
    default: return "EXT";
    }
}

} // namespace VitaMultiPad
