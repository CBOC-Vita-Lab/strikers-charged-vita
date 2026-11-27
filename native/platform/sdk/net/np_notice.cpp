// A short banner in the game's own text for a warning of the online transport during a match ("The online service cannot be reached. The match goes on.").
//
// The transport sets OnlineInfo::warning when the signaling service fails while a link is up (online/np_online.cpp SignalingLost). The match is not
// affected by it: the lockstep runs over the links that exist. So the player is told, but nothing about the match waits for the message: this is not a
// popup (a popup holds the game's input lock and would starve the lockstep), it is text drawn with the game's font on top of the front-end scenes (the
// HUD overlays) of the frame, a few seconds, then it fades. It reads no pad, holds no lock and allocates nothing.
//
// FERender::RenderNotice (feRender.cpp) calls NpNoticeRender once per frame, after the last scene was drawn, with that scene's view and matrix.
// MSC_ONLINE_NOTICE=0 turns the banner off (the warning is still in the log); MSC_ONLINE_NOTICE_MS sets how long it stays (default 9000).
#include "platform/sdk/net/np_lan.h"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/np_wait_banner.h"
#include "platform/sdk/net/np_mpbeta.h"
#include "platform/sdk/net/np_settings_help.h"
#include "platform/sdk/net/online/np_online_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/Font/fontmanager.h"
#include "Game/NetworkSession.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glStruct.h"
#include "NL/nlColour.h"
#include "NL/nlFont.h"
#include "NL/nlTextBox.h"
#include "NL/platvmath.h"

extern "C" uint64_t np_time_ns(void) __attribute__((weak));   // the SDK layer's clock (virtual in scripted harness runs)

namespace {

const int kMaxChars = 120;

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

uint64_t NowMs() {
    if (np_time_ns) return np_time_ns() / 1000000ull;
    return 0;
}

char g_shown[128];            // the warning the banner belongs to
uint64_t g_startMs = 0;       // when the banner began (the first frame of the match after the warning appeared)
bool g_active = false;
bool g_done = false;          // this warning was shown (until the text changes)
bool g_logged = false;

}  // namespace

// ---- the banner's text and timing, without any game type (the host test of the state machine uses the same function) -------------------------------
// Returns the alpha 0..255 of the banner at `nowMs`, 0 when nothing is to be drawn. `inMatch`: the match is running. `warning`: the transport's sentence ("" = none).
int NpNoticeAlpha(const char* warning, int inMatch, unsigned long long nowMs, int showMs) {
    if (!warning || !*warning) {
        g_shown[0] = 0;
        g_active = false;
        g_done = false;
        g_logged = false;
        return 0;
    }
    if (strncmp(g_shown, warning, sizeof g_shown - 1) != 0) {   // a new sentence: show it again
        snprintf(g_shown, sizeof g_shown, "%s", warning);
        g_active = false;
        g_done = false;
        g_logged = false;
    }
    if (g_done || !inMatch) return 0;
    if (!g_active) {
        g_active = true;
        g_startMs = nowMs;
    }
    const int64_t age = int64_t(nowMs - g_startMs);
    if (age >= showMs) {
        g_done = true;
        g_active = false;
        return 0;
    }
    const int fade = 500;   // ms in and out
    if (age < fade) return int(255 * age / fade);
    if (age > showMs - fade) return int(255 * (showMs - age) / fade);
    return 255;
}

// One line of text in the game's font, centred, `y` front-end units below the middle (negative = low on the screen), alpha 1..255. English in, the game's language out.
static void DrawLine(GLView* view, const nlMatrix4& matView, const char* line, float y, int alpha) {
    nlFont* font = FontManager::Instance() ? FontManager::Instance()->GetFontByHashID(0) : 0;
    if (!font) return;

    // (1.3) in the game's language (np_text.h), sentence by sentence: the transport's reason and "The match goes on." are two sentences
    unsigned short wide[kMaxChars + 1];
    char translated[kMaxChars * 3 + 1];
    NpText::Sentences(NpText::CurrentLanguage(), line, translated, sizeof translated);
    NpText::ToUtf16(translated, wide, kMaxChars);

    // The same path as TLTextInstance::Render: the string is laid out in a box (centred in both directions), the matrix places the box (front-end units,
    // origin in the middle of the screen) and the view's matrix puts it on the screen.
    const float boxW = 720.0f, boxH = 64.0f, scale = 0.62f, x = 0.0f;
    nlMatrix4 local, text;
    nlMakeScaleMatrix(local, scale, scale, 1.0f);
    local.m41 += x;
    local.m42 += y;
    nlMultMatrices(text, local, matView);

    unsigned short buffer[kMaxChars + 8];
    FontCharString charString(wide, font, buffer);
    nlVector2 box;
    box.x = boxW / scale;
    box.y = boxH / scale;
    nlTextBox::StringDrawInfo info;
    memset(&info, 0, sizeof info);
    info.pFont = font;
    info.String = charString.m_pString;
    info.pMatrix = &text;
    const unsigned long options = nlTextBox::AlignCenter | nlTextBox::VAlignCenter | nlTextBox::FlipY;
    nlTextBox::ProcessString(&charString, font, box, options, &text, info);

    nlVector2 at;
    at.x = -box.x / 2.0f;
    at.y = box.y / 2.0f;
    nlColour colour;
    colour.c[0] = 255;
    colour.c[1] = 236;
    colour.c[2] = 120;
    colour.c[3] = (unsigned char)alpha;
    nlTextBox::DrawString(info, at, colour, view);
}

static void RenderWarning(GLView* view, const nlMatrix4& matView) {
    if (!view || !NpNetActive()) {
        char none = 0;
        NpNoticeAlpha(&none, 0, 0, 0);
        return;
    }
    static int on = -1;
    if (on < 0) on = EnvInt("MSC_ONLINE_NOTICE", 1) != 0;
    if (!on) return;
    const char* warning = NpNetWarning();
    const bool inMatch = g_pNetworkSessionBase && g_pNetworkSessionBase->GetSessionState() == NET_SESSION_IN_GAME;
    const int alpha = NpNoticeAlpha(warning, inMatch, NowMs(), EnvInt("MSC_ONLINE_NOTICE_MS", 9000));
    if (alpha <= 0) return;
    if (!g_logged) {
        g_logged = true;
        fprintf(stderr, "[online-notice] banner: %s\n", warning);
    }

    DrawLine(view, matView, warning, -196.0f, alpha);
}

// "Waiting for the other player..." with a seconds counter, after 1.5 s of lockstep steps without the partner's input (np_wait_banner.h). Render only:
// it reads NpLanWaitedMs() (a wall-clock measure kept by NpLanStep) and draws; nothing flows back. LAN and internet matches alike. MSC_WAIT_BANNER=0 turns it off.
static void RenderWaiting(GLView* view, const nlMatrix4& matView) {
    static int on = -1;
    if (on < 0) on = EnvInt("MSC_WAIT_BANNER", 1) != 0;
    if (!on || !g_pNetworkSessionBase || g_pNetworkSessionBase->GetSessionState() != NET_SESSION_IN_GAME) return;
    const unsigned waited = NpLanWaitedMs();
    const int alpha = NpWait::Alpha(waited);
    if (alpha <= 0) return;
    char line[96];
    snprintf(line, sizeof line, "Waiting for the other player... (%d s)", NpWait::Seconds(waited));
    DrawLine(view, matView, line, -140.0f, alpha);   // above the transport's warning line (-196), so both can show
}

// (mpbeta27) the result screen of a LAN or online match (session state GAME_END): one line low on the screen, "Multiplayer is in beta. Something wrong? Choose REPORT A
// PROBLEM in the Multiplayer menu.", faded in over half a second and kept while the screen is up. Render only (it reads the session state and draws); MSC_MP_BETA=0 turns it off.
static void RenderResultLine(GLView* view, const nlMatrix4& matView) {
    static int on = -1;
    if (on < 0) on = NpMpBeta::On() ? 1 : 0;
    static uint64_t since = 0;
    static bool counted = false;
    if (!on || !g_pNetworkSessionBase || g_pNetworkSessionBase->GetSessionState() != NET_SESSION_GAME_END) {
        since = 0;
        counted = false;
        return;
    }
    const uint64_t now = NowMs();
    if (!since) since = now ? now : 1;
    if (!counted) {
        counted = true;
        ++NpMpBeta::Count().stamp;
        fprintf(stderr, "[mpbeta] result screen line shown\n");
    }
    const uint64_t age = now - since;
    const int alpha = age >= 500 ? 220 : int(220 * age / 500);
    if (alpha > 0) DrawLine(view, matView, NpMpBeta::ResultLine(), -236.0f, alpha);
}

// ---- set18: the VITA SETTINGS help line (np_settings_help.h) -------------------------------------------------------------------------------------------
namespace {
unsigned short g_helpText[kMaxChars + 1];
const nlFont* g_helpFont = 0;
nlColour g_helpColour;
bool g_helpOn = false;
const void* g_helpScene = 0;
}  // namespace

void NpSettingsHelpShow(const char* utf8, const nlFont* font, const nlColour* colour, const void* scene) {
    if (!utf8 || !*utf8 || !scene) {
        g_helpOn = false;
        g_helpScene = 0;
        return;
    }
    g_helpScene = scene;
    NpText::ToUtf16(utf8, g_helpText, kMaxChars);
    g_helpFont = font;
    if (colour) {
        g_helpColour = *colour;
        g_helpColour.c[3] = 255;   // (the bars fade in by their slide's animation; the asset colour's own alpha is not what is on screen)
    }
    else {
        g_helpColour.c[0] = g_helpColour.c[1] = g_helpColour.c[2] = 255;
        g_helpColour.c[3] = 255;
    }
    g_helpOn = true;
}

// Where the line sits: the panel's steel foot under the DONE bar, centred, two rows at most, in front-end units (origin in the middle of the screen, y up). The box is
// narrower than the steel so the words stay off the frame's rivets; the lettering is the bars' own, smaller.
// FERender::RenderScene calls this after each scene: the panel's view and matrix are kept, and the line is drawn at the end of the frame's scene list (NpNoticeRender),
// in that view, so the scenes drawn after the panel (its steel frame among them) are under it.
namespace {
GLView* g_helpView = 0;
nlMatrix4 g_helpMatView;
}  // namespace

void NpSettingsHelpRender(const void* scene, GLView* view, const nlMatrix4& matView) {
    if (!g_helpOn || !view || scene != g_helpScene) return;
    g_helpView = view;
    g_helpMatView = matView;
}

static void RenderSettingsHelp() {
    GLView* view = g_helpView;
    g_helpView = 0;
    if (!g_helpOn || !view) return;
    const nlMatrix4& matView = g_helpMatView;
    const nlFont* font = g_helpFont ? g_helpFont : (FontManager::Instance() ? FontManager::Instance()->GetFontByHashID(0) : 0);
    if (!font) return;
    const float y = -180.0f, scale = 0.5f, boxW = 380.0f, z = 0.0f;   // (the panel's steel foot under DONE; tuned on the host picture)
    const float boxH = 40.0f;
    nlMatrix4 local, text;
    nlMakeScaleMatrix(local, scale, scale, 1.0f);
    local.m42 += y;
    local.m43 += z;
    nlMultMatrices(text, local, matView);
    unsigned short buffer[kMaxChars + 8];
    FontCharString charString(g_helpText, font, buffer);
    nlVector2 box;
    box.x = boxW / scale;
    box.y = boxH / scale;
    nlTextBox::StringDrawInfo info;
    memset(&info, 0, sizeof info);
    info.pFont = font;
    info.String = charString.m_pString;
    info.pMatrix = &text;
    const unsigned long options = nlTextBox::AlignCenter | nlTextBox::VAlignCenter | nlTextBox::FlipY;
    nlTextBox::ProcessString(&charString, font, box, options, &text, info);
    nlVector2 at;
    at.x = -box.x / 2.0f;
    at.y = box.y / 2.0f;
    extern int g_npFontLayer;   // (nlFont.cpp: the view layer of the glyphs; 1 = after the panel's own layer, over its steel frame)
    g_npFontLayer = 1;
    nlTextBox::DrawString(info, at, g_helpColour, view);
    g_npFontLayer = 0;
}

void NpNoticeRender(GLView* view, const nlMatrix4& matView) {
    if (!view) {
        char none = 0;
        NpNoticeAlpha(&none, 0, 0, 0);
        return;
    }
    RenderWaiting(view, matView);
    RenderWarning(view, matView);
    RenderResultLine(view, matView);
    RenderSettingsHelp();
}

// r16 (fable-da21 F3): a LAN or online match is running (NET_SESSION_IN_GAME); the keep-awake idle limit does not apply then (np_keep_awake.h).
bool NpLanInMatch() { return g_pNetworkSessionBase && g_pNetworkSessionBase->GetSessionState() == NET_SESSION_IN_GAME; }
