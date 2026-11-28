"""Host C++17 ASan/UBSan tests for PS Vita / PlayStation TV multi-controller slot selection and DualShock button normalization."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER_DIR = ROOT / "native/platform/vita/src"

HARNESS = r'''
#include "np_vita_multipad.h"
#include <cstdio>
#include <cstdlib>

using namespace VitaMultiPad;

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
        ++g_fail; \
    } \
} while (0)

static void test_handheld_only() {
    uint8_t ports[5] = {kTypePhy, kTypeUnpaired, kTypeUnpaired, kTypeUnpaired, kTypeUnpaired};
    Slot s[4];
    SelectSlots(ports, false, true, s);
    CHECK(s[0].port == 0 && s[0].type == kTypePhy && s[0].extPort == -1);
    CHECK(s[1].port == -1 && s[2].port == -1 && s[3].port == -1);
}

static void test_handheld_ds34vita_no_ghost_p2() {
    uint8_t ports[5] = {kTypePhy, kTypeDs4, kTypeUnpaired, kTypeUnpaired, kTypeUnpaired};
    Slot s[4];
    SelectSlots(ports, false, true, s);
    CHECK(s[0].port == 0 && s[0].type == kTypePhy && s[0].extPort == 1 && s[0].extType == kTypeDs4);
    CHECK(s[1].port == -1 && s[2].port == -1 && s[3].port == -1);
}

static void test_handheld_ds34vita_plus_extra_bt_pads() {
    uint8_t ports[5] = {kTypePhy, kTypeDs4, kTypeDs3, kTypeDs4, kTypeUnpaired};
    Slot s[4];
    SelectSlots(ports, false, true, s);
    CHECK(s[0].port == 0 && s[0].extPort == 1);
    CHECK(s[1].port == 2 && s[1].type == kTypeDs3);
    CHECK(s[2].port == 3 && s[2].type == kTypeDs4);
    CHECK(s[3].port == -1);

    // Online/LAN guard (allowMulti == false) keeps only slot 0 (+ mirrored extPort 1).
    SelectSlots(ports, false, false, s);
    CHECK(s[0].port == 0 && s[0].extPort == 1);
    CHECK(s[1].port == -1 && s[2].port == -1 && s[3].port == -1);
}

static void test_handheld_minivitatv() {
    uint8_t ports[5] = {kTypeVirt, kTypeDs4, kTypeDs3, kTypeUnpaired, kTypeUnpaired};
    Slot s[4];
    SelectSlots(ports, false, true, s);
    CHECK(s[0].port == 1 && s[0].type == kTypeDs4 && s[0].extPort == -1);
    CHECK(s[1].port == 2 && s[1].type == kTypeDs3 && s[1].extPort == -1);
    CHECK(s[2].port == -1 && s[3].port == -1);
}

static void test_pstv_four_controllers_and_sparse() {
    uint8_t full[5] = {kTypeVirt, kTypeDs4, kTypeDs3, kTypeDs4, kTypeDs3};
    Slot s[4];
    SelectSlots(full, true, true, s);
    CHECK(s[0].port == 1 && s[0].type == kTypeDs4);
    CHECK(s[1].port == 2 && s[1].type == kTypeDs3);
    CHECK(s[2].port == 3 && s[2].type == kTypeDs4);
    CHECK(s[3].port == 4 && s[3].type == kTypeDs3);

    // Online/LAN guard on PSTV exposes only P1.
    SelectSlots(full, true, false, s);
    CHECK(s[0].port == 1 && s[0].type == kTypeDs4);
    CHECK(s[1].port == -1 && s[2].port == -1 && s[3].port == -1);

    // Sparse pairing (e.g. P1 turned off, P2 & P4 active) compacts into slots 0..1.
    uint8_t sparse[5] = {kTypeVirt, kTypeUnpaired, kTypeDs4, kTypeUnpaired, kTypeDs3};
    SelectSlots(sparse, true, true, s);
    CHECK(s[0].port == 2 && s[0].type == kTypeDs4);
    CHECK(s[1].port == 4 && s[1].type == kTypeDs3);
    CHECK(s[2].port == -1 && s[3].port == -1);

    // Fallback when no real port is reported: slot 0 falls back to port 0 so P1 never loses control.
    uint8_t empty[5] = {kTypeUnpaired, kTypeUnpaired, kTypeUnpaired, kTypeUnpaired, kTypeUnpaired};
    SelectSlots(empty, true, true, s);
    CHECK(s[0].port == 0 && s[0].type == kTypeVirt);
    CHECK(s[1].port == -1);
}

static void test_button_normalization() {
    // Legacy Positive mode: mask only game bits, L/R stay in place.
    CHECK(NormalizeButtons(kBtnCross | kBtnLTrigger | kBtnRTrigger | kBtnL3 | kBtnR3, false) ==
          (kBtnCross | kBtnLTrigger | kBtnRTrigger));

    // Positive2 mode: L1/R1 -> L/R triggers, L2 -> Select, R2 -> R trigger, L3/R3 ignored.
    CHECK(NormalizeButtons(kBtnL1 | kBtnCross, true) == (kBtnLTrigger | kBtnCross));
    CHECK(NormalizeButtons(kBtnR1 | kBtnSquare, true) == (kBtnRTrigger | kBtnSquare));
    CHECK(NormalizeButtons(kBtnLTrigger, true) == kBtnSelect);
    CHECK(NormalizeButtons(kBtnRTrigger, true) == kBtnRTrigger);
    CHECK(NormalizeButtons(kBtnL3 | kBtnR3 | 0x00010000u, true) == 0u);
    CHECK(NormalizeButtons(kBtnL1 | kBtnLTrigger | kBtnR1 | kBtnRTrigger | kBtnStart | kBtnUp, true) ==
          (kBtnLTrigger | kBtnSelect | kBtnRTrigger | kBtnStart | kBtnUp));

    // Auxiliary mirrored DualShock port extracts only L2/R2.
    CHECK(ExtraTriggersFromPositive2(kBtnCross | kBtnL1 | kBtnR1 | kBtnL3) == 0u);
    CHECK(ExtraTriggersFromPositive2(kBtnLTrigger) == kBtnSelect);
    CHECK(ExtraTriggersFromPositive2(kBtnRTrigger) == kBtnRTrigger);
    CHECK(ExtraTriggersFromPositive2(kBtnLTrigger | kBtnRTrigger) == (kBtnSelect | kBtnRTrigger));
}

static void test_rumble_and_names() {
    CHECK(!SupportsRumble(kTypeUnpaired));
    CHECK(!SupportsRumble(kTypePhy));
    CHECK(!SupportsRumble(kTypeVirt));
    CHECK(SupportsRumble(kTypeDs3));
    CHECK(SupportsRumble(kTypeDs4));
    CHECK(IsRealPortType(kTypePhy) && IsRealPortType(kTypeDs3) && IsRealPortType(kTypeDs4));
    CHECK(!IsRealPortType(kTypeUnpaired) && !IsRealPortType(kTypeVirt));
}

int main() {
    test_handheld_only();
    test_handheld_ds34vita_no_ghost_p2();
    test_handheld_ds34vita_plus_extra_bt_pads();
    test_handheld_minivitatv();
    test_pstv_four_controllers_and_sparse();
    test_button_normalization();
    test_rumble_and_names();
    if (g_fail) return 1;
    std::printf("multipad_unit: ok\n");
    return 0;
}
'''


class MultiPadTests(unittest.TestCase):
    def test_multipad_cpp_logic(self):
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        self.assertIsNotNone(cxx, "C++ compiler required")
        with tempfile.TemporaryDirectory() as d:
            src = pathlib.Path(d, "test_multipad.cpp")
            exe = pathlib.Path(d, "test_multipad")
            src.write_text(HARNESS, encoding="utf-8")
            cmd = [cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(HEADER_DIR), str(src), "-o", str(exe)]
            # Prefer ASan/UBSan when available on the host compiler.
            san = subprocess.run(cmd + ["-fsanitize=address,undefined"], capture_output=True, text=True)
            if san.returncode != 0:
                res = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("multipad_unit: ok", run.stdout)

    def test_platform_and_sdk_wiring(self):
        wire = (ROOT / "native/platform/vita/src/np_vita_wire.cpp").read_text(encoding="utf-8")
        inp = (ROOT / "native/platform/vita/src/np_vita_input.cpp").read_text(encoding="utf-8")
        touch = (ROOT / "native/platform/sdk/input/np_touch.cpp").read_text(encoding="utf-8")
        sdk_in = (ROOT / "native/platform/sdk/src/input_native.cpp").read_text(encoding="utf-8")
        self.assertIn("NpSdk_SetRumbleSink(RumbleSink)", wire)
        self.assertIn("NpVitaStopAllRumble()", wire)
        self.assertIn("NpVitaReadPad(pad, &in)", wire)
        self.assertIn("MultiPadAllowedNow()", wire)
        self.assertIn("sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE)", inp)
        self.assertIn("sceCtrlGetControllerPortInfo", inp)
        self.assertIn("sceCtrlSetActuator", inp)
        self.assertIn("chan < 0 || chan >= 4", touch)
        self.assertIn("sync_hotplug", sdk_in)


if __name__ == "__main__":
    unittest.main()
