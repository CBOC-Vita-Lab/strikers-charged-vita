// np_prompts.cpp: the game's button prompts in the Vita's names (W5 CTL).
//
// The tutorials (STRIKERS 101 lessons), the HUD tips and the help pages name Wii buttons in their text: "PRESS THE {clr:FFA800}A{nbs}BUTTON
// {clr:pop}", "SHAKE Wii REMOTE TO HIT!", "FLICK NUNCHUK TO TOGGLE POWER-UPS.". When a localisation table is loaded (nlLocalization.cpp,
// OnTableLoaded) every string is rewritten for the layout the player chose (np_controls.h): the A button becomes CROSS, B becomes CIRCLE
// (MODERN) or SQUARE (CLASSIC) and so on: the names come from the controls table (np_control_table.h roles), so the texts, the
// Settings controls page and the pause menu's controller map say the same. The table is rewritten in place, so a new text must not be longer than the old one; a string the
// rules would lengthen is left as it is (counted in the log line). Only the English tables have these sentences; the others are not touched.
// The pictures that show a Wii button (the title screen's A, the HUD's icons, the controller map) are replaced when the game makes the
// texture (glx_MakeTexture calls NpCtlTextureOverride below; np_prompt_art.h draws them). MSC_PROMPTS=0 leaves every text and picture as
// the game has it.
#include "NL/nlLocalization.h"
#include "NL/glx/glxTexture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdk/input/np_control_table.h"
#include "platform/sdk/input/np_controls.h"
#include "platform/sdk/input/np_prompt_art.h"

static_assert(sizeof(GXTextureHeader) == sizeof(NpPromptArt::TexHeader), "np_prompt_art.h mirrors GXTextureHeader");

namespace {

struct Rule {
    const char* from;
    const char* to;
};

// The rules are made when a table loads, from the controls table (np_control_table.h roles: the same names the controls page and the
// controller map use) and the player's switches. Longest first where one rule's text contains another's.
#define NP_BTN(x) "{clr:FFA800}" x "{nbs}BUTTON{clr:pop}"
const int kMaxRules = 40;
Rule g_rules[kMaxRules];
char g_ruleText[kMaxRules][112];
int g_ruleCount = 0;
void AddRule(const char* from, const char* fmt, const char* a = "", const char* b = "") {
    if (g_ruleCount >= kMaxRules) return;
    snprintf(g_ruleText[g_ruleCount], sizeof g_ruleText[0], fmt, a, b);
    g_rules[g_ruleCount].from = from;
    g_rules[g_ruleCount].to = g_ruleText[g_ruleCount];
    ++g_ruleCount;
}
void MakeRules(const NpControlTable::Config& cfg) {
    using namespace NpControlTable;
    g_ruleCount = 0;
    const int L = cfg.layout;
    // "PRESS THE (A BUTTON)" -> "PRESS (CROSS)" (the key keeps the game's orange)
    static const struct { const char* withThe; const char* plain; Role role; } kKeys[] = {
        {"THE " NP_BTN("A"), NP_BTN("A"), kRoleA}, {"THE " NP_BTN("B"), NP_BTN("B"), kRoleB}, {"THE " NP_BTN("C"), NP_BTN("C"), kRoleC},
        {"THE " NP_BTN("Z"), NP_BTN("Z"), kRoleZ}, {"THE " NP_BTN("1"), NP_BTN("1"), kRoleOne},
    };
    for (const auto& k : kKeys) AddRule(k.withThe, "{clr:FFA800}%s{clr:pop}", RoleName(L, k.role));
    for (const auto& k : kKeys) AddRule(k.plain, "{clr:FFA800}%s{clr:pop}", RoleName(L, k.role));
    AddRule("{clr:FFA800}+CONTROL{nbs}PAD{clr:pop}", "{clr:FFA800}%s{clr:pop}", RoleName(L, kRoleDpad));
    // the Wii-only gestures: what the Vita does instead
    AddRule("OR SHAKE THE Wii REMOTE TO THROW A HIT.", "OR PRESS %s TO THROW A HIT.", RoleName(L, kRoleShake));
    AddRule("SHAKE Wii REMOTE TO HIT!", "%s TO HIT!", RoleName(L, kRoleShake));
    if (cfg.rear) AddRule("FLICK NUNCHUK TO TOGGLE POWER-UPS.", "%s/TAP BACK: NEXT POWER-UP.", RoleName(L, kRoleFlick));
    else AddRule("FLICK NUNCHUK TO TOGGLE POWER-UPS.", "%s TO TOGGLE POWER-UPS.", RoleName(L, kRoleFlick));
    AddRule("POINT YOUR Wii REMOTE AT THE SCREEN TO TAKE CONTROL OF THE GOALIE'S HANDS!",
            cfg.rearSave ? "AIM WITH THE REAR PAD; CROSS OR R TO BLOCK!" :
            cfg.touchSave ? "MOVE THE GOALIE'S HANDS WITH A STICK OR A FINGER!" : "MOVE THE GOALIE'S HANDS WITH A STICK!");
    AddRule("WILL TRIGGER A SOUND IN YOUR Wii REMOTE AND THEN APPEAR IN THE HUD.", "WILL APPEAR IN THE HUD.");
    // the controller checks of the Wii (the Vita's controller and its Nunchuk view are always there)
    AddRule("PLEASE ENSURE THAT ALL PLAYERS HAVE A NUNCHUK ATTACHED TO THE Wii REMOTE.", "PLEASE CHECK THAT EVERY PLAYER HAS A CONTROLLER.");
    AddRule("PLEASE ENSURE THAT THE NUNCHUK IS ATTACHED TO THE Wii REMOTE.", "PLEASE CHECK THE CONTROLLER.");
    // pointing and clicking with the remote: the Vita picks with the D-pad / sticks and Cross (or a finger)
    AddRule("CLICK TO CHOOSE A CHEAT", "CHOOSE A CHEAT");
    AddRule("CLICK ON ANY OF THE SEVEN ICONS", "PICK ANY OF THE SEVEN ICONS");
    AddRule("CLICK ON ", "CHOOSE ");
    AddRule("CLICK HERE", "PICK HERE");   // the captain panels of the Domination set-up
    // the Wii's system: the HOME menu and the system memory
    AddRule("RETURN TO THE Wii MENU", "QUIT THE GAME");
    AddRule("Wii SYSTEM MEMORY", "SYSTEM MEMORY");
    AddRule("REFER TO THE Wii OPERATIONS MANUAL", "REFER TO THE OPERATIONS MANUAL");
    AddRule("Wii REMOTE", "CONTROLLER");
}
#undef NP_BTN

bool dump() {
    const char* e = getenv("MSC_CTL_TRACE");
    return e && atoi(e) >= 2;
}
bool traceTextures() {
    const char* e = getenv("MSC_CTL_TRACE");
    return e && atoi(e) >= 3;
}
bool enabled() {
    const char* e = getenv("MSC_PROMPTS");
    return !(e && *e == '0');
}

// Applies the rules to one NUL-terminated UTF-16 string in place. Returns 1 changed, 0 untouched, -1 the result would not fit.
int rewrite(unsigned short* s, const Rule* rules, size_t ruleCount, unsigned short* scratch, size_t scratchLen) {
    size_t len = 0;
    while (s[len]) ++len;
    if (len == 0 || len >= scratchLen) return 0;
    // quick reject: every rule names a key ('{'), "Wii", the NUNCHUK or CLICKs
    bool candidate = false;
    for (size_t i = 0; i < len && !candidate; ++i)
        candidate = s[i] == '{' || (s[i] == 'W' && i + 2 < len && s[i + 1] == 'i' && s[i + 2] == 'i') || (s[i] == 'N' && i + 6 < len && s[i + 1] == 'U') ||
                    (s[i] == 'C' && i + 4 < len && s[i + 1] == 'L' && s[i + 2] == 'I');
    if (!candidate) return 0;
    size_t out = 0;
    bool changed = false;
    for (size_t i = 0; i < len;) {
        bool matched = false;
        for (size_t r = 0; r < ruleCount && !matched; ++r) {
            const char* from = rules[r].from;
            size_t k = 0;
            while (from[k] && i + k < len && s[i + k] == (unsigned char)from[k]) ++k;
            if (from[k]) continue;
            const char* to = rules[r].to;
            for (size_t t = 0; to[t]; ++t) {
                if (out >= scratchLen - 1) return -1;
                scratch[out++] = (unsigned char)to[t];
            }
            i += k;
            matched = changed = true;
        }
        if (!matched) {
            if (out >= scratchLen - 1) return -1;
            scratch[out++] = s[i++];
        }
    }
    if (!changed) return 0;
    if (out > len) return -1;   // in place only: the table's layout must not change
    for (size_t i = 0; i < len; ++i) s[i] = i < out ? scratch[i] : 0;
    return 1;
}

}  // namespace

extern "C" void NpCtlPatchLocalization(nlLocalization* localization) {
    if (!localization || !localization->m_pFile || !localization->m_LookupTable || !localization->m_FirstString || !enabled()) return;
    const NpControls::Layout layout = NpControls::ConfiguredLayout();
    MakeRules(NpControlTable::ConfigFromEnv());
    const Rule* rules = g_rules;
    const size_t ruleCount = (size_t)g_ruleCount;
    static unsigned short scratch[2048];
    int changed = 0, tooLong = 0;
    const unsigned long strings = localization->m_pFile->StringCount;
    for (unsigned long i = 0; i < strings; ++i) {
        unsigned short* s = localization->m_FirstString + localization->m_LookupTable[i].StringOffset;
        const int r = rewrite(s, rules, ruleCount, scratch, sizeof scratch / sizeof scratch[0]);
        if (r > 0) ++changed;
        else if (r < 0) ++tooLong;
        if (r != 0 && dump()) {
            // MSC_CTL_TRACE=2: the new texts (ASCII), or the ones left alone
            char line[600];
            size_t n = 0;
            for (const unsigned short* c = s; *c && n < sizeof line - 1; ++c) line[n++] = *c < 128 ? (char)*c : '?';
            line[n] = 0;
            fprintf(stderr, "[ctl] prompt %s: %s\n", r > 0 ? "now" : "kept (too long)", line);
        }
    }
    if (changed || tooLong)
        fprintf(stderr, "[ctl] prompts: %d string(s) name the Vita's buttons (%s), %d left as they are (too long)\n", changed,
                NpControls::LayoutName(layout), tooLong);
}

// ---- the pictures ----------------------------------------------------------------------------------------------------------
// glx_MakeTexture (src/NL/glx/glxTexture.cpp, NP_NATIVE) asks for every texture it makes: a replacement (header + texels of the same
// size, format and levels) or 0. A picture is drawn once and kept (the front end loads the same ones again for every menu); the game
// copies it into its own texture at once. Textures may be made on more than one thread: the cache takes a spin lock.
namespace {
struct CachedPicture {
    unsigned long hash;
    uint8_t* blob;   // 0 = the original is kept (not ours at this size)
};
CachedPicture gPictures[48];
int gPictureCount = 0;
volatile int gPictureLock = 0;
void LockPictures() {
    while (__sync_lock_test_and_set(&gPictureLock, 1)) {
    }
}
void UnlockPictures() { __sync_lock_release(&gPictureLock); }
}  // namespace

extern "C" const void* NpCtlTextureOverride(unsigned long hash, const void* header) {
    if (header && traceTextures()) {
        // MSC_CTL_TRACE=3: every texture the game makes (the inventory)
        const NpPromptArt::TexHeader* t = (const NpPromptArt::TexHeader*)header;
        fprintf(stderr, "[ctl] texture %08lx %ux%u fmt=%u levels=%u%s\n", hash, (unsigned)t->width, (unsigned)t->height, (unsigned)t->format,
                (unsigned)t->numLevels, NpPromptArt::Known((uint32_t)hash) ? " (ours)" : "");
    }
    if (!header || !enabled() || !NpPromptArt::Known((uint32_t)hash)) return 0;
    LockPictures();
    for (int i = 0; i < gPictureCount; ++i)
        if (gPictures[i].hash == hash) {
            uint8_t* blob = gPictures[i].blob;
            UnlockPictures();
            return blob;
        }
    UnlockPictures();
    const NpControls::Layout layout = NpControls::ConfiguredLayout();
    const NpPromptArt::TexHeader* h = (const NpPromptArt::TexHeader*)header;
    size_t bytes = 0;
    uint8_t* blob = NpPromptArt::Build((uint32_t)hash, h, (const uint8_t*)header + sizeof(NpPromptArt::TexHeader), NpControlTable::ConfigFromEnv(), &bytes);
    LockPictures();
    bool kept = false;
    for (int i = 0; i < gPictureCount; ++i)
        if (gPictures[i].hash == hash) {   // another thread made it meanwhile
            free(blob);
            blob = gPictures[i].blob;
            kept = true;
            break;
        }
    if (!kept && gPictureCount < (int)(sizeof gPictures / sizeof gPictures[0])) {
        gPictures[gPictureCount].hash = hash;
        gPictures[gPictureCount].blob = blob;
        ++gPictureCount;
        kept = true;
    }
    UnlockPictures();
    if (!kept) {   // the cache is full (more pictures than the table has): keep the original
        free(blob);
        return 0;
    }
    if (dump() || !blob)
        fprintf(stderr, "[ctl] picture %08lx %s: %s (%s)\n", hash, blob ? "drawn" : "kept (unexpected size)", NpPromptArt::Describe((uint32_t)hash),
                NpControls::LayoutName(layout));
    return blob;
}
