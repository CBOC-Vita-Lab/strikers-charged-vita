#include "np_settings.h"
#include "np_vita_internal.h"
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
constexpr size_t kPixels = VitaConsole::kWidth * VitaConsole::kHeight;
std::map<int, std::vector<uint32_t>> blocks;
int nextBlock = 1;
uint32_t* current = nullptr;
uint32_t* pending = nullptr;
std::vector<uint32_t> displayed;
int pauses = 0, resumes = 0;
bool failAlloc = false, failBase = false, failSet = false, failWait = false, bootActive = false;

void CheckDisplayed() {
    if (current) assert(std::equal(displayed.begin(), displayed.end(), current));
}
}

SceUID sceKernelAllocMemBlock(const char*, int, SceSize size, const void*) {
    if (failAlloc) return -1;
    const int id = nextBlock++;
    blocks[id].resize(size / sizeof(uint32_t));
    return id;
}
int sceKernelGetMemBlockBase(SceUID id, void** base) {
    if (failBase) return -1;
    *base = blocks.at(id).data();
    return 0;
}
int sceKernelFreeMemBlock(SceUID id) {
    assert(blocks.at(id).data() != current && blocks.at(id).data() != pending);
    blocks.erase(id);
    return 0;
}
uint64_t sceKernelGetProcessTimeWide() { return 0; }
SceUID sceKernelGetThreadId() { return 1; }
int sceKernelDelayThread(unsigned int) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData*, int) { return 0; }
int sceDisplaySetFrameBuf(const SceDisplayFrameBuf* fb, int sync) {
    assert(fb && fb->base && sync == SCE_DISPLAY_SETBUF_NEXTFRAME);
    assert(fb->pitch == VitaConsole::kWidth && fb->height == VitaConsole::kHeight);
    CheckDisplayed();
    if (failSet) return -1;
    pending = static_cast<uint32_t*>(fb->base);
    return 0;
}
int sceDisplayWaitVblankStart() {
    CheckDisplayed();
    if (failWait) return -1;
    if (pending) { current = pending; pending = nullptr; }
    if (current) displayed.assign(current, current + kPixels);
    return 0;
}
int sceDisplayWaitSetFrameBuf() { return sceDisplayWaitVblankStart(); }

namespace VitaBootScreen {
void Pause() { ++pauses; }
void Resume() { ++resumes; }
void Stop() {}
bool Active() { return bootActive; }
}
namespace VitaLog {
int Tail(char (*)[kTailColumns + 1], int, uint32_t*) { return 0; }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const char* mode = argv[1];
    assert(VitaConsole::Init());
    VitaConsole::PresentScreen();
    sceDisplayWaitVblankStart();
    failAlloc = std::strcmp(mode, "allocation-failure") == 0;
    failBase = std::strcmp(mode, "base-failure") == 0;
    uint32_t* screen = VitaConsole::TakeScreen();
    if (failAlloc || failBase) {
        assert(!screen && pauses == 0 && blocks.size() == 1 && !pending);
        CheckDisplayed();
        std::puts(mode);
        return 0;
    }
    assert(screen && screen != current && screen != pending);
    NpSettings::State state;
    NpSettings::Load(state, nullptr);
    if (std::strcmp(mode, "queue-failure") == 0 || std::strcmp(mode, "wait-failure") == 0) {
        NpSettings::Draw(screen, state, 0, 1, "", false, true);
        failSet = std::strcmp(mode, "queue-failure") == 0;
        failWait = !failSet;
        const uint32_t* previousFront = VitaConsole::Pixels();
        assert(!VitaConsole::PresentScreen());
        CheckDisplayed();
        if (failSet) {
            assert(VitaConsole::Pixels() == previousFront && !pending);
            VitaConsole::ReleaseScreen(true);
        } else {
            assert(pending == screen && !VitaConsole::TakeScreen());
            VitaConsole::ReleaseScreen(true);
            assert(blocks.size() == 2 && pending == screen && !VitaConsole::TakeScreen());
            failWait = false;
            VitaConsole::ReleaseScreen(true);
            assert(current == screen && !pending);
        }
        CheckDisplayed();
        assert(blocks.size() == 1 && pauses == 1);
        std::puts(mode);
        return 0;
    }
    std::vector<uint32_t> backdrop(kPixels, 0xFF789ABCu);
    NpSettings::Art art;
    art.backdrop = backdrop.data();
    art.heading = [](uint32_t*, int, int, int, const char*, bool) {
        CheckDisplayed();
        return 0;
    };
    if (std::strcmp(mode, "art") == 0) NpSettings::SetArt(&art);
    bootActive = std::strcmp(mode, "boot-resume") == 0;
    for (int cycle = 0; cycle < 2; ++cycle) {
        const int rows = NpSettings::RowCount(0);
        for (int row = 0; row < rows * 2; ++row) {
            const int selected = row % rows;
            NpSettings::Draw(screen, state, 0, selected, "", false, true);
            CheckDisplayed();
            if (row % 3 == 0) NpSettings::DrawControls(screen, state);
            if (row % 3 == 1) NpSettings::DrawRestartConfirmation(screen, row % 2);
            if (row % 3 == 2) NpSettings::DrawLeaveConfirmation(screen, row % NpSettings::kLeaveChoices);
            CheckDisplayed();
            const std::vector<uint32_t> completed(screen, screen + kPixels);
            screen = VitaConsole::PresentScreen();
            assert(!pending && screen && screen != current && screen == VitaConsole::TakeScreen());
            assert(std::equal(completed.begin(), completed.end(), current));
            // Scripted screenshots recompose without presenting; the visible frame must not change.
            NpSettings::Draw(screen, state, 0, selected, "", false, true);
            CheckDisplayed();
            VitaConsole::SetLine(0, 0xFFFFFF, "ignored while settings owns the screen");
            VitaConsole::Refresh();
            CheckDisplayed();
        }
        VitaConsole::ReleaseScreen(!bootActive);
        CheckDisplayed();
        assert(blocks.size() == 1 && pauses == cycle + 1 && resumes == (bootActive ? cycle + 1 : 0));
        if (cycle == 1) break;
        screen = VitaConsole::TakeScreen();
        assert(!pending && screen && screen != current);
    }
    NpSettings::SetArt(nullptr);
    std::printf("%s: repeated row/controls/confirmation/shot redraws preserve live scanout\n", mode);
}
