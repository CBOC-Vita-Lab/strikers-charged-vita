// Shared by the THP tests: reading frames of a THP file of the disc dump, undoing the tile layout.
#ifndef NP_THP_TESTUTIL_H
#define NP_THP_TESTUTIL_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static inline void Detile(const uint8_t* tiled, uint32_t w, uint32_t h, uint32_t pitch, uint8_t* dst) {
    const uint32_t tilesX = w / 8;
    for (uint32_t ty = 0; ty < h / 4; ++ty)
        for (uint32_t tx = 0; tx < tilesX; ++tx)
            for (uint32_t r = 0; r < 4; ++r) memcpy(dst + (size_t)(ty * 4 + r) * pitch + tx * 8, tiled + ((size_t)ty * tilesX + tx) * 32 + r * 8, 8);
}

struct Frame {
    std::vector<uint8_t> data;   // the video component plus 64 zero bytes
    size_t size;
    int index;
};

// Reads frames of a THP file: the first `first`, then every `step`th up to `last`.
static inline bool ReadFrames(const char* path, int first, int step, int last, std::vector<Frame>& out, uint32_t& w, uint32_t& h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[48];
    if (fread(hdr, 1, 48, f) != 48 || memcmp(hdr, "THP", 4) != 0) { fclose(f); return false; }
    const uint32_t numFrames = be32(hdr + 20), firstSize = be32(hdr + 24), compOff = be32(hdr + 32), movieOff = be32(hdr + 40);
    fseek(f, compOff, SEEK_SET);
    uint8_t comp[20];
    if (fread(comp, 1, 20, f) != 20) { fclose(f); return false; }
    const uint32_t nc = be32(comp);
    long off = compOff + 20;
    w = h = 0;
    for (uint32_t i = 0; i < nc; ++i) {
        uint8_t info[16];
        fseek(f, off, SEEK_SET);
        if (comp[4 + i] == 0) {
            if (fread(info, 1, 12, f) != 12) { fclose(f); return false; }
            w = be32(info);
            h = be32(info + 4);
            off += 12;
        } else {
            off += 16;
        }
    }
    uint32_t size = firstSize;
    long pos = movieOff;
    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < numFrames && (int)i <= last; ++i) {
        buf.resize(size);
        fseek(f, pos, SEEK_SET);
        if (fread(buf.data(), 1, size, f) != size) break;
        const uint32_t next = be32(buf.data());
        const bool take = (int)i < first || ((int)i % step == 0);
        if (take) {
            uint32_t vsize = 0, offset = 8 + 4 * nc;
            for (uint32_t c = 0; c < nc; ++c) {
                const uint32_t cs = be32(buf.data() + 8 + 4 * c);
                if (comp[4 + c] == 0) {
                    vsize = cs;
                    break;
                }
                offset += cs;
            }
            Frame fr;
            fr.index = (int)i;
            fr.size = vsize;
            fr.data.assign(buf.begin() + offset, buf.begin() + offset + vsize);
            fr.data.resize(vsize + 64, 0);
            out.push_back(fr);
        }
        pos += size;
        size = next;
        if (size == 0 || size > 4 * 1024 * 1024) break;
    }
    fclose(f);
    return !out.empty();
}

#endif
