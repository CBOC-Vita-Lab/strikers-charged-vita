// np_menu_nav.cpp: console-style navigation of the game's pointer-only menus.
//
// Port of MenuNav in the recompiled runtime (one/port/runtime/src/hle/input/pad.cpp, a "mod" there, always on here),
// written against the decompiled classes instead of guest memory. The menus of Mario Strikers Charged are driven by a
// pointer (Wii Remote IR). The D-pad and the sticks move a highlight between the buttons: the emulated pointer snaps
// onto the next button in that direction and the game's own hover logic (A to press) runs unchanged. When nothing is
// highlighted (a menu just opened) the first button in reading order is selected; B goes to the BACK button and taps A
// on it; L/R tap -/+ (pages); on ring menus (the main menu) a stick direction picks the button at that angle.
//
// What the recompiled runtime read from guest memory is read here from: g_pFEPointerManager->mListeners (every
// FEPointerRegion, the only ContainsPoint override in the game), gFEPointerPositions (the pointer, centre origin,
// +y up), the scene stack, g_pFEInput's lock depth and gFEPointerInstances (the hand).
#define private public   // FEPointerRegion keeps its rotation and pivot private; this TU only reads them
#include "Game/FE/fePointer.h"
#undef private

#include "Game/FE/feBackButton.h"
#include "Game/FE/feDPD.h"
#include "Game/FE/feInput.h"
#include "Game/FE/fePointerButton.h"
#include "Game/FE/fePointerManager.h"
#include "Game/FE/tlComponentInstance.h"
#include "Game/FE/tlInstance.h"
#include "Game/GameSceneManager.h"
#include "Game/Render/RLViewLayers.h"
#include "Game/SH/SHChooseCaptains.h"
#include "Game/SH/SHChooseSidekicks.h"
#include "NL/gl/glStruct.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdk/input/np_game_input.h"

namespace {

void* g_backButtonVptr = 0;
constexpr int kChannels = 4;
constexpr int kMaxButtons = 160;

// StadiumSelectScene: SceneList 5. ChooseCaptains: 2 / 6, ChooseSidekicks: 3 / 7 (the character grid keeps its buttons
// live while it is hidden).
constexpr int kSceneStadiumSelect = 5;

struct Button {
    float minX, maxX, minY, maxY, rot, pivotX, pivotY;
    NpPoint centre;
    NpPoint aim;   // where the pointer is placed to select it
    bool back;
    const void* listener;
    // Same test as FEPointerRegion::ContainsPoint.
    bool Contains(NpPoint p) const {
        if (rot != 0.0f) {
            const float lx = p.x - pivotX, ly = p.y - pivotY, c = cosf(rot), sn = sinf(rot);
            p = {lx * c + ly * sn + pivotX, -lx * sn + ly * c + pivotY};
        }
        return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
    }
};
struct ButtonList {
    Button b[kMaxButtons];
    int n = 0;
    const Button* begin() const { return b; }
    const Button* end() const { return b + n; }
    bool empty() const { return n == 0; }
};

struct State {
    uint32_t heldDir = 0;
    uint64_t repeatAtMs = 0;
    NpPoint goal{};
    NpPoint aim{};
    int settleFrames = 0;
    bool backHeld = false;
    int backFrames = 0;       // B pressed: heading for the back button
    int pressFrames = 0;      // tapping A on it
    int hideHandFrames = 0;   // hand hidden while B goes back
    int pageFrames = 0;       // tapping +/- (stage select)
    int pageDir = 0;
    int pauseFrames = 0;      // tapping the pause button (B on an in-match overlay)
    const void* scene = 0;    // top scene last frame, to spot screen changes
    uint32_t memoryKey = 0;   // focus memory key for this screen
    int heldPage = 0;         // L/R page input last frame
    bool hadButtons = false;  // buttons last frame (false: a menu/overlay just appeared)
    bool overlay = false;     // this menu appeared over an unchanged scene (e.g. the pause menu)
    const void* sceneWhenEmpty = 0;
    const void* selectedListener = 0;  // the button last selected (buttons move: a screen slides out)
    int offButtonFrames = 0;           // frames the pointer has rested on no button
};
State s_state[kChannels];
// Player 1 always navigates; other pads join in once they are used, so an idle second controller does not snap its
// pointer around in menus player 1 is driving.
bool s_engaged[kChannels] = {true, false, false, false};

// Where each pad last was on each screen (keyed by scene id, in-match overlays apart), so coming back to a screen or
// closing a pop-up returns to that button.
struct Memory {
    uint32_t key;
    NpPoint at;
    bool used;
};
constexpr int kMemorySlots = 32;
Memory s_memory[kChannels][kMemorySlots];
int s_memoryNext[kChannels];
const Memory* memory_find(int chan, uint32_t key) {
    for (int i = 0; i < kMemorySlots; ++i)
        if (s_memory[chan][i].used && s_memory[chan][i].key == key) return &s_memory[chan][i];
    return 0;
}
void memory_store(int chan, uint32_t key, NpPoint at) {
    for (int i = 0; i < kMemorySlots; ++i) {
        if (s_memory[chan][i].used && s_memory[chan][i].key == key) {
            s_memory[chan][i].at = at;
            return;
        }
    }
    Memory& m = s_memory[chan][s_memoryNext[chan]];
    s_memoryNext[chan] = (s_memoryNext[chan] + 1) % kMemorySlots;
    m = {key, at, true};
}

// The selected button per pad (focus memory and the "selected" tracking)
Button s_selected[kChannels];
bool s_hasSelected[kChannels];

// Seconds per game frame, smoothed: the navigation counts frames (a snap settles in 4, a tap lasts 3 ...).
float s_frameDt = 1.0f / 30.0f;
int frames_for(double seconds) {
    const double dt = s_frameDt > 1.0f / 120.0f ? s_frameDt : 1.0f / 120.0f;
    int f = (int)(seconds / dt + 0.5);
    return f < 1 ? 1 : f;
}

uint64_t now_ms() { return NpGameNowNs() / 1000000ull; }

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// The menu canvas the pointer moves on: GetPointerPosition (feDPD) maps KPAD pos to screen as (x * w/2, -y * h/2),
// w = 854 in widescreen else the screen width, h = the screen height.
NpPoint half_extents() {
    gl_ScreenInfo* info = glGetScreenInfo();
    if (info) {
        const float w = IsWidescreen() ? 854.0f : (float)info->ScreenWidth;
        const float h = (float)info->ScreenHeight;
        if (w > 100.0f && h > 100.0f) return {w * 0.5f, h * 0.5f};
    }
    return {320.0f, 240.0f};
}

NpPoint pointer_position(int chan) { return {gFEPointerPositions[chan].x, gFEPointerPositions[chan].y}; }

BaseSceneHandler* current_scene() {
    GameSceneManager* m = GameSceneManager::Instance();
    if (!m) return 0;
    const unsigned long depth = m->mCurrentStackDepth;
    if (depth == 0 || depth > BaseGameSceneManager::MAX_SCENE_DEPTH) return 0;
    return m->mBaseSceneHandlerStack[depth - 1];
}
int current_scene_id() {
    GameSceneManager* m = GameSceneManager::Instance();
    if (!m) return -1;
    const unsigned long depth = m->mCurrentStackDepth;
    if (depth == 0 || depth > BaseGameSceneManager::MAX_SCENE_DEPTH) return -1;
    return (int)m->m_sceneStack[depth - 1];
}

// Every live pointer region (button), in screen space. Scenes feed pointer events only to their own buttons, so buttons
// of screens that are registered but not shown never see an event: a button counts as live only if its last event for
// this pad is at the pointer's current position.
void collect_buttons(int chan, ButtonList* out) {
    out->n = 0;
    if (!g_pFEPointerManager) return;
    const NpPoint at = pointer_position(chan);
    const bool inputLocked = g_pFEInput && g_pFEInput->m_InputLockDepth != 0;
    // a hidden character grid keeps its buttons registered: skip them
    const FEPointerListener* hiddenFrom = 0;
    const FEPointerListener* hiddenTo = 0;
    if (BaseSceneHandler* scene = current_scene()) {
        const int id = current_scene_id();
        if (id == 2 || id == 6) {
            ChooseCaptainsSceneV2* s = static_cast<ChooseCaptainsSceneV2*>(scene);
            if (!s->mCaptainsShown) {
                hiddenFrom = &s->mCaptainButtons[0];
                hiddenTo = &s->mCaptainButtons[12];
            }
        } else if (id == 3 || id == 7) {
            ChooseSidekicksSceneV2* s = static_cast<ChooseSidekicksSceneV2*>(scene);
            if (!s->mSidekicksShown) {
                hiddenFrom = &s->mSidekickButtons[0];
                hiddenTo = &s->mSidekickButtons[8];
            }
        }
    }
    int guard = 0;
    for (ListEntry<FEPointerListener*>* entry = g_pFEPointerManager->mListeners.m_Head; entry && guard < 512;
         entry = entry->next, ++guard) {
        FEPointerListener* listener = entry->entry;
        if (!listener) continue;
        if (hiddenFrom && (const char*)listener >= (const char*)hiddenFrom && (const char*)listener < (const char*)hiddenTo) continue;
        FEPointerRegion* region = static_cast<FEPointerRegion*>(listener);   // the only ContainsPoint override
        if (region->mDisabled) continue;
        if (inputLocked && !region->mIgnoreInputLock) continue;
        const FEPointerEvent& last = region->mPreviousEvents[chan];
        if (last.mIndex != chan || fabsf(last.mPosition.x - at.x) > 1.0f || fabsf(last.mPosition.y - at.y) > 1.0f) continue;
        Button b;
        memset(&b, 0, sizeof b);
        b.minX = region->mMinX;
        b.maxX = region->mMaxX;
        b.maxY = region->mMaxY;
        b.minY = region->mMinY;
        if (!(b.maxX > b.minX && b.maxY > b.minY) || b.maxX - b.minX > 600.0f || b.maxY - b.minY > 420.0f) continue;
        b.rot = region->mRotation;
        b.pivotX = region->mPivot.x;
        b.pivotY = region->mPivot.y;
        b.centre = {(b.minX + b.maxX) * 0.5f, (b.minY + b.maxY) * 0.5f};
        if (b.rot != 0.0f) {
            const float dx = b.centre.x - b.pivotX, dy = b.centre.y - b.pivotY, c = cosf(b.rot), sn = sinf(b.rot);
            b.centre = {b.pivotX + dx * c - dy * sn, b.pivotY + dx * sn + dy * c};
        }
        // Where the pointer goes: low in the button, so the hand (drawn below its fingertip) leaves the label visible;
        // small or rotated buttons (ring icons) take the centre.
        b.aim = b.centre;
        if (b.rot == 0.0f && b.maxY - b.minY > 24.0f) b.aim.y = b.minY + (b.maxY - b.minY) * 0.3f;
        b.back = g_backButtonVptr && *(void* const*)listener == g_backButtonVptr;
        b.listener = listener;
        if (isfinite(b.centre.x) && isfinite(b.centre.y) && out->n < kMaxButtons) out->b[out->n++] = b;
    }
}

float maxf(float a, float b) { return a > b ? a : b; }

// The next button in the pressed direction. Buttons lined up with the current one (overlapping it across the direction
// of travel: same row for left/right, same column for up/down) come first, nearest first; otherwise the nearest,
// weighting sideways distance. Off the end of a row or column, it wraps to the far end of the same row/column. BACK is
// never a target (B does that).
bool pick_in_direction(const ButtonList& buttons, const Button* current, NpPoint from, uint32_t dir, NpPoint* out) {
    using namespace NpMenuNav;
    const bool horizontal = (dir & (kLeft | kRight)) != 0;
    const float dx = (dir & kRight) ? 1.0f : (dir & kLeft) ? -1.0f : 0.0f;
    const float dy = (dir & kUp) ? 1.0f : (dir & kDown) ? -1.0f : 0.0f;
    const float lo = current ? (horizontal ? current->minY : current->minX) : (horizontal ? from.y : from.x) - 4.0f;
    const float hi = current ? (horizontal ? current->maxY : current->maxX) : (horizontal ? from.y : from.x) + 4.0f;
    float best = 1e30f, wrapBest = -1.0f;
    NpPoint wrap{};
    // Distances run from the CENTRE of the current button, not from where the pointer rests on it (the game's aim point of a
    // button is below its centre): otherwise a neighbour in the same row counts as "up" and UP on the top row moves sideways.
    if (current) from = current->centre;
    for (const Button& b : buttons) {
        if (&b == current || b.back) continue;
        const float vx = b.centre.x - from.x, vy = b.centre.y - from.y;
        const float forward = vx * dx + vy * dy;
        const float bLo = horizontal ? b.minY : b.minX, bHi = horizontal ? b.maxY : b.maxX;
        const float gap = maxf(0.0f, maxf(bLo - hi, lo - bHi));
        if (forward >= 8.0f) {
            const float score = gap == 0.0f ? forward : 100000.0f + forward + 3.0f * gap;
            if (score < best) {
                best = score;
                *out = b.centre;
            }
        } else if (forward <= -8.0f && gap == 0.0f && -forward > wrapBest) {
            wrapBest = -forward;   // farthest lined-up button behind us
            wrap = b.centre;
        }
    }
    if (best < 1e30f) return true;
    if (wrapBest > 0.0f) {
        *out = wrap;
        return true;
    }
    return false;
}

// Default selection when a menu opens: the first button in reading order (top row first, then left to right), skipping
// BACK and controls at the screen edges (page dots, side arrows). With preferWidest (stage select) the widest button wins
// instead, i.e. PLAY NOW over the arrows.
NpPoint pick_default(const ButtonList& buttons, bool preferWidest) {
    const Button* pool[kMaxButtons];
    int n = 0;
    const NpPoint half = half_extents();
    for (const Button& b : buttons) {
        if (b.back || fabsf(b.centre.x) > half.x - 50.0f || b.centre.y > half.y - 50.0f) continue;
        pool[n++] = &b;
    }
    if (n == 0)
        for (const Button& b : buttons)
            if (!b.back) pool[n++] = &b;
    if (n == 0) return buttons.b[0].centre;
    const Button* best = pool[0];
    for (int i = 0; i < n; ++i) {
        const Button* b = pool[i];
        if (preferWidest) {
            if (b->maxX - b->minX > best->maxX - best->minX) best = b;
        } else if (b->centre.y > best->centre.y + 20.0f ||
                   (fabsf(b->centre.y - best->centre.y) <= 20.0f && b->centre.x < best->centre.x)) {
            best = b;   // higher row, or same row further left
        }
    }
    return best->centre;
}

// Hiding the hand: scenes re-show it every frame, but nothing touches its colour (SetPointerColour is a stub), so the
// hand and its child graphics get a colour override with zero alpha, remembering the originals to put back.
struct SavedColour {
    TLInstance* inst;
    u32 flags;
    nlColour colour;
};
constexpr int kMaxSaved = 64;
struct HandState {
    TLInstance* hand = 0;
    SavedColour saved[kMaxSaved];
    int n = 0;
};
HandState s_hand[kChannels];

void collect_tree(TLInstance* inst, TLInstance** out, int* n) {
    for (int guard = 0; inst && guard < 64 && *n < kMaxSaved; ++guard) {
        bool seen = false;
        for (int i = 0; i < *n; ++i)
            if (out[i] == inst) seen = true;
        if (seen) return;
        out[(*n)++] = inst;
        collect_tree(inst->pChildren, out, n);
        inst = inst->m_next;
    }
}

void set_hand_visible(int chan, bool visible) {
    HandState& hs = s_hand[chan];
    TLInstance* hand = (TLInstance*)gFEPointerInstances[chan];
    if (hand != hs.hand) {   // new scene / hand: the old instances may be gone
        hs.n = 0;
        hs.hand = hand;
    }
    if (!hand) return;
    if (visible) {
        for (int i = 0; i < hs.n; ++i) {
            hs.saved[i].inst->m_overloadFlags = hs.saved[i].flags;
            hs.saved[i].inst->m_overloadedAttributes.colour = hs.saved[i].colour;
        }
        hs.n = 0;
        return;
    }
    TLInstance* tree[kMaxSaved];
    int nt = 0;
    tree[nt++] = hand;
    collect_tree(hand->pChildren, tree, &nt);
    for (int i = 0; i < nt; ++i) {
        TLInstance* inst = tree[i];
        bool known = false;
        for (int j = 0; j < hs.n; ++j)
            if (hs.saved[j].inst == inst) known = true;
        if (!known && hs.n < kMaxSaved) {
            hs.saved[hs.n].inst = inst;
            hs.saved[hs.n].flags = inst->m_overloadFlags;
            hs.saved[hs.n].colour = inst->m_overloadedAttributes.colour;
            ++hs.n;
        }
        inst->m_overloadFlags |= 0x10;
        inst->m_overloadedAttributes.colour.c[3] = 0;   // alpha
    }
}

void select_at(int chan, const ButtonList& buttons, NpPoint centre) {
    for (const Button& b : buttons) {
        if (b.centre.x == centre.x && b.centre.y == centre.y) {
            s_selected[chan] = b;
            s_hasSelected[chan] = true;
            return;
        }
    }
}

// Select the button whose centre is `goal`: the pointer goes exactly to its aim point (screen space), which the game
// reads on its next update.
void snap_to(State& st, const ButtonList& buttons, NpPoint goal, float cursor[2]) {
    const NpPoint half = half_extents();
    st.goal = goal;
    st.aim = goal;
    for (const Button& b : buttons) {
        if (b.centre.x == goal.x && b.centre.y == goal.y) {
            st.aim = b.aim;
            break;
        }
    }
    st.settleFrames = 4;   // until the game has caught up with the new position
    cursor[0] = clampf(st.aim.x / half.x, -1.0f, 1.0f);
    cursor[1] = clampf(-st.aim.y / half.y, -1.0f, 1.0f);
}

}  // namespace

extern "C" void NpNoteBackButtonVptr(void* backButton) {
    if (backButton) g_backButtonVptr = *(void**)backButton;
}

namespace NpMenuNav {

bool Enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("NP_MENU_NAV");
        on = !(e && *e == '0');
    }
    return on == 1;
}

void ForgetChannel(int chan) {
    if (chan < 0 || chan >= kChannels) return;
    set_hand_visible(chan, true);
    s_state[chan] = State();
    s_hasSelected[chan] = false;
}

Result Update(int chan, uint32_t dir, NpPoint stick, bool backButton, int pageInput, float cursor[2], float dt) {
    Result r;
    if (chan < 0 || chan >= kChannels) return r;
    State& st = s_state[chan];
    if (dt > 0.0f) s_frameDt += (dt - s_frameDt) * 0.1f;
    if (!Enabled()) {
        // Off (or just switched off): give the game its pointer and hand back.
        if (st.scene || st.hadButtons) {
            set_hand_visible(chan, true);
            st = State();
        }
        s_hasSelected[chan] = false;
        return r;
    }
    // A menu is up whenever live pointer buttons exist (none during matches).
    static ButtonList buttons;
    collect_buttons(chan, &buttons);
    s_hasSelected[chan] = false;
    if (dir || backButton || hypotf(stick.x, stick.y) > 0.5f) s_engaged[chan] = true;
    if (!s_engaged[chan]) return r;   // idle extra pad: leave its pointer alone
    BaseSceneHandler* sceneNow = current_scene();
    if (buttons.empty()) {
        st = State();
        st.sceneWhenEmpty = sceneNow;
        return r;
    }
    r.menu = true;
    // One direction at a time; the first press moves at once, holding repeats.
    if (dir & (kUp | kDown)) dir &= (kUp | kDown);
    else dir &= (kLeft | kRight);
    if (((dir & kUp) && (dir & kDown)) || ((dir & kLeft) && (dir & kRight))) dir = 0;
    const uint64_t now = now_ms();
    bool step = false;
    if (dir && dir != st.heldDir) {
        step = true;
        st.repeatAtMs = now + 350;
    } else if (dir && now >= st.repeatAtMs) {
        step = true;
        st.repeatAtMs = now + 130;
    }
    st.heldDir = dir;
    const bool backPressed = backButton && !st.backHeld;
    st.backHeld = backButton;

    if (st.hideHandFrames > 0) --st.hideHandFrames;
    set_hand_visible(chan, st.hideHandFrames == 0);   // the game's own hand marks the selection
    const NpPoint at = pointer_position(chan);
    // The marker follows the button being moved to (st.goal) while a snap settles, else whatever the pointer rests on.
    if (st.settleFrames > 0) {
        select_at(chan, buttons, st.goal);
    } else {
        for (const Button& b : buttons) {
            if (b.Contains(at)) {
                s_selected[chan] = b;
                s_hasSelected[chan] = true;
                break;
            }
        }
    }
    // A new screen (another scene on top, or buttons appearing after none) starts on its default selection (first in
    // reading order), wherever the pointer was left.
    const int sceneId = current_scene_id();
    const bool stageSelectScreen = sceneId == kSceneStadiumSelect;
    if (sceneNow != st.scene || !st.hadButtons) {
        // In-match overlays (the pause menu) appear without a scene change.
        st.overlay = !st.hadButtons && sceneNow == st.sceneWhenEmpty;
        st.scene = sceneNow;
        st.hadButtons = true;
        st.memoryKey = (uint32_t)(sceneId + 2) ^ (st.overlay ? 0x10000u : 0u);
        NpPoint start = pick_default(buttons, stageSelectScreen);
        const Memory* remembered = memory_find(chan, st.memoryKey);
        if (remembered) {
            for (const Button& b : buttons) {
                if (b.centre.x == remembered->at.x && b.centre.y == remembered->at.y) {
                    start = remembered->at;
                    break;
                }
            }
        }
        snap_to(st, buttons, start, cursor);
        select_at(chan, buttons, st.goal);
        return r;
    }
    if (s_hasSelected[chan]) memory_store(chan, st.memoryKey, s_selected[chan].centre);
    // Stage select defaults to PLAY NOW (pick_default) and pages with L/R like any other screen.
    const bool stageSelect = sceneId == kSceneStadiumSelect;
    if (st.pageFrames > 0) {
        --st.pageFrames;
        r.page = st.pageFrames >= 2 ? st.pageDir : 0;
    }
    // L/R page or switch tabs on any screen (the game's -/+), as the shoulder buttons do on SMS.
    if (pageInput != 0 && pageInput != st.heldPage) {
        st.pageDir = pageInput;
        st.pageFrames = 4;
        r.page = pageInput;
    }
    st.heldPage = pageInput;
    // B: go to the back button, then tap A on it. Overlays without one (the pause menu) close with the game's own pause
    // button instead.
    if (backPressed) {
        const Button* back = 0;
        for (const Button& b : buttons)
            if (b.back) {
                back = &b;
                break;
            }
        if (back) {
            // Seamless: the hand hides, the pointer goes to BACK, and A taps there next frame (the screen changes
            // before anything is seen moving).
            snap_to(st, buttons, back->centre, cursor);
            st.backFrames = 2;
            st.hideHandFrames = frames_for(1.0 / 3.0);
        } else if (st.overlay) {
            st.pauseFrames = 4;
        }
    }
    if (st.pauseFrames > 0) {
        --st.pauseFrames;
        r.pressPause = st.pauseFrames >= 2;
    }
    if (st.pressFrames > 0) {
        --st.pressFrames;
        r.pressA = st.pressFrames >= 2;
        return r;
    }
    if (st.backFrames > 0 && --st.backFrames == 0) {
        st.pressFrames = 3;
        r.pressA = true;
        return r;
    }
    // Ring menus (the main menu): a stick or D-pad direction selects the button at that angle around the ring's centre,
    // following the stick as it turns. BACK stays on B.
    const Button* ring[kMaxButtons];
    int nring = 0;
    NpPoint ringCentre{};
    for (const Button& b : buttons)
        if (!b.back) ring[nring++] = &b;
    if (nring >= 5) {
        for (int i = 0; i < nring; ++i) {
            ringCentre.x += ring[i]->centre.x;
            ringCentre.y += ring[i]->centre.y;
        }
        ringCentre.x /= nring;
        ringCentre.y /= nring;
        float lo = 1e30f, hi = 0.0f, sum = 0.0f;
        for (int i = 0; i < nring; ++i) {
            const float d = hypotf(ring[i]->centre.x - ringCentre.x, ring[i]->centre.y - ringCentre.y);
            lo = d < lo ? d : lo;
            hi = d > hi ? d : hi;
            sum += d;
        }
        if (sum <= 0.0f || (hi - lo) / (sum / nring) > 0.35f) nring = 0;
    } else {
        nring = 0;
    }
    if (nring > 0 && !stageSelect) {
        NpPoint aim = stick;
        if (hypotf(aim.x, aim.y) < 0.55f) {
            aim = {(dir & kRight) ? 1.0f : (dir & kLeft) ? -1.0f : 0.0f, (dir & kUp) ? 1.0f : (dir & kDown) ? -1.0f : 0.0f};
            if (!step) aim = {};
        }
        if (aim.x != 0.0f || aim.y != 0.0f) {
            const float want = atan2f(aim.y, aim.x);
            const Button* best = 0;
            float bestDiff = 1e30f;
            for (int i = 0; i < nring; ++i) {
                const Button* b = ring[i];
                float diff = fabsf(atan2f(b->centre.y - ringCentre.y, b->centre.x - ringCentre.x) - want);
                if (diff > 3.14159265f) diff = 6.2831853f - diff;
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = b;
                }
            }
            if (best && !best->Contains(at) &&
                !(st.settleFrames > 0 && st.goal.x == best->centre.x && st.goal.y == best->centre.y)) {
                snap_to(st, buttons, best->centre, cursor);
                select_at(chan, buttons, st.goal);
            }
        }
    }
    NpPoint goal{};
    const Button* current = 0;
    for (const Button& b : buttons)
        if (b.Contains(at)) {
            current = &b;
            break;
        }
    if (nring == 0 && step && pick_in_direction(buttons, current, at, dir, &goal)) {
        snap_to(st, buttons, goal, cursor);
        select_at(chan, buttons, st.goal);
        return r;
    }
    if (st.settleFrames > 0) {
        // The game picks the new position up on its next update; wait for it before re-checking.
        --st.settleFrames;
        if (hypotf(st.aim.x - at.x, st.aim.y - at.y) < 2.0f) st.settleFrames = 0;
        return r;
    }
    // Keep something selected. Off every button, the pointer follows the button it was on if that is still there (moved:
    // a screen's buttons slide as it comes or goes), and only after a few frames goes to the default: a screen in
    // transition briefly shows its buttons elsewhere, and snapping there every other frame makes the pointer jitter.
    bool onButton = false;
    for (const Button& b : buttons)
        if (b.Contains(at)) onButton = true;
    if (onButton) {
        st.offButtonFrames = 0;
        if (s_hasSelected[chan]) st.selectedListener = s_selected[chan].listener;
        return r;
    }
    const Button* followed = 0;
    for (const Button& b : buttons)
        if (b.listener == st.selectedListener) {
            followed = &b;
            break;
        }
    if (st.selectedListener != 0 && followed) {
        snap_to(st, buttons, followed->centre, cursor);
        select_at(chan, buttons, st.goal);
        return r;
    }
    if (++st.offButtonFrames < frames_for(0.1)) return r;
    st.offButtonFrames = 0;
    snap_to(st, buttons, pick_default(buttons, stageSelect), cursor);
    select_at(chan, buttons, st.goal);
    return r;
}

}  // namespace NpMenuNav
