// np_prompt_art.cpp: see np_prompt_art.h.
#include "np_prompt_art.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "np_art.h"
#include "np_control_table.h"

namespace NpPromptArt {

namespace {

using namespace NpArt;

// how much wider than tall the overlay shows the controller map picture (measured in the game's 960 x 544 frame)
const float kMapAspect = 16.0f / 9.0f;

enum Kind {
    kButtonA,        // fe/screens/images/a_button: the Wii Remote's A button (round, light face)
    kFontGlyph,      // a font page (CI8 + palette) whose U+22A5 glyph is the A button ("PRESS \u22A5 TO START")
    kButtonB,        // b_button: the Wii Remote's B trigger -> the layout's shoot / back button, drawn like the A icon
    kDpad,           // dpad: the + Control Pad, the down arm lit -> the Vita's four keys, down lit
    kController,     // wii_controller, 1controller: a Wii Remote -> a PS Vita
    kPlusMinus,      // button_plus(_over), button_minus(_over): the + / - buttons (menus: next / previous page) -> R / L
    kCursorHolding,  // cursorN_holding: the player's hand holding a Wii Remote -> holding a PS Vita
    kHomeAwayBox,    // home_away_box: a team box with a Wii Remote pictured in it -> a PS Vita
    kControllerMap,  // the pause menu's controller map (a photo of the Wii Remote + Nunchuk with labels) -> the controls table's lines
};

struct Entry {
    uint32_t hash;
    Kind kind;
    uint16_t width, height;
    uint8_t levels;
    uint8_t format;           // eGXTextureFormat: 2 CMPR, 8 CI8
    uint16_t gx, gy, gw, gh;  // kFontGlyph: the glyph's cell on the page (nlFont::Load's packing of the font description)
    const char* what;
};

// The textures (by name hash), as found in the front-end bundles (tools/ctl/fe_textures.py; docs/wave5-CTL.md has the inventory).
// The fonts: the localisation writes the A button as U+22A5 ("PRESS \u22A5 TO START"), and the fonts the game loads (FontLoading.cpp:
// art/fe/fonts/eur*.res, jpn* for Japanese) have a picture of the button as that glyph. (The eng/uke/lng/... fonts on the disc, which draw
// it as their '@', are never loaded.)
const Entry kEntries[] = {
    {0x97bf18bcu, kButtonA, 32, 32, 1, 2, 0, 0, 0, 0, "fe/screens/images/a_button (A button icon)"},
    {0xf31bd653u, kFontGlyph, 512, 512, 1, 8, 396, 387, 43, 43, "eurfonttext18 page: the A button glyph U+22A5 (PRESS (A) TO START)"},
    {0x3064505fu, kFontGlyph, 512, 512, 1, 8, 282, 150, 43, 75, "eurfontheading36 page 2: the A button glyph U+22A5"},
    {0xbdc3b4efu, kFontGlyph, 512, 512, 1, 8, 337, 396, 31, 36, "jpnfonttext18 page 1: the A button glyph U+22A5"},
    {0xdf64dd80u, kFontGlyph, 512, 512, 1, 8, 188, 72, 31, 36, "jpnfonttextingame18 page 1: the A button glyph U+22A5"},
    {0x8400ed9du, kButtonB, 32, 32, 1, 2, 0, 0, 0, 0, "fe/screens/images/b_button (B trigger icon)"},
    {0xa940ba59u, kDpad, 64, 64, 1, 2, 0, 0, 0, 0, "fe/screens/images/dpad (+ Control Pad, down lit)"},
    {0xa2dc8b8cu, kController, 64, 64, 1, 2, 0, 0, 0, 0, "fe/screens/images/wii_controller (a Wii Remote)"},
    {0x7f92d155u, kController, 64, 64, 1, 2, 1, 0, 0, 0, "fe/screens/images/1controller (a Wii Remote, big)"},
    {0x93b1c29fu, kPlusMinus, 128, 128, 1, 2, NpControlTable::kRolePlus, 0, 0, 0, "fe/screens/images/button_plus (+ button)"},
    {0x8cf5375au, kPlusMinus, 128, 128, 1, 2, NpControlTable::kRolePlus, 0, 0, 0, "fe/screens/images/button_plus_over (+ button)"},
    {0x09b20ac7u, kPlusMinus, 128, 128, 1, 2, NpControlTable::kRoleMinus, 0, 0, 0, "fe/screens/images/button_minus (- button)"},
    {0x13b0d882u, kPlusMinus, 128, 128, 1, 2, NpControlTable::kRoleMinus, 0, 0, 0, "fe/screens/images/button_minus_over (- button)"},
    {0x545a6ab2u, kCursorHolding, 64, 128, 1, 2, 0, 0, 0, 0, "fe/screens/images/cursor0_holding (hand holding a Wii Remote)"},
    {0xc8d6dbb3u, kCursorHolding, 64, 128, 1, 2, 0, 0, 0, 0, "fe/screens/images/cursor1_holding (hand holding a Wii Remote)"},
    {0x3d534cb4u, kCursorHolding, 64, 128, 1, 2, 0, 0, 0, 0, "fe/screens/images/cursor2_holding (hand holding a Wii Remote)"},
    {0xb1cfbdb5u, kCursorHolding, 64, 128, 1, 2, 0, 0, 0, 0, "fe/screens/images/cursor3_holding (hand holding a Wii Remote)"},
    {0xe9c3df42u, kHomeAwayBox, 256, 256, 1, 2, 0, 0, 0, 0, "fe/screens/images/home_away_box (team box with a Wii Remote)"},
    // the controller maps: US English (controller_config.fen's own image), Japanese, and the async images of the other languages
    // (OverlayHandlerControllerMap.cpp: fe/controller_maps/CONTROLLER_CONFIG_<language> in controllermapui.res). The pictures are
    // drawn in English: the other languages' tables have no Vita texts.
    {0xea5b7839u, kControllerMap, 512, 512, 1, 2, 0, 0, 0, 0, "fe/screens/images/controller config_test (controller map, US English)"},
    {0x573d148eu, kControllerMap, 512, 512, 7, 2, 0, 0, 0, 0, "fe/controller_maps/controller_config_jp (controller map, Japanese)"},
    {0x18b97d6au, kControllerMap, 512, 512, 7, 2, 0, 0, 0, 0, "controllermapui.res 18b97d6a (controller map, another language)"},
    {0x1a2a83eeu, kControllerMap, 512, 512, 7, 2, 0, 0, 0, 0, "controllermapui.res 1a2a83ee (controller map, another language)"},
    {0x1b38d076u, kControllerMap, 512, 512, 7, 2, 0, 0, 0, 0, "controllermapui.res 1b38d076 (controller map, another language)"},
    {0x13aba88au, kControllerMap, 512, 512, 7, 2, 0, 0, 0, 0, "controllermapui.res 13aba88a (controller map, another language)"},
    {0x2ca8ab7eu, kControllerMap, 512, 512, 7, 2, 0, 0, 0, 0, "controllermapui.res 2ca8ab7e (controller map, UK English)"},
};

// The controller map: the controls table's lines for the player's configuration, on a panel over the original picture (its rough edge
// stays as the frame). The picture is shown wider than it is (the overlay stretches it): it is drawn on a canvas `aspect` times as wide
// and squeezed back, so the text keeps its proportions on the screen.
void DrawControllerMap(Image& im, const NpControlTable::Config& cfg, float aspect) {
    using namespace NpControlTable;
    const int W = (int)(512.0f * aspect + 0.5f), H = 512;
    Image c(W, H);
    if (!c.Ok()) return;
    const Rgba white = MakeRgba(250, 250, 250), outline = MakeRgba(0, 0, 0), orange = MakeRgba(255, 168, 0), blue = MakeRgba(120, 196, 236);
    const Rgba grey = MakeRgba(120, 132, 138);
    // the panel (opaque: nothing of the Wii picture shows through), a little lighter at the top
    for (int y = 14; y < H - 14; ++y) {
        const float k = (float)(y - 14) / (float)(H - 28);
        const Rgba row = MakeRgba((int)(26 - 12 * k), (int)(54 - 24 * k), (int)(64 - 28 * k));
        for (int x = 14; x < W - 14; ++x) c.At(x, y) = row;
    }
    // the corners rounded off
    const int rad = 16;
    for (int y = 14; y < 14 + rad; ++y)
        for (int x = 14; x < 14 + rad; ++x) {
            const float dx = (float)(14 + rad - x) - 0.5f, dy = (float)(14 + rad - y) - 0.5f;
            if (dx * dx + dy * dy > rad * rad) {
                c.At(x, y).a = 0;
                c.At(W - 1 - x, y).a = 0;
                c.At(x, H - 1 - y).a = 0;
                c.At(W - 1 - x, H - 1 - y).a = 0;
            }
        }
    // title, layout, a Vita
    const bool modern = cfg.layout == 1;
    int x = 34;
    x += DrawText(c, x, 8, "CONTROLS ", 1, white, outline, 2);
    DrawText(c, x, 8, modern ? "MODERN" : "CLASSIC", 1, orange, outline, 2);
    DrawVita(c, (float)W - 120.0f, 38.0f, 140.0f, MakeRgba(232, 232, 236), MakeRgba(30, 30, 34), MakeRgba(50, 70, 80));
    // the table
    const int col0 = 34, col1 = (int)(W * 0.275f), col2 = (int)(W * 0.625f), wCol1 = col2 - col1 - 14, wCol2 = W - 30 - col2;
    int y = 66;
    DrawText(c, col0, y, "BUTTON", 0, blue, outline, 1);
    DrawText(c, col1, y, "WITH THE BALL", 0, blue, outline, 1);
    DrawText(c, col2, y, "WITHOUT THE BALL", 0, blue, outline, 1);
    y += 22;
    FillRect(c, 26, y, W - 26, y + 2, MakeRgba(90, 150, 170));
    y += 4;
    // 20 pixels a line (capitals: the 21-pixel cell's bottom rows are empty), so the last line ends above the overlay's DONE button
    // (the bottom eighth of the picture, in the middle)
    const int pitch = 20;
    char buf[160];
    bool rule = false;
    for (int i = 0; i < LineCount(); ++i) {
        const Line& l = LineAt(i);
        if (!Shown(l, cfg)) continue;
        if (IsSentence(l)) {
            if (!rule) {
                y += 2;
                FillRect(c, 26, y, W - 26, y + 2, MakeRgba(90, 150, 170));
                y += 4;
                rule = true;
            }
            int sx = col0 + DrawText(c, col0, y, l.input, 0, orange, outline, 1);
            sx += DrawText(c, sx, y, ": ", 0, white, outline, 1);
            DrawTextFit(c, sx, y, Sentence(l, cfg, buf, sizeof buf), 0, white, outline, 1, W - 30 - sx);
            y += pitch;
            continue;
        }
        const bool on = Active(l, cfg);
        // a face button's own symbol before its name
        int nx = col0;
        static const struct { const char* name; Symbol s; } kFace[] = {{"CROSS", kCross}, {"CIRCLE", kCircle}, {"SQUARE", kSquare}, {"TRIANGLE", kTriangle}};
        for (const auto& f : kFace)
            if (!strcmp(l.input, f.name)) {
                DrawRoundButton(c, (float)col0 + 9.0f, (float)y + 10.5f, 9.0f, f.s);
                nx = col0 + 24;
            }
        DrawTextFit(c, nx, y, l.input, 0, on ? orange : grey, outline, 1, col1 - nx - 10);
        DrawTextFit(c, col1, y, on ? l.withBall : "(REAR TOUCH OFF)", 0, on ? white : grey, outline, 1, wCol1);
        if (on) DrawTextFit(c, col2, y, l.withoutBall, 0, white, outline, 1, wCol2);
        y += pitch;
    }
    // the original's rough edge stays as the frame, but nothing of its pictures: a pixel that is not the background's grey-teal (the
    // hands, the white remote, the orange labels) takes the background's colour
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < im.w; ++xx) {
            Rgba& p = im.At(xx, yy);
            const int lum = (p.r * 3 + p.g * 6 + p.b) / 10;
            if (p.b + 12 < p.r || lum > 185) p = MakeRgba(58, 92, 102);
        }
    // squeeze the canvas to the texture's width and lay it over the original
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < im.w; ++xx) {
            const float s0 = (float)xx * (float)W / (float)im.w, s1 = (float)(xx + 1) * (float)W / (float)im.w;
            float r = 0, g = 0, b = 0, a = 0, wsum = 0;
            for (float u = s0; u < s1 - 1e-4f;) {
                const float next = fminf(s1, floorf(u) + 1.0f);
                const float wgt = next - u;
                const int sx = (int)u < W ? (int)u : W - 1;
                const Rgba& p = c.At(sx, yy);
                const float pa = (float)p.a / 255.0f * wgt;
                r += p.r * pa;
                g += p.g * pa;
                b += p.b * pa;
                a += pa;
                wsum += wgt;
                u = next;
            }
            if (a <= 0.0f) continue;
            Blend(im, xx, yy, MakeRgba((int)(r / a), (int)(g / a), (int)(b / a)), a / wsum);
        }
}

// ---- CI8 (8-bit indices in 8 x 4 tiles, an RGB5A3 palette) -------------------------------------------------------------------------
size_t Ci8Offset(int width, int x, int y) { return ((size_t)(y / 4) * (size_t)(width / 8) + (size_t)(x / 8)) * 32 + (size_t)(y % 4) * 8 + (size_t)(x % 8); }
Rgba From5A3(unsigned v) {
    if (v & 0x8000) {
        const int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return MakeRgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2);
    }
    const int a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return MakeRgba(r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1);
}
unsigned To5A3(Rgba c) {
    if (c.a >= 0xF0) return 0x8000u | (unsigned)(c.r >> 3) << 10 | (unsigned)(c.g >> 3) << 5 | (unsigned)(c.b >> 3);
    return (unsigned)(c.a >> 5) << 12 | (unsigned)(c.r >> 4) << 8 | (unsigned)(c.g >> 4) << 4 | (unsigned)(c.b >> 4);
}
int Dist(Rgba a, Rgba b) {
    const int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b, da = a.a - b.a;
    return dr * dr + dg * dg + db * db + 2 * da * da;
}

// Redraws the A button glyph of a font page (CI8): the glyph's cell is decoded with the palette, the button's face found (its light
// pixels), the letter replaced by the symbol; changed pixels take the nearest palette colour, or a new one from the palette's unused
// tail (the font pages use about 100 of their 256 entries).
bool RedrawFontGlyph(const Entry& e, uint8_t* texels, uint8_t* palette, int symbol) {
    const int W = e.width;
    uint16_t pal[256];
    for (int i = 0; i < 256; ++i) pal[i] = (uint16_t)(palette[2 * i] << 8 | palette[2 * i + 1]);
    bool used[256] = {false};
    const size_t count = (size_t)e.width * (size_t)e.height;
    for (size_t i = 0; i < count; ++i) used[texels[i]] = true;
    Image cell(e.gw, e.gh), before(e.gw, e.gh);
    if (!cell.Ok() || !before.Ok()) return false;
    double sx = 0, sy = 0;
    int light = 0;
    for (int y = 0; y < e.gh; ++y)
        for (int x = 0; x < e.gw; ++x) {
            const Rgba c = From5A3(pal[texels[Ci8Offset(W, e.gx + x, e.gy + y)]]);
            cell.At(x, y) = before.At(x, y) = c;
            if (c.a > 128 && (c.r + c.g + c.b) / 3 > 150) {
                sx += x + 0.5;
                sy += y + 0.5;
                ++light;
            }
        }
    if (light < 200) return false;   // not the button we know
    const float cx = (float)(sx / light), cy = (float)(sy / light);
    const float r = sqrtf((float)light / 3.14159265f) * 1.04f;   // the face: a disc of the light pixels' area
    RedrawRoundButton(cell, cx, cy, r, (Symbol)symbol, r * 0.95f);
    int next = 0;
    for (int y = 0; y < e.gh; ++y)
        for (int x = 0; x < e.gw; ++x) {
            const Rgba c = cell.At(x, y);
            const Rgba o = before.At(x, y);
            if (c.r == o.r && c.g == o.g && c.b == o.b && c.a == o.a) continue;
            int best = -1, bestD = 1 << 30;
            for (int i = 0; i < 256; ++i) {
                if (!used[i]) continue;
                const int d = Dist(c, From5A3(pal[i]));
                if (d < bestD) {
                    bestD = d;
                    best = i;
                }
            }
            if (bestD > 3 * 10 * 10) {
                while (next < 256 && used[next]) ++next;
                if (next < 256) {
                    pal[next] = (uint16_t)To5A3(c);
                    used[next] = true;
                    best = next;
                }
            }
            if (best >= 0) texels[Ci8Offset(W, e.gx + x, e.gy + y)] = (uint8_t)best;
        }
    for (int i = 0; i < 256; ++i) {
        palette[2 * i] = (uint8_t)(pal[i] >> 8);
        palette[2 * i + 1] = (uint8_t)pal[i];
    }
    return true;
}

const Entry* Find(uint32_t hash) {
    for (size_t i = 0; i < sizeof kEntries / sizeof kEntries[0]; ++i)
        if (kEntries[i].hash == hash) return &kEntries[i];
    return 0;
}

// the original as an image (CMPR, top level)
bool Original(const TexHeader* h, const uint8_t* texels, Image& im) { return Decode((int)h->format, texels, h->width, h->height, 0, 0, im); }

}  // namespace

const char* Describe(uint32_t hash) {
    const Entry* e = Find(hash);
    return e ? e->what : "";
}

bool Known(uint32_t hash) { return Find(hash) != 0; }

uint8_t* Build(uint32_t hash, const TexHeader* header, const uint8_t* texels, const NpControlTable::Config& config, size_t* bytes) {
    const int layout = config.layout;
    const Entry* e = Find(hash);
    if (!e || !header || !texels || header->format != e->format || header->width != e->width || header->height != e->height ||
        header->numLevels != e->levels || header->numEntries != (e->format == 8 ? 256u : 0u))
        return 0;
    const int symbolA = NpControlTable::RoleSymbol(layout, NpControlTable::kRoleA);
    if (e->kind == kFontGlyph) {
        // CI8: the page and its palette, with the glyph's cell redrawn
        const size_t texBytes = (size_t)e->width * (size_t)e->height, total = sizeof(TexHeader) + texBytes + 512;
        uint8_t* out = (uint8_t*)malloc(total);
        if (!out) return 0;
        memcpy(out, header, sizeof(TexHeader));
        memcpy(out + sizeof(TexHeader), texels, texBytes + 512);
        if (!RedrawFontGlyph(*e, out + sizeof(TexHeader), out + sizeof(TexHeader) + texBytes, symbolA)) {
            free(out);
            return 0;
        }
        if (bytes) *bytes = total;
        return out;
    }
    Image im;
    if (!Original(header, texels, im)) return 0;
    const Rgba white = MakeRgba(242, 242, 244), ink = MakeRgba(28, 28, 30), screen = MakeRgba(52, 58, 66);
    switch (e->kind) {
    case kButtonA:
        RedrawRoundButton(im, 16.0f, 16.0f, 12.6f, (Symbol)symbolA, 12.0f);
        break;
    case kButtonB:
        Fill(im, MakeRgba(0, 0, 0, 0));
        DrawGlossButton(im, 16.0f, 16.0f, 15.5f, (Symbol)NpControlTable::RoleSymbol(layout, NpControlTable::kRoleB), 12.0f);
        break;
    case kDpad:
        Fill(im, MakeRgba(0, 0, 0, 0));
        DrawDpad(im, 32.0f, 32.0f, 60.0f, 2, white, MakeRgba(120, 120, 124), MakeRgba(232, 186, 52));
        break;
    case kController:
        Fill(im, MakeRgba(0, 0, 0, 0));
        if (e->gx) DrawVita(im, 32.0f, 32.0f, 58.0f, white, ink, screen);   // the big one: thick outline as the original
        else DrawVita(im, 32.0f, 32.0f, 54.0f, white, MakeRgba(60, 60, 64), screen);
        break;
    case kPlusMinus: {
        // the symbol's own colour (its centre), the symbol painted over with the face around it, the shoulder letter drawn
        const Rgba sym = im.At(64, 64);
        InpaintDisc(im, 64.0f, 64.0f, 34.0f);
        const int letter = NpControlTable::RoleSymbol(layout, (NpControlTable::Role)e->gx);
        DrawSymbol(im, (Symbol)letter, 64.0f, 64.0f, 52.0f, MakeRgba(sym.r, sym.g, sym.b));
        break;
    }
    case kCursorHolding: {
        // the hand stays; the remote above its fingers goes; a Vita in the hand
        for (int y = 0; y < 64; ++y)
            for (int x = 14; x < 50; ++x) im.At(x, y) = MakeRgba(0, 0, 0, 0);
        DrawVita(im, 32.0f, 56.0f, 52.0f, white, ink, screen);
        break;
    }
    case kHomeAwayBox: {
        // the box's frame stays; inside, each row takes the colour of its left edge (the panel's own gradient); a faint Vita
        // the panel inside the frame: x 17..238, y 36..227 (the frame and its shadow stay); the edge colours averaged over 7 rows
        const int x0 = 17, x1 = 239, y0 = 36, y1 = 228;
        Rgba left[256], right[256];
        for (int y = y0; y < y1; ++y) {
            int lr = 0, lg = 0, lb = 0, rr = 0, rg = 0, rb = 0, n = 0;
            for (int d = -3; d <= 3; ++d) {
                const int yy = y + d < y0 ? y0 : y + d >= y1 ? y1 - 1 : y + d;
                const Rgba a = im.At(x0 + 1, yy), b = im.At(x1 - 2, yy);
                lr += a.r; lg += a.g; lb += a.b; rr += b.r; rg += b.g; rb += b.b;
                ++n;
            }
            left[y] = MakeRgba(lr / n, lg / n, lb / n);
            right[y] = MakeRgba(rr / n, rg / n, rb / n);
        }
        for (int y = y0; y < y1; ++y) {
            const Rgba a = left[y], b = right[y];
            for (int x = x0; x < x1; ++x) {
                const float k = (float)(x - x0) / (float)(x1 - x0);
                im.At(x, y) = MakeRgba((int)(a.r + (b.r - a.r) * k), (int)(a.g + (b.g - a.g) * k), (int)(a.b + (b.b - a.b) * k));
            }
        }
        for (int i = 0; i < 6; ++i) Disc(im, 128.0f, 132.0f, 90.0f - i * 13.0f, MakeRgba(120, 180, 200, 14));   // the light in the middle
        DrawVita(im, 128.0f, 132.0f, 180.0f, MakeRgba(120, 170, 186, 70), MakeRgba(150, 200, 214, 90), MakeRgba(20, 50, 64, 80));
        break;
    }
    case kControllerMap:
        DrawControllerMap(im, config, kMapAspect);
        break;
    default:
        return 0;
    }
    const size_t texBytes = CmprSize(e->width, e->height, e->levels);
    uint8_t* out = (uint8_t*)malloc(sizeof(TexHeader) + texBytes);
    if (!out) return 0;
    memcpy(out, header, sizeof(TexHeader));
    EncodeCmpr(im, e->levels, out + sizeof(TexHeader));
    if (bytes) *bytes = sizeof(TexHeader) + texBytes;
    return out;
}

}  // namespace NpPromptArt
