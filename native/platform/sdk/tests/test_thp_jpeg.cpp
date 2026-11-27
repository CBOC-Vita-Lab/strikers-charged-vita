// The hardware JPEG path around the console's codec (thp_jpeg.h), on real THP frames of the disc dump (skipped where the files are
// absent): the restuffed stream is a valid JPEG whose scan unstuffs back to the THP bytes, the retiling is the exact inverse of the
// tile layout the software decoder writes (also for a codec pitch wider than the picture), and truncated or damaged frames end in an
// error instead of reading behind the buffer. With MSC_THP_DUMP_DIR the first frames are written as .jpg files together with the
// software decoder's planes (tests/movie/check_bridge.py decodes the .jpg files with libjpeg and compares).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "np/np_sdk.h"
#include "thp_decode.h"
#include "thp_jpeg.h"
#include "thp_testutil.h"

extern "C" {
int THPInit(void);
int THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
}

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static void WriteFile(const char* dir, const char* name, const uint8_t* p, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

static void SoftDecode(const Frame& fr, uint32_t w, uint32_t h, std::vector<uint8_t>* planes, int& r) {
    planes[0].assign((size_t)w * h, 0);
    planes[1].assign((size_t)w / 2 * (h / 2), 0);
    planes[2].assign((size_t)w / 2 * (h / 2), 0);
    r = THPVideoDecode((void*)fr.data.data(), planes[0].data(), planes[1].data(), planes[2].data(), nullptr);
}

static bool TestFile(const char* path, const char* dumpDir, const char* tag) {
    std::vector<Frame> frames;
    uint32_t w, h;
    if (!ReadFrames(path, 24, 9, 160, frames, w, h)) {
        printf("  (%s not present: skipped)\n", path);
        return false;
    }
    printf("  %s: %ux%u, %zu frames checked\n", path, w, h, frames.size());
    size_t stuffedExtra = 0, scanBytes = 0;
    for (const Frame& fr : frames) {
        np_thp::JpegInfo info;
        CHECK(np_thp::ScanFrame(fr.data.data(), fr.size, info) == np_thp::kBridgeOk);
        CHECK(info.width == w && info.height == h);
        CHECK(info.scanEnd + 2 <= fr.size && fr.size - (info.scanEnd + 2) <= 3);   // the component is padded to a multiple of four
        std::vector<uint8_t> std_(np_thp::StuffedSize(info) + 8);
        size_t n = 0;
        CHECK(np_thp::MakeStandardJpeg(fr.data.data(), fr.size, std_.data(), std_.size(), &n) == np_thp::kBridgeOk);
        CHECK(n == np_thp::StuffedSize(info));
        CHECK(std_[n - 2] == 0xFF && std_[n - 1] == 0xD9);
        // stuffing: every 0xFF of the scan is followed by 0x00
        bool stuffedOk = true;
        for (size_t i = info.scanStart; i + 2 < n; ++i)
            if (std_[i] == 0xFF && std_[i + 1] != 0x00) { stuffedOk = false; break; }
        CHECK(stuffedOk);
        // ... and removing it gives the THP bytes back
        std::vector<uint8_t> back(n + 8);
        const size_t bn = np_thp::UnstuffScan(std_.data(), n - 2, info.scanStart, back.data());
        CHECK(bn == info.scanEnd && memcmp(back.data(), fr.data.data(), bn) == 0);
        stuffedExtra += n - (info.scanEnd + 2);
        scanBytes += info.scanEnd - info.scanStart;

        // the retiling is the inverse of the decoder's tile layout, for a pitch wider than the picture
        std::vector<uint8_t> ref[3];
        int r;
        SoftDecode(fr, w, h, ref, r);
        CHECK(r == 0);
        const uint32_t pitch = w + 48;
        std::vector<uint8_t> lin((size_t)pitch * h * 3 / 2, 0xEE);
        uint8_t* y = lin.data();
        uint8_t* cb = y + (size_t)pitch * h;
        uint8_t* cr = cb + (size_t)(pitch / 2) * (h / 2);
        Detile(ref[0].data(), w, h, pitch, y);
        Detile(ref[1].data(), w / 2, h / 2, pitch / 2, cb);
        Detile(ref[2].data(), w / 2, h / 2, pitch / 2, cr);
        std::vector<uint8_t> t[3] = {std::vector<uint8_t>(ref[0].size(), 0), std::vector<uint8_t>(ref[1].size(), 0), std::vector<uint8_t>(ref[2].size(), 0)};
        np_thp::TilePlane(y, pitch, w, h, t[0].data());
        np_thp::TilePlane(cb, pitch / 2, w / 2, h / 2, t[1].data());
        np_thp::TilePlane(cr, pitch / 2, w / 2, h / 2, t[2].data());
        CHECK(t[0] == ref[0] && t[1] == ref[1] && t[2] == ref[2]);

        // a bounded decode of the intact frame is the unbounded decode
        std::vector<uint8_t> b[3] = {std::vector<uint8_t>(ref[0].size(), 0), std::vector<uint8_t>(ref[1].size(), 0), std::vector<uint8_t>(ref[2].size(), 0)};
        {
            np_thp::Tables tables;
            static const uint8_t kNat[80] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
                                             63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};
            static const double kAan[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};
            tables.naturalOrder = kNat;
            for (int i = 0; i < 8; ++i) tables.aan[i] = (double)(float)kAan[i];
            np_thp::Frame f2;
            bool supported = true;
            CHECK(np_thp::DecodeFrame(fr.data.data(), b[0].data(), b[1].data(), b[2].data(), tables, f2, supported, fr.data.data() + fr.size) == 0);
            CHECK(b[0] == ref[0] && b[1] == ref[1] && b[2] == ref[2]);
            // truncated: half of the frame, and only the header
            for (size_t cut : {fr.size / 2, (size_t)info.scanStart + 40, (size_t)200}) {
                std::vector<uint8_t> part(fr.data.begin(), fr.data.begin() + cut);
                part.resize(cut + 64, 0);
                np_thp::JpegInfo pi;
                const int br = np_thp::ScanFrame(part.data(), cut, pi);
                CHECK(br != np_thp::kBridgeOk);   // no end-of-image marker where the component ends
                std::vector<uint8_t> c0(ref[0].size()), c1(ref[1].size()), c2(ref[2].size());
                bool sup = true;
                const int dr = np_thp::DecodeFrame(part.data(), c0.data(), c1.data(), c2.data(), tables, f2, sup, part.data() + cut);
                CHECK(dr != 0 || !sup);   // an error, not a picture of the memory behind the buffer
            }
        }

        if (dumpDir && (fr.index < 3 || fr.index == 153)) {
            char name[128];
            snprintf(name, sizeof name, "%s_%03d.jpg", tag, fr.index);
            WriteFile(dumpDir, name, std_.data(), n);
            snprintf(name, sizeof name, "%s_%03d.y", tag, fr.index);
            std::vector<uint8_t> yl((size_t)w * h), ul((size_t)w / 2 * (h / 2)), vl(ul.size());
            Detile(ref[0].data(), w, h, w, yl.data());
            Detile(ref[1].data(), w / 2, h / 2, w / 2, ul.data());
            Detile(ref[2].data(), w / 2, h / 2, w / 2, vl.data());
            WriteFile(dumpDir, name, yl.data(), yl.size());
            snprintf(name, sizeof name, "%s_%03d.u", tag, fr.index);
            WriteFile(dumpDir, name, ul.data(), ul.size());
            snprintf(name, sizeof name, "%s_%03d.v", tag, fr.index);
            WriteFile(dumpDir, name, vl.data(), vl.size());
        }
    }
    printf("  restuffing adds %.2f%% to the scan\n", scanBytes ? 100.0 * stuffedExtra / scanBytes : 0.0);
    return true;
}

static void TestRefusals() {
    // frames that are not the shape of a THP frame are refused (the player decodes them in software)
    np_thp::JpegInfo info;
    uint8_t junk[64] = {0};
    CHECK(np_thp::ScanFrame(junk, sizeof junk, info) == np_thp::kBridgeNotJpeg);
    CHECK(np_thp::ScanFrame(nullptr, 0, info) == np_thp::kBridgeNotJpeg);
    // SOI, DRI: a restart interval is not handled
    const uint8_t dri[] = {0xFF, 0xD8, 0xFF, 0xDD, 0x00, 0x04, 0x00, 0x10, 0xFF, 0xDA, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(np_thp::ScanFrame(dri, sizeof dri, info) == np_thp::kBridgeUnsupported);
    // progressive
    const uint8_t prog[] = {0xFF, 0xD8, 0xFF, 0xC2, 0x00, 0x11, 8, 1, 0, 2, 0, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1, 0xFF, 0xDA, 0x00, 0x02, 0, 0, 0, 0};
    CHECK(np_thp::ScanFrame(prog, sizeof prog, info) == np_thp::kBridgeUnsupported);
    // the output buffer is too small
    uint8_t out[8];
    size_t n;
    CHECK(np_thp::MakeStandardJpeg(prog, sizeof prog, out, sizeof out, &n) != np_thp::kBridgeOk);
}

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    THPInit();
    TestRefusals();
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir || !*dir) {
        printf("  (MSC_DISC_FILES not set: real-frame checks skipped)\n");
        dir = "/nonexistent";
    }
    const char* dump = getenv("MSC_THP_DUMP_DIR");
    const char* names[] = {"Art/movies/nlglogo.thp", "Art/movies/stad_lavapit_pal.thp", "Art/movies/stad_locked.thp", "Art/movies/introtest_pal.thp"};
    const char* tags[] = {"logo", "lava", "locked", "intro"};
    int ran = 0;
    for (int i = 0; i < 4; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        ran += TestFile(path, dump, tags[i]) ? 1 : 0;
    }
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_thp_jpeg: ok (%d file(s))\n", ran);
    return 0;
}
