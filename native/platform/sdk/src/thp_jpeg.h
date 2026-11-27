#ifndef NP_THP_JPEG_H
#define NP_THP_JPEG_H
// Hardware JPEG path of the THP player: the portable parts (host pointers only, no platform calls).
//
// A THP video frame is a baseline JPEG whose entropy-coded scan has lost its byte stuffing (see
// thp_decode.h): every 0xFF of the scan stands for the standard 0xFF 0x00. A conforming decoder (the
// Vita's JPEG engine, libjpeg) reads a bare 0xFF followed by a byte as a marker, so the stream has to be
// restuffed before it is handed over. The frames carry their own quantisation and Huffman tables (the
// frame header holds up to four DHT segments, or one segment with all tables), so the decoder runs in its
// "with DHT" mode and nothing has to be inserted.
//
// MakeStandardJpeg      THP frame -> standard JPEG byte stream
// TilePlane             linear plane (rows `pitch` apart) -> GX I8 tile layout (8x4 texel tiles, raster order of tiles)
//
// The end of the picture is NOT found by scanning for 0xFF 0xD9 from the front: the unstuffed scan contains
// that pair by chance about twice per frame. The video component of a THP frame is the JPEG followed by zero
// padding to a multiple of four bytes, so the end-of-image marker is the last 0xFF 0xD9 and the caller passes
// the exact component size.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace np_thp {

struct JpegInfo {
    uint32_t width = 0, height = 0;
    uint32_t scanStart = 0;   // offset of the first entropy-coded byte
    uint32_t scanEnd = 0;     // offset of the end-of-image marker (the scan ends before it)
    uint32_t ffBytes = 0;     // 0xFF bytes of the scan (each one grows the stuffed stream by a byte)
};

enum : int {
    kBridgeOk = 0,
    kBridgeNotJpeg = 1,      // no SOI where the frame starts
    kBridgeBadMarker = 2,    // a header marker is damaged or truncated
    kBridgeUnsupported = 3,  // not the baseline 4:2:0 shape of THP, or a restart interval
    kBridgeNoEoi = 4,        // no end-of-image marker at the end of the component
    kBridgeNoSpace = 5,
};

// Reads the header of a THP video frame (`size` = the whole video component) and finds the scan.
inline int ScanFrame(const uint8_t* src, size_t size, JpegInfo& info)
{
    if (!src || size < 4 || src[0] != 0xFF || src[1] != 0xD8) return kBridgeNotJpeg;
    size_t pos = 2;
    bool sof = false, dht = false, dqt = false;
    for (;;) {
        if (pos + 4 > size) return kBridgeBadMarker;
        if (src[pos] != 0xFF) return kBridgeBadMarker;
        while (pos < size && src[pos] == 0xFF) ++pos;
        if (pos >= size) return kBridgeBadMarker;
        const uint8_t marker = src[pos++];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xD9) return kBridgeBadMarker;
        if (pos + 2 > size) return kBridgeBadMarker;
        const size_t len = (static_cast<size_t>(src[pos]) << 8) | src[pos + 1];
        if (len < 2 || len > size - pos) return kBridgeBadMarker;
        if (marker == 0xDD) return kBridgeUnsupported;   // restart interval: not in any THP of the game
        if (marker == 0xC0) {                              // baseline SOF: 8 bit, three components, 4:2:0
            if (len < 17 || src[pos + 2] != 8 || src[pos + 7] != 3) return kBridgeUnsupported;
            info.height = (static_cast<uint32_t>(src[pos + 3]) << 8) | src[pos + 4];
            info.width = (static_cast<uint32_t>(src[pos + 5]) << 8) | src[pos + 6];
            if (src[pos + 9] != 0x22 || src[pos + 12] != 0x11 || src[pos + 15] != 0x11) return kBridgeUnsupported;
            sof = true;
        } else if (marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return kBridgeUnsupported;                     // progressive, lossless, arithmetic
        } else if (marker == 0xC4) {
            dht = true;
        } else if (marker == 0xDB) {
            dqt = true;
        }
        pos += len;
        if (marker == 0xDA) break;
    }
    if (!sof || !dht || !dqt || info.width == 0 || info.height == 0) return kBridgeUnsupported;
    info.scanStart = static_cast<uint32_t>(pos);
    // the end-of-image marker: the last non-zero bytes of the component
    size_t end = size;
    while (end > pos + 2 && src[end - 1] == 0) --end;
    if (end < pos + 2 || src[end - 2] != 0xFF || src[end - 1] != 0xD9) return kBridgeNoEoi;
    info.scanEnd = static_cast<uint32_t>(end - 2);
    uint32_t ff = 0;
    for (size_t i = pos; i < info.scanEnd; ++i) ff += src[i] == 0xFF;
    info.ffBytes = ff;
    return kBridgeOk;
}

// Upper bound of the stuffed stream for a frame of `size` bytes whose scan has `ffBytes` 0xFF bytes.
inline size_t StuffedSize(const JpegInfo& info) { return static_cast<size_t>(info.scanEnd) + info.ffBytes + 2; }

// THP frame -> standard JPEG. dst gets: the header as it is, the scan with 0xFF -> 0xFF 0x00, then 0xFF 0xD9.
inline int MakeStandardJpeg(const uint8_t* src, size_t size, uint8_t* dst, size_t cap, size_t* outSize, JpegInfo* infoOut = nullptr)
{
    JpegInfo info;
    const int r = ScanFrame(src, size, info);
    if (r != kBridgeOk) return r;
    const size_t need = StuffedSize(info);
    if (need > cap) return kBridgeNoSpace;
    memcpy(dst, src, info.scanStart);
    uint8_t* o = dst + info.scanStart;
    const uint8_t* p = src + info.scanStart;
    const uint8_t* const e = src + info.scanEnd;
    while (p < e) {
        // copy the run up to the next 0xFF in one go
        const uint8_t* q = static_cast<const uint8_t*>(memchr(p, 0xFF, static_cast<size_t>(e - p)));
        if (!q) {
            memcpy(o, p, static_cast<size_t>(e - p));
            o += e - p;
            break;
        }
        memcpy(o, p, static_cast<size_t>(q - p));
        o += q - p;
        *o++ = 0xFF;
        *o++ = 0x00;
        p = q + 1;
    }
    *o++ = 0xFF;
    *o++ = 0xD9;
    *outSize = static_cast<size_t>(o - dst);
    if (infoOut) *infoOut = info;
    return kBridgeOk;
}

// Inverse of the stuffing for a standard stream: removes the 0x00 after every 0xFF of the scan (test aid and the
// host-side fake codec). Returns the size written; `scanStart` is where the entropy-coded data begins.
inline size_t UnstuffScan(const uint8_t* src, size_t size, size_t scanStart, uint8_t* dst)
{
    memcpy(dst, src, scanStart);
    size_t o = scanStart;
    for (size_t i = scanStart; i < size; ++i) {
        dst[o++] = src[i];
        if (src[i] == 0xFF && i + 1 < size && src[i + 1] == 0x00) ++i;
    }
    return o;
}

// GX I8 layout: tiles of 8 x 4 texels (32 bytes, row after row), tiles in raster order. `width` a multiple of 8, `height`
// a multiple of 4; rows of the source are `pitch` bytes apart. The source may be uncached or write-combined memory (the
// codec's output buffer on the console, where reading a few bytes at a time is slow): each band of four rows is first
// pulled across in whole rows, then cut into tiles.
inline void TilePlane(const uint8_t* src, uint32_t pitch, uint32_t width, uint32_t height, uint8_t* dst)
{
    const uint32_t tilesX = width / 8, tilesY = height / 4;
    alignas(16) uint8_t band[4 * 1024];
    const bool useBand = pitch <= sizeof(band) / 4;
    for (uint32_t ty = 0; ty < tilesY; ++ty) {
        const uint8_t* rows[4];
        if (useBand) {
            memcpy(band, src + static_cast<size_t>(ty) * 4 * pitch, 4u * pitch > sizeof(band) ? sizeof(band) : 4u * pitch);
            // the four rows are contiguous in the source (pitch apart), so one copy fetched them all
            for (int r = 0; r < 4; ++r) rows[r] = band + static_cast<size_t>(r) * pitch;
        } else {
            for (int r = 0; r < 4; ++r) rows[r] = src + (static_cast<size_t>(ty) * 4 + r) * pitch;
        }
        uint8_t* out = dst + static_cast<size_t>(ty) * tilesX * 32;
        for (uint32_t tx = 0; tx < tilesX; ++tx, out += 32) {
            const uint32_t x = tx * 8;
            memcpy(out, rows[0] + x, 8);
            memcpy(out + 8, rows[1] + x, 8);
            memcpy(out + 16, rows[2] + x, 8);
            memcpy(out + 24, rows[3] + x, 8);
        }
    }
}

// Mean and maximum absolute difference of two equally sized buffers.
inline void PlaneDiff(const uint8_t* a, const uint8_t* b, size_t n, double& mean, unsigned& maxDiff)
{
    uint64_t sum = 0;
    unsigned mx = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        sum += d;
        if (d > mx) mx = d;
    }
    mean = n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    maxDiff = mx;
}

} // namespace np_thp

#endif
