// The player's own game art for our screens (the Settings screen), read from the disc files on the memory card at the moment the screen opens.
// Nothing of it is in the package or the repository: the picture is the start screen's, in the game's own front-end bundle
// (files/Art/fe/mainui.dmn, a 512 x 512 CMPR texture), and the lettering is the game's own heading font (files/Art/fe/fonts/engfontheading36.res:
// a glyph description and a CI8 glyph page). Without the files, Load returns false and the screens keep their plain look.
//
// Portable C++ on 0xAABBGGRR pixels (stdio only), so the host test (tests/test_brand_art.cpp) can draw the same screens.
#pragma once

#include <cstdint>
#include <vector>

namespace NpBrandArt {

constexpr int kWidth = 960, kHeight = 544;

class Art {
public:
    // gameRoot: the folder that holds sys/ and files/ (NpGameRootPath()). False when the picture is not there; the font is optional.
    bool Load(const char* gameRoot);
    void Release();

    // The picture covering the screen, kWidth x kHeight, already dimmed so that text on it reads. Null before Load / after Release.
    const uint32_t* Backdrop() const { return backdrop_.empty() ? nullptr : backdrop_.data(); }
    bool HasFont() const { return !glyphA_.empty(); }

    // Text in the game's heading font in the logo's colours (flame fill, dark outline, soft shadow); `height` is the size of a capital
    // letter's line (the font's 57 units). Returns the width drawn; draw = false only measures. Characters the font lacks are skipped.
    int Heading(uint32_t* pixels, int x, int y, int height, const char* text, bool draw = true) const;

private:
    struct Glyph {
        int16_t x, y, width, advance, offset;
        bool present;
    };
    bool LoadPicture(const char* mainui);
    bool LoadFont(const char* fontRes);
    void ReleaseFont();

    std::vector<uint32_t> backdrop_;
    std::vector<uint8_t> glyphA_, glyphL_;   // the font page: alpha, and luminance (black fill 0 .. white outline 249)
    Glyph glyphs_[128] = {};
    int pageSize_ = 512, renderHeight_ = 75, lineHeight_ = 57, ascent_ = 46, renderAscent_ = 50;
};

}  // namespace NpBrandArt
