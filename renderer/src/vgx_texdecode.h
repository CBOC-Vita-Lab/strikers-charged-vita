// GX texture formats -> RGBA8 (front end; pure CPU, no GL).
#pragma once
#include <cstdint>
#include <vector>

namespace vgx {
// Bytes occupied by one mip level of a GX texture (tiles are padded).
uint32_t TexLevelSize(uint32_t width, uint32_t height, uint32_t fmt);
// Total bytes of `levels` mip levels starting at level 0.
uint32_t TexDataSize(uint32_t width, uint32_t height, uint32_t fmt, uint32_t levels);
// the plain loop with the divisions (cpu60-25: reference for MSC_VGX_TEXSIZE_MEMO=0 / 2 and its unit test)
uint32_t TexDataSizeRef(uint32_t width, uint32_t height, uint32_t fmt, uint32_t levels);
bool TexFormatIsCI(uint32_t fmt);
// Decodes one level into width*height RGBA8 pixels (R first), top row first.
// tlut/tlutFmt/tlutEntries are only read for the C4/C8/C14X2 formats.
void DecodeTexture(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, const uint8_t* tlut,
                   uint32_t tlutFmt, uint32_t tlutEntries, uint8_t* dstRGBA);
// I4/I8/IA4/IA8 only: width*height LA88 pairs, sampling as (L,L,L,A).
// I4/I8 replicate intensity into alpha too. Invalid format/size/pointer returns false
// without writing; source and destination must hold a padded GX level and 2*w*h bytes.
bool DecodeTextureLA88(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, uint8_t* dstLA);
// GX I8 to one byte a texel (walk15, MSC_VGX_I8CHURN): the tiles' rows copied out, width a multiple of 8 and height of 4 (false otherwise). The bytes DecodeTextureLA88 writes at every even place.
bool DecodeTextureI8(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst);
// CMPR is S3TC DXT1 in another byte and block order: converts one level (8x8 tiles of four
// big-endian sub-blocks) into ((w + 3) / 4) * ((h + 3) / 4) DXT1 blocks, row-major, as
// GL_COMPRESSED_RGBA_S3TC_DXT1_EXT takes them. Returns false (dst unusable) when the level
// has a three-colour block that uses its fourth entry: on the GameCube that texel is
// transparent with the colour of the block's average, in DXT1 it is transparent black, and
// the difference shows as dark fringes wherever such a texture is filtered. Those textures
// have to be decoded instead (allowTransparent: convert them all the same).
bool CmprToDxt1(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst, bool allowTransparent);
// 1.3 (MSC_VGX_CMPRA): one CMPR level as DXT5, for the levels CmprToDxt1 refuses: 8 bits per texel instead of the 32 of a decoded texture. Alpha is the same
// 0 or 255 per texel. Four-colour blocks keep their colours. A three-colour block's middle colour (also the colour of its transparent texels) is not a DXT5 palette
// entry: its texels alternate between the two inner entries in a checkerboard, whose mean is that colour; each is at most a sixth of c0 to c1 away.
void CmprToDxt5(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst);
// 1.3 (MSC_VGX_CMPRA=2): the largest difference, in 8-bit steps of any colour channel, between CmprToDxt5's texel and the decoded one, over a level: only the middle colour of
// three-colour blocks differs (0 when no block uses it, or the endpoints are close). The alpha is always the same.
uint32_t CmprDxt5MaxError(const uint8_t* src, uint32_t width, uint32_t height);
inline uint32_t Dxt5Size(uint32_t width, uint32_t height) { return ((width + 3) / 4) * ((height + 3) / 4) * 16; }
// 1.3 (MSC_VGX_TEX16): the 16-bit format that holds every texel of these RGBA8 levels without loss, as what a GPU would expand it to: TF_RGB565, TF_RGBA5551, TF_RGBA4444,
// or 0 for none. allow5551 = false leaves that format out. PackRgba16 rewrites one level as little-endian 16-bit words of that format.
uint8_t Rgba16Format(const std::vector<std::vector<uint8_t>>& levels, bool allow5551);
void PackRgba16(std::vector<uint8_t>& level, uint8_t format);
inline uint32_t Dxt1Size(uint32_t width, uint32_t height) { return ((width + 3) / 4) * ((height + 3) / 4) * 8; }
// A plain range-fit DXT1 encoder for opaque RGBA8 (mip levels below the authored ones).
void EncodeDxt1(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* dst);
// 2x2 box filter, for completing mip chains.
void Downsample(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t& dw, uint32_t& dh);
}
