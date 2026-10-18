// np_art.cpp: see np_art.h.
#include "np_art.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "np_ctl_font.h"

namespace NpArt {

Image::Image(int w_, int h_) { Reset(w_, h_); }
Image::~Image() { free(px); }
void Image::Reset(int w_, int h_) {
    free(px);
    px = 0;
    w = h = 0;
    if (w_ <= 0 || h_ <= 0) return;
    px = (Rgba*)calloc((size_t)w_ * (size_t)h_, sizeof(Rgba));
    if (px) {
        w = w_;
        h = h_;
    }
}

void Blend(Image& im, int x, int y, Rgba c, float coverage) {
    if (x < 0 || y < 0 || x >= im.w || y >= im.h || coverage <= 0.0f) return;
    if (coverage > 1.0f) coverage = 1.0f;
    const float a = coverage * (float)c.a / 255.0f;
    Rgba& d = im.At(x, y);
    const float da = (float)d.a / 255.0f;
    const float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) return;
    const float fr = ((float)c.r * a + (float)d.r * da * (1.0f - a)) / oa;
    const float fg = ((float)c.g * a + (float)d.g * da * (1.0f - a)) / oa;
    const float fb = ((float)c.b * a + (float)d.b * da * (1.0f - a)) / oa;
    d.r = (uint8_t)(fr + 0.5f);
    d.g = (uint8_t)(fg + 0.5f);
    d.b = (uint8_t)(fb + 0.5f);
    d.a = (uint8_t)(oa * 255.0f + 0.5f);
}

void Fill(Image& im, Rgba c) {
    for (int i = 0; i < im.w * im.h; ++i) im.px[i] = c;
}

void FillRect(Image& im, int x0, int y0, int x1, int y1, Rgba c) {
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < im.h; ++y)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < im.w; ++x) Blend(im, x, y, c, 1.0f);
}

namespace {

template <class F>
void Shape(Image& im, float bx0, float by0, float bx1, float by1, Rgba c, F inside) {
    int x0 = (int)floorf(bx0) - 1, y0 = (int)floorf(by0) - 1, x1 = (int)ceilf(bx1) + 1, y1 = (int)ceilf(by1) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > im.w) x1 = im.w;
    if (y1 > im.h) y1 = im.h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            int n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    if (inside((float)x + 0.125f + 0.25f * (float)sx, (float)y + 0.125f + 0.25f * (float)sy)) ++n;
            if (n) Blend(im, x, y, c, (float)n / 16.0f);
        }
}

float SegDist2(float px, float py, float x0, float y0, float x1, float y1) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0.0f ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float qx = x0 + t * dx - px, qy = y0 + t * dy - py;
    return qx * qx + qy * qy;
}

}  // namespace

void Disc(Image& im, float cx, float cy, float r, Rgba c) {
    Shape(im, cx - r, cy - r, cx + r, cy + r, c, [&](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; });
}

void Ring(Image& im, float cx, float cy, float r, float hw, Rgba c) {
    Shape(im, cx - r - hw, cy - r - hw, cx + r + hw, cy + r + hw, c, [&](float x, float y) {
        const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        return fabsf(d - r) <= hw;
    });
}

void Capsule(Image& im, float x0, float y0, float x1, float y1, float rad, Rgba c) {
    Shape(im, (x0 < x1 ? x0 : x1) - rad, (y0 < y1 ? y0 : y1) - rad, (x0 > x1 ? x0 : x1) + rad, (y0 > y1 ? y0 : y1) + rad, c,
          [&](float x, float y) { return SegDist2(x, y, x0, y0, x1, y1) <= rad * rad; });
}

void RoundRect(Image& im, float x0, float y0, float x1, float y1, float rad, Rgba c) {
    Shape(im, x0, y0, x1, y1, c, [&](float x, float y) {
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        const float cx = x < x0 + rad ? x0 + rad : x > x1 - rad ? x1 - rad : x;
        const float cy = y < y0 + rad ? y0 + rad : y > y1 - rad ? y1 - rad : y;
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad;
    });
}

void Triangle(Image& im, float x0, float y0, float x1, float y1, float x2, float y2, Rgba c) {
    const float minx = fminf(x0, fminf(x1, x2)), maxx = fmaxf(x0, fmaxf(x1, x2));
    const float miny = fminf(y0, fminf(y1, y2)), maxy = fmaxf(y0, fmaxf(y1, y2));
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    Shape(im, minx, miny, maxx, maxy, c, [&](float x, float y) {
        const float a = (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0);
        const float b = (x2 - x1) * (y - y1) - (x - x1) * (y2 - y1);
        const float d = (x0 - x2) * (y - y2) - (x - x2) * (y0 - y2);
        return area > 0 ? (a >= 0 && b >= 0 && d >= 0) : (a <= 0 && b <= 0 && d <= 0);
    });
}

void VGradient(Image& im, int x0, int y0, int x1, int y1, Rgba t, Rgba b) {
    for (int y = y0; y < y1; ++y) {
        const float k = y1 - 1 > y0 ? (float)(y - y0) / (float)(y1 - 1 - y0) : 0.0f;
        const Rgba c = MakeRgba((int)(t.r + (b.r - t.r) * k), (int)(t.g + (b.g - t.g) * k), (int)(t.b + (b.b - t.b) * k),
                                (int)(t.a + (b.a - t.a) * k));
        for (int x = x0; x < x1; ++x) Blend(im, x, y, c, 1.0f);
    }
}

// ---- text ----------------------------------------------------------------------------------------------------------------
namespace {
struct FontRef {
    const NpCtlGlyph* glyphs;
    const uint8_t* data;
    int height;
};
FontRef GetFont(int font) {
    FontRef f;
    if (font == 1) {
        f.glyphs = kNpCtlFontBigGlyphs;
        f.data = kNpCtlFontBigData;
        f.height = kNpCtlFontBigHeight;
    } else {
        f.glyphs = kNpCtlFontSmallGlyphs;
        f.data = kNpCtlFontSmallData;
        f.height = kNpCtlFontSmallHeight;
    }
    return f;
}
int Coverage(const FontRef& f, const NpCtlGlyph& g, int x, int y) {
    if (x < 0 || y < 0 || x >= g.width || y >= f.height) return 0;
    const int rowBytes = (g.width + 1) / 2;
    const uint8_t v = f.data[g.offset + y * rowBytes + x / 2];
    return (x & 1) ? (v & 15) : (v >> 4);
}
int Glyph(const char ch) {
    unsigned c = (unsigned char)ch;
    return (c < 32 || c > 126) ? '?' - 32 : (int)c - 32;
}
// draws one string with a horizontal scale (1 = natural)
int DrawScaled(Image& im, int x, int y, const char* s, int font, Rgba c, float sx) {
    const FontRef f = GetFont(font);
    float pen = (float)x;
    for (; *s; ++s) {
        const NpCtlGlyph& g = f.glyphs[Glyph(*s)];
        const int w = (int)ceilf((float)g.width * sx);
        for (int gy = 0; gy < f.height; ++gy)
            for (int gx = 0; gx < w; ++gx) {
                // box-sample the source columns this destination pixel covers
                const float s0 = (float)gx / sx, s1 = (float)(gx + 1) / sx;
                float acc = 0.0f, wsum = 0.0f;
                for (float u = s0; u < s1 - 1e-4f;) {
                    const float next = fminf(s1, floorf(u) + 1.0f);
                    const float wgt = next - u;
                    acc += (float)Coverage(f, g, (int)u, gy) * wgt;
                    wsum += wgt;
                    u = next;
                }
                if (wsum > 0.0f && acc > 0.0f) Blend(im, (int)pen + gx, y + gy, c, acc / wsum / 15.0f);
            }
        pen += (float)g.advance * sx;
    }
    return (int)(pen - (float)x + 0.5f);
}
}  // namespace

int TextWidth(const char* s, int font) {
    const FontRef f = GetFont(font);
    int w = 0;
    for (; *s; ++s) w += f.glyphs[Glyph(*s)].advance;
    return w;
}
int TextHeight(int font) { return GetFont(font).height; }

int DrawTextFit(Image& im, int x, int y, const char* s, int font, Rgba c, Rgba outline, int outlinePx, int maxWidth) {
    const int natural = TextWidth(s, font);
    const float sx = (maxWidth > 0 && natural > maxWidth) ? (float)maxWidth / (float)natural : 1.0f;
    if (outlinePx > 0)
        for (int oy = -outlinePx; oy <= outlinePx; ++oy)
            for (int ox = -outlinePx; ox <= outlinePx; ++ox)
                if (ox || oy) DrawScaled(im, x + ox, y + oy, s, font, outline, sx);
    return DrawScaled(im, x, y, s, font, c, sx);
}
int DrawText(Image& im, int x, int y, const char* s, int font, Rgba c, Rgba outline, int outlinePx) {
    return DrawTextFit(im, x, y, s, font, c, outline, outlinePx, 0);
}

// ---- symbols -------------------------------------------------------------------------------------------------------------
Rgba SymbolColor(Symbol s) {
    switch (s) {
    case kCross: return MakeRgba(64, 128, 224);
    case kCircle: return MakeRgba(226, 60, 70);
    case kSquare: return MakeRgba(214, 96, 182);
    case kTriangle: return MakeRgba(36, 172, 128);
    default: return MakeRgba(56, 56, 60);
    }
}

void DrawSymbol(Image& im, Symbol s, float cx, float cy, float size, Rgba c) {
    const float half = size * 0.5f;
    const float stroke = size * 0.11f + 0.35f;   // half width of the strokes
    switch (s) {
    case kCross:
        Capsule(im, cx - half * 0.82f, cy - half * 0.82f, cx + half * 0.82f, cy + half * 0.82f, stroke, c);
        Capsule(im, cx - half * 0.82f, cy + half * 0.82f, cx + half * 0.82f, cy - half * 0.82f, stroke, c);
        break;
    case kCircle: Ring(im, cx, cy, half * 0.84f, stroke, c); break;
    case kSquare: {
        const float a = half * 0.78f;
        Capsule(im, cx - a, cy - a, cx + a, cy - a, stroke, c);
        Capsule(im, cx + a, cy - a, cx + a, cy + a, stroke, c);
        Capsule(im, cx + a, cy + a, cx - a, cy + a, stroke, c);
        Capsule(im, cx - a, cy + a, cx - a, cy - a, stroke, c);
        break;
    }
    case kTriangle: {
        const float r = half * 0.92f;
        const float x0 = cx, y0 = cy - r * 0.95f, x1 = cx + r * 0.95f, y1 = cy + r * 0.62f, x2 = cx - r * 0.95f, y2 = cy + r * 0.62f;
        Capsule(im, x0, y0, x1, y1, stroke, c);
        Capsule(im, x1, y1, x2, y2, stroke, c);
        Capsule(im, x2, y2, x0, y0, stroke, c);
        break;
    }
    case kLetterL: {
        const float hw = size * 0.13f, top = cy - half * 0.8f, bot = cy + half * 0.8f, left = cx - half * 0.42f, right = cx + half * 0.46f;
        Capsule(im, left, top, left, bot, hw, c);
        Capsule(im, left, bot, right, bot, hw, c);
        break;
    }
    case kLetterR: {
        const float hw = size * 0.13f, top = cy - half * 0.8f, bot = cy + half * 0.8f, left = cx - half * 0.46f, mid = cy - half * 0.02f;
        const float bowlR = (mid - top) * 0.5f, bowlX = cx + half * 0.12f;
        Capsule(im, left, top, left, bot, hw, c);
        Capsule(im, left, top, bowlX, top, hw, c);
        Capsule(im, left, mid, bowlX, mid, hw, c);
        Shape(im, bowlX - hw, top - hw, bowlX + bowlR + hw, mid + hw, c, [&](float x, float y) {
            if (x < bowlX) return false;
            const float d = sqrtf((x - bowlX) * (x - bowlX) + (y - (top + bowlR)) * (y - (top + bowlR)));
            return fabsf(d - bowlR) <= hw;
        });
        Capsule(im, cx - half * 0.02f, mid, cx + half * 0.5f, bot, hw, c);
        break;
    }
    case kStart:
        Triangle(im, cx - half * 0.45f, cy - half * 0.6f, cx + half * 0.6f, cy, cx - half * 0.45f, cy + half * 0.6f, c);
        break;
    case kSelect:
        for (int i = -1; i <= 1; ++i) Capsule(im, cx - half * 0.6f, cy + i * half * 0.5f, cx + half * 0.6f, cy + i * half * 0.5f, stroke * 0.8f, c);
        break;
    }
}

void RedrawRoundButton(Image& im, float cx, float cy, float faceR, Symbol s, float symbolSize) {
    // What was printed on the face (a dark letter) is painted over with the face's own shade of each row: the median of the row's light
    // pixels, smoothed over three rows (the gloss of the original stays: light at the top, grey below). Only the middle of the face
    // (where the letter is) is painted, with a soft edge, so the rim and the outer face stay the original's pixels.
    const int y0 = (int)floorf(cy - faceR), y1 = (int)ceilf(cy + faceR);
    const int rows = y1 - y0 + 1;
    if (rows <= 0 || rows > 1024) return;
    Rgba fill[1024];
    bool have[1024];
    for (int k = 0; k < rows; ++k) {
        have[k] = false;
        const int y = y0 + k;
        if (y < 0 || y >= im.h) continue;
        Rgba row[256];
        int lum[256], n = 0;
        for (int x = (int)(cx - faceR); x <= (int)(cx + faceR) && n < 256; ++x) {
            if (x < 0 || x >= im.w) continue;
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            if (dx * dx + dy * dy > faceR * faceR) continue;
            const Rgba& p = im.At(x, y);
            const int l = (p.r * 3 + p.g * 6 + p.b) / 10;
            if (p.a < 200 || l < 150) continue;
            row[n] = p;
            lum[n] = l;
            ++n;
        }
        if (!n) continue;
        // the median by brightness
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && lum[j - 1] > lum[j]; --j) {
                const int tl = lum[j]; lum[j] = lum[j - 1]; lum[j - 1] = tl;
                const Rgba tc = row[j]; row[j] = row[j - 1]; row[j - 1] = tc;
            }
        fill[k] = row[n / 2];
        have[k] = true;
    }
    Rgba smooth[1024];
    for (int k = 0; k < rows; ++k) {
        int r = 0, g = 0, b = 0, n = 0;
        for (int d = -1; d <= 1; ++d)
            if (k + d >= 0 && k + d < rows && have[k + d]) {
                r += fill[k + d].r;
                g += fill[k + d].g;
                b += fill[k + d].b;
                ++n;
            }
        smooth[k] = n ? MakeRgba(r / n, g / n, b / n) : MakeRgba(0, 0, 0, 0);
    }
    const float wipeR = faceR * 0.82f, soft = faceR * 0.12f;
    for (int k = 0; k < rows; ++k) {
        if (!smooth[k].a) continue;
        const int y = y0 + k;
        if (y < 0 || y >= im.h) continue;
        for (int x = (int)(cx - faceR - 1); x <= (int)(cx + faceR + 1); ++x) {
            if (x < 0 || x >= im.w) continue;
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy);
            float cov = d <= wipeR ? 1.0f : d >= wipeR + soft ? 0.0f : 1.0f - (d - wipeR) / soft;
            if (cov > 0.0f) Blend(im, x, y, smooth[k], cov);
        }
    }
    DrawSymbol(im, s, cx, cy, symbolSize, SymbolColor(s));
}

void DrawRoundButton(Image& im, float cx, float cy, float r, Symbol s) {
    Disc(im, cx, cy + r * 0.06f, r, MakeRgba(30, 30, 34, 200));   // shadow
    Disc(im, cx, cy, r, MakeRgba(70, 72, 78));
    // the face: light, a little gloss towards the top
    for (int y = (int)(cy - r); y <= (int)(cy + r); ++y) {
        const float k = (y - (cy - r)) / (2.0f * r);
        const Rgba face = MakeRgba((int)(238 - 40 * k), (int)(238 - 40 * k), (int)(242 - 38 * k));
        for (int x = (int)(cx - r); x <= (int)(cx + r); ++x) {
            int n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = (float)x + 0.125f + 0.25f * sx - cx, dy = (float)y + 0.125f + 0.25f * sy - cy;
                    if (dx * dx + dy * dy <= (r * 0.86f) * (r * 0.86f)) ++n;
                }
            if (n) Blend(im, x, y, face, (float)n / 16.0f);
        }
    }
    DrawSymbol(im, s, cx, cy, r * 1.0f, SymbolColor(s));
}

void DrawGlossButton(Image& im, float cx, float cy, float r, Symbol s, float symbolSize) {
    Disc(im, cx, cy, r, MakeRgba(34, 34, 38));
    Disc(im, cx, cy, r * 0.86f, MakeRgba(120, 120, 126));
    const float fr = r * 0.80f;
    Shape(im, cx - fr, cy - fr, cx + fr, cy + fr, MakeRgba(238, 238, 240), [&](float x, float y) {
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= fr * fr && y < cy - fr * 0.05f;
    });
    Shape(im, cx - fr, cy - fr, cx + fr, cy + fr, MakeRgba(200, 200, 204), [&](float x, float y) {
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= fr * fr && y >= cy - fr * 0.05f;
    });
    DrawSymbol(im, s, cx, cy, symbolSize, SymbolColor(s));
}

void InpaintDisc(Image& im, float cx, float cy, float r) {
    for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); ++y) {
        if (y < 0 || y >= im.h) continue;
        const float dy = (float)y + 0.5f - cy;
        if (dy * dy > r * r) continue;
        const float half = sqrtf(r * r - dy * dy);
        int xa = (int)floorf(cx - half) - 1, xb = (int)ceilf(cx + half);
        if (xa < 0) xa = 0;
        if (xb >= im.w) xb = im.w - 1;
        if (xb <= xa + 1) continue;
        const Rgba a = im.At(xa, y), b = im.At(xb, y);
        for (int x = xa + 1; x < xb; ++x) {
            const float k = (float)(x - xa) / (float)(xb - xa);
            im.At(x, y) = MakeRgba((int)(a.r + (b.r - a.r) * k), (int)(a.g + (b.g - a.g) * k), (int)(a.b + (b.b - a.b) * k),
                                   (int)(a.a + (b.a - a.a) * k));
        }
    }
}

namespace {
// a convex polygon (clockwise or not), anti-aliased in one pass (no seams between the triangles of a fan)
void ConvexPolygon(Image& im, const float* xs, const float* ys, int n, Rgba c) {
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0], area = 0.0f;
    for (int i = 0; i < n; ++i) {
        x0 = fminf(x0, xs[i]); x1 = fmaxf(x1, xs[i]); y0 = fminf(y0, ys[i]); y1 = fmaxf(y1, ys[i]);
        const int j = (i + 1) % n;
        area += xs[i] * ys[j] - xs[j] * ys[i];
    }
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    Shape(im, x0, y0, x1, y1, c, [&](float x, float y) {
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            if (sign * ((xs[j] - xs[i]) * (y - ys[i]) - (x - xs[i]) * (ys[j] - ys[i])) < 0.0f) return false;
        }
        return true;
    });
}
}  // namespace

void DrawDpad(Image& im, float cx, float cy, float size, int highlight, Rgba face, Rgba edge, Rgba hi) {
    // four separate keys, each a pentagon pointing at the centre (as on the Vita), an outline first
    const float arm = size * 0.5f, w = size * 0.17f, gap = size * 0.06f;
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < 4; ++k) {
            const float dx = k == 1 ? 1.0f : k == 3 ? -1.0f : 0.0f, dy = k == 0 ? -1.0f : k == 2 ? 1.0f : 0.0f;
            const float px = -dy, py = dx;   // perpendicular
            const float e = pass == 0 ? size * 0.035f : 0.0f;
            const float tip = gap - e, base = gap + w, end = arm + e, half = w + e;
            const float xs[5] = {cx + dx * tip, cx + dx * base + px * half, cx + dx * end + px * half, cx + dx * end - px * half,
                                 cx + dx * base - px * half};
            const float ys[5] = {cy + dy * tip, cy + dy * base + py * half, cy + dy * end + py * half, cy + dy * end - py * half,
                                 cy + dy * base - py * half};
            ConvexPolygon(im, xs, ys, 5, pass == 0 ? edge : (k == highlight ? hi : face));
        }
}

void DrawVita(Image& im, float cx, float cy, float w, Rgba body, Rgba edge, Rgba screen) {
    const float h = w * 0.44f, x0 = cx - w * 0.5f, y0 = cy - h * 0.5f, x1 = cx + w * 0.5f, y1 = cy + h * 0.5f;
    RoundRect(im, x0 - w * 0.012f, y0 - w * 0.012f, x1 + w * 0.012f, y1 + w * 0.012f, h * 0.5f, edge);
    RoundRect(im, x0, y0, x1, y1, h * 0.48f, body);
    RoundRect(im, cx - w * 0.27f, y0 + h * 0.1f, cx + w * 0.27f, y1 - h * 0.1f, w * 0.01f, screen);
    // the sticks, the D-pad, the face buttons
    Disc(im, x0 + w * 0.12f, y0 + h * 0.70f, w * 0.032f, edge);
    Disc(im, x1 - w * 0.12f, y0 + h * 0.70f, w * 0.032f, edge);
    DrawDpad(im, x0 + w * 0.12f, y0 + h * 0.36f, w * 0.12f, -1, edge, edge, edge);
    const float bx = x1 - w * 0.12f, by = y0 + h * 0.36f, d = w * 0.035f, br = w * 0.017f;
    Disc(im, bx, by - d, br, edge);
    Disc(im, bx + d, by, br, edge);
    Disc(im, bx, by + d, br, edge);
    Disc(im, bx - d, by, br, edge);
}

// ---- GameCube textures ---------------------------------------------------------------------------------------------------
namespace {
Rgba From565(unsigned v) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return MakeRgba(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2);
}
Rgba From5A3(unsigned v) {
    if (v & 0x8000) {
        const int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return MakeRgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2);
    }
    const int a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return MakeRgba(r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1);
}
unsigned Be16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }
void Put(Image& im, int x, int y, Rgba c) {
    if (x < im.w && y < im.h) im.At(x, y) = c;
}
unsigned To565(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (unsigned)((r * 31 + 127) / 255) << 11 | (unsigned)((g * 63 + 127) / 255) << 5 | (unsigned)((b * 31 + 127) / 255);
}
}  // namespace

bool Decode(int fmt, const uint8_t* d, int w, int h, const uint8_t* pal, int palN, Image& out) {
    out.Reset(w, h);
    if (!out.Ok()) return false;
    size_t o = 0;
    switch (fmt) {
    case 0: case 1: case 7:
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4)
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x, o += 2) {
                        const unsigned v = Be16(d + o);
                        Put(out, tx + x, ty + y, fmt == 0 ? From565(v) : fmt == 1 ? From5A3(v) : MakeRgba(v & 255, v & 255, v & 255, v >> 8));
                    }
        return true;
    case 3:
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4, o += 64)
                for (int i = 0; i < 16; ++i)
                    Put(out, tx + i % 4, ty + i / 4, MakeRgba(d[o + 2 * i + 1], d[o + 32 + 2 * i], d[o + 32 + 2 * i + 1], d[o + 2 * i]));
        return true;
    case 4: case 6: case 8:
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 8)
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 8; ++x, ++o) {
                        const int v = d[o];
                        Rgba c = fmt == 4 ? MakeRgba(v, v, v) : fmt == 6 ? MakeRgba(255, 255, 255, v) : MakeRgba(255, 0, 255);
                        if (fmt == 8 && pal && v < palN) c = From5A3(Be16(pal + 2 * v));
                        Put(out, tx + x, ty + y, c);
                    }
        return true;
    case 5:
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; x += 2, ++o) {
                        const int a = d[o] >> 4, b = d[o] & 15;
                        Put(out, tx + x, ty + y, MakeRgba(a * 17, a * 17, a * 17));
                        Put(out, tx + x + 1, ty + y, MakeRgba(b * 17, b * 17, b * 17));
                    }
        return true;
    case 2:
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int sb = 0; sb < 4; ++sb, o += 8) {
                    const int sx = (sb & 1) * 4, sy = (sb >> 1) * 4;
                    const unsigned c0 = Be16(d + o), c1 = Be16(d + o + 2);
                    const Rgba a = From565(c0), b = From565(c1);
                    Rgba cs[4] = {a, b, a, a};
                    if (c0 > c1) {
                        cs[2] = MakeRgba((2 * a.r + b.r) / 3, (2 * a.g + b.g) / 3, (2 * a.b + b.b) / 3);
                        cs[3] = MakeRgba((a.r + 2 * b.r) / 3, (a.g + 2 * b.g) / 3, (a.b + 2 * b.b) / 3);
                    } else {
                        cs[2] = MakeRgba((a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2);
                        cs[3] = MakeRgba(0, 0, 0, 0);
                    }
                    for (int i = 0; i < 16; ++i) {
                        const int idx = (d[o + 4 + i / 4] >> (6 - 2 * (i % 4))) & 3;
                        Put(out, tx + sx + i % 4, ty + sy + i / 4, cs[idx]);
                    }
                }
        return true;
    default:
        return false;
    }
}

size_t CmprLevelSize(int w, int h) {
    // as the game counts it (gcSwizzler.cpp GCTextureSize: half a byte per texel, no padding to whole 8 x 8 tiles)
    return (size_t)w * (size_t)h / 2;
}
size_t CmprSize(int w, int h, int levels) {
    size_t n = 0;
    for (int l = 0; l < levels; ++l) {
        n += CmprLevelSize(w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return n;
}

namespace {
void EncodeBlock(const Rgba* p, uint8_t* out) {
    // endpoints: the two opaque colours farthest apart; any transparent pixel selects the 3-colour mode with index 3 = transparent
    int opaque[16], n = 0;
    bool transparent = false;
    for (int i = 0; i < 16; ++i) {
        if (p[i].a < 128) transparent = true;
        else opaque[n++] = i;
    }
    unsigned c0 = 0, c1 = 0;
    if (n > 0) {
        int bi = opaque[0], bj = opaque[0], best = -1;
        for (int a = 0; a < n; ++a)
            for (int b = a; b < n; ++b) {
                const Rgba& x = p[opaque[a]];
                const Rgba& y = p[opaque[b]];
                const int d = (x.r - y.r) * (x.r - y.r) + (x.g - y.g) * (x.g - y.g) + (x.b - y.b) * (x.b - y.b);
                if (d > best) {
                    best = d;
                    bi = opaque[a];
                    bj = opaque[b];
                }
            }
        c0 = To565(p[bi].r, p[bi].g, p[bi].b);
        c1 = To565(p[bj].r, p[bj].g, p[bj].b);
    }
    if (transparent) {
        if (c0 > c1) { const unsigned t = c0; c0 = c1; c1 = t; }
    } else {
        if (c0 < c1) { const unsigned t = c0; c0 = c1; c1 = t; }
        if (c0 == c1) {
            // one colour: the 4-colour mode needs c0 > c1
            if (c1 > 0) --c1;
            else ++c0;
        }
    }
    const Rgba a = From565(c0), b = From565(c1);
    Rgba cs[4] = {a, b, a, a};
    int count = 4;
    if (c0 > c1) {
        cs[2] = MakeRgba((2 * a.r + b.r) / 3, (2 * a.g + b.g) / 3, (2 * a.b + b.b) / 3);
        cs[3] = MakeRgba((a.r + 2 * b.r) / 3, (a.g + 2 * b.g) / 3, (a.b + 2 * b.b) / 3);
    } else {
        cs[2] = MakeRgba((a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2);
        count = 3;
    }
    out[0] = (uint8_t)(c0 >> 8);
    out[1] = (uint8_t)c0;
    out[2] = (uint8_t)(c1 >> 8);
    out[3] = (uint8_t)c1;
    for (int row = 0; row < 4; ++row) {
        uint8_t bits = 0;
        for (int col = 0; col < 4; ++col) {
            const Rgba& q = p[row * 4 + col];
            int idx = 3;
            if (!(transparent && q.a < 128)) {
                int bestD = 1 << 30;
                for (int k = 0; k < count; ++k) {
                    const int d = (q.r - cs[k].r) * (q.r - cs[k].r) + (q.g - cs[k].g) * (q.g - cs[k].g) + (q.b - cs[k].b) * (q.b - cs[k].b);
                    if (d < bestD) {
                        bestD = d;
                        idx = k;
                    }
                }
            }
            bits |= (uint8_t)(idx << (6 - 2 * col));
        }
        out[4 + row] = bits;
    }
}
void EncodeLevel(const Image& im, uint8_t* out) {
    size_t o = 0;
    const int pw = (im.w + 7) / 8 * 8, ph = (im.h + 7) / 8 * 8;
    for (int ty = 0; ty < ph; ty += 8) {
        for (int tx = 0; tx < pw; tx += 8)
            for (int sb = 0; sb < 4; ++sb, o += 8) {
                Rgba blk[16];
                for (int i = 0; i < 16; ++i) {
                    const int x = tx + (sb & 1) * 4 + i % 4, y = ty + (sb >> 1) * 4 + i / 4;
                    blk[i] = (x < im.w && y < im.h) ? im.At(x, y) : MakeRgba(0, 0, 0, 0);
                }
                EncodeBlock(blk, out + o);
            }
    }
}
}  // namespace

void EncodeCmpr(const Image& src, int levels, uint8_t* out) {
    Image cur(src.w, src.h);
    if (!cur.Ok()) return;
    memcpy(cur.px, src.px, sizeof(Rgba) * (size_t)src.w * (size_t)src.h);
    for (int l = 0; l < levels; ++l) {
        const size_t size = CmprLevelSize(cur.w, cur.h);
        if (cur.w % 8 == 0 && cur.h % 8 == 0) {
            EncodeLevel(cur, out);
        } else {
            // a level smaller than one tile: encode the padded tile, keep what the game's size has room for
            const size_t padded = (size_t)((cur.w + 7) / 8 * 8) * (size_t)((cur.h + 7) / 8 * 8) / 2;
            uint8_t* tmp = (uint8_t*)malloc(padded);
            if (!tmp) return;
            EncodeLevel(cur, tmp);
            memcpy(out, tmp, size);
            free(tmp);
        }
        out += size;
        if (l + 1 == levels) break;
        const int nw = cur.w > 1 ? cur.w / 2 : 1, nh = cur.h > 1 ? cur.h / 2 : 1;
        Image next(nw, nh);
        if (!next.Ok()) return;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                int r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int k = 0; k < 4; ++k) {
                    const int sx = x * 2 + (k & 1), sy = y * 2 + (k >> 1);
                    if (sx >= cur.w || sy >= cur.h) continue;
                    const Rgba& p = cur.At(sx, sy);
                    r += p.r * p.a;
                    g += p.g * p.a;
                    b += p.b * p.a;
                    a += p.a;
                    ++n;
                }
                next.At(x, y) = a ? MakeRgba(r / a, g / a, b / a, a / n) : MakeRgba(0, 0, 0, 0);
            }
        cur.Reset(nw, nh);
        memcpy(cur.px, next.px, sizeof(Rgba) * (size_t)nw * (size_t)nh);
    }
}

}  // namespace NpArt
