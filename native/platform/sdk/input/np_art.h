// np_art.h: the Vita-button pictures (W5 CTL): a small RGBA canvas, anti-aliased shapes, the PlayStation face-button symbols drawn
// from geometry (our own drawings), text in DejaVu Sans Condensed Bold (np_ctl_font.h), and the GameCube texture formats the game's
// front end uses (decode any of them, encode CMPR with mip levels) so a replacement keeps the original's size, format and memory.
// Pure code: no game headers; buffers come from malloc (never the game's arenas).
#ifndef NP_ART_H
#define NP_ART_H

#include <stddef.h>
#include <stdint.h>

namespace NpArt {

struct Rgba {
    uint8_t r, g, b, a;
};
inline Rgba MakeRgba(int r, int g, int b, int a = 255) {
    Rgba c;
    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    c.a = (uint8_t)a;
    return c;
}

class Image {
public:
    Image() {}
    Image(int w, int h);
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    void Reset(int w, int h);   // all transparent black
    int w = 0, h = 0;
    Rgba* px = 0;
    Rgba& At(int x, int y) { return px[y * w + x]; }
    const Rgba& At(int x, int y) const { return px[y * w + x]; }
    bool Ok() const { return px != 0; }
};

// "Source over" with a coverage 0..1 (the colour's own alpha is multiplied in).
void Blend(Image& im, int x, int y, Rgba c, float coverage);
void Fill(Image& im, Rgba c);
void FillRect(Image& im, int x0, int y0, int x1, int y1, Rgba c);   // [x0, x1) x [y0, y1)
// Anti-aliased shapes (4x4 supersampling of an inside test), coordinates in pixels (pixel centres at +0.5).
void Disc(Image& im, float cx, float cy, float r, Rgba c);
void Ring(Image& im, float cx, float cy, float r, float halfWidth, Rgba c);
void Capsule(Image& im, float x0, float y0, float x1, float y1, float radius, Rgba c);
void RoundRect(Image& im, float x0, float y0, float x1, float y1, float radius, Rgba c);
void Triangle(Image& im, float x0, float y0, float x1, float y1, float x2, float y2, Rgba c);
void VGradient(Image& im, int x0, int y0, int x1, int y1, Rgba top, Rgba bottom);

// Text: font 0 = small (labels, 21 px line), 1 = big (46 px). Returns the advance in pixels. An outline of `outlinePx` (0 = none) is
// drawn first in `outline`.
int TextWidth(const char* s, int font);
int TextHeight(int font);
int DrawText(Image& im, int x, int y, const char* s, int font, Rgba c, Rgba outline, int outlinePx);
// Text scaled to fit `maxWidth` (horizontal squeeze only), returns the width used.
int DrawTextFit(Image& im, int x, int y, const char* s, int font, Rgba c, Rgba outline, int outlinePx, int maxWidth);

// The face-button symbols and their PlayStation colours; the shoulder letters; drawn centred at (cx, cy), `size` = the symbol's height.
enum Symbol { kCross, kCircle, kSquare, kTriangle, kLetterL, kLetterR, kStart, kSelect };
Rgba SymbolColor(Symbol s);
void DrawSymbol(Image& im, Symbol s, float cx, float cy, float size, Rgba c);
// A round face button in the style of the game's own button icons: `base` is the original icon (its round button face is kept,
// what was printed on it is wiped, row by row, with the face's own light shades), then the symbol.
void RedrawRoundButton(Image& im, float cx, float cy, float faceRadius, Symbol s, float symbolSize);
// A small round button drawn from nothing (for pictures): a light glossy disc with a dark rim and the symbol.
void DrawRoundButton(Image& im, float cx, float cy, float r, Symbol s);
// The same in the style of the game's A button icon (a dark ring, a light face lit from the top, the lower half a shade darker).
void DrawGlossButton(Image& im, float cx, float cy, float r, Symbol s, float symbolSize);
// Paints a disc over with what surrounds it: each row is interpolated between the colours just left and right of the disc.
void InpaintDisc(Image& im, float cx, float cy, float r);
// The Vita's D-pad (four separate arrow keys), one key (0 up, 1 right, 2 down, 3 left, -1 none) highlighted.
void DrawDpad(Image& im, float cx, float cy, float size, int highlight, Rgba face, Rgba edge, Rgba hi);
// A PS Vita seen from the front (for the "your controller" pictures), `w` wide.
void DrawVita(Image& im, float cx, float cy, float w, Rgba body, Rgba edge, Rgba screen);

// GameCube textures (eGXTextureFormat numbering of the game: 0 RGB565, 1 RGB5A3, 2 CMPR, 3 RGBA8, 4 I8, 5 I4, 6 A8, 7 IA8, 8 CI8;
// texels big-endian as on the disc; the CI8 palette RGB5A3, big-endian).
bool Decode(int format, const uint8_t* texels, int w, int h, const uint8_t* palette, int paletteEntries, Image& out);
// Sizes as the game counts them (GCTextureSize: w * h / 2 per level, levels halving down).
size_t CmprLevelSize(int w, int h);
size_t CmprSize(int w, int h, int levels);
// Encodes `im` (and `levels - 1` box-filtered mips) as CMPR into `out` (CmprSize bytes); alpha below 128 is transparent.
void EncodeCmpr(const Image& im, int levels, uint8_t* out);

}  // namespace NpArt

#endif  // NP_ART_H
