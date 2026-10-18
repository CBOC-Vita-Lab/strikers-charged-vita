#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr GLenum kLuminanceAlpha = 0x190A;
GLint unpack = 8;
unsigned uploads = 0;
unsigned pixelStoreCalls = 0, integerQueries = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
[[maybe_unused]] void CapturePixelStore(GLenum name, GLint value) {
    Check(name == GL_UNPACK_ALIGNMENT, "only unpack alignment changes");
    unpack = value;
    ++pixelStoreCalls;
}
[[maybe_unused]] void CaptureGetInteger(GLenum name, GLint* value) {
    Check(name == GL_UNPACK_ALIGNMENT, "only unpack alignment queried");
    *value = unpack;
    ++integerQueries;
}
void CaptureTexImage(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid* data) {
    Check(internal == static_cast<GLint>(kLuminanceAlpha) && format == kLuminanceAlpha, "LA88 upload must use luminance-alpha GL formats");
    Check(type == GL_UNSIGNED_BYTE && w == 3 && h == 5 && level == 0, "LA88 GL dimensions and byte type preserved");
#ifndef __VGX_VITA__
    Check(unpack == 1, "odd-width LA rows uploaded with byte alignment");
#endif
    const auto* p = static_cast<const unsigned char*>(data);
    for (unsigned i = 0; i < 30; ++i) Check(p[i] == i, "GL upload receives exact LA payload");
    ++uploads;
    glTexImage2D(target, level, internal, w, h, border, format, type, data);
}
}

#define glPixelStorei CapturePixelStore
#define glGetIntegerv CaptureGetInteger
#define glTexImage2D CaptureTexImage
#include "../src/vgx_backend_gl.cpp"
#undef glPixelStorei
#undef glGetIntegerv
#undef glTexImage2D

namespace vgx { bool LogEnabled() { return false; } }

int main() {
    vgx::TexUpload up;
    up.texId = 1; up.width = 3; up.height = 5; up.format = vgx::TF_LA88;
    up.levels.emplace_back(30);
    for (unsigned i = 0; i < 30; ++i) up.levels[0][i] = i;
    vgx::backend::Upload(up);
    Check(uploads == 1, "single non-mip upload");
    Check(unpack == 8, "upload restores caller unpack alignment");
#ifdef __VGX_VITA__
    Check(pixelStoreCalls == 0 && integerQueries == 0, "Vita route never uses unsupported unpack-alignment API");
#else
    Check(pixelStoreCalls == 2 && integerQueries == 1, "desktop saves and restores alignment");
#endif
    Check(vgx::backend::GetStats().textureBytes == 30, "backend counts compact payload bytes");
    uint64_t bytes[VGX_STUB_KINDS]; uint32_t counts[VGX_STUB_KINDS];
    vgx_stub_mem(bytes, counts);
    uint64_t total = 0;
    for (int i = 1; i < VGX_STUB_KINDS; ++i) total += bytes[i];
    Check(total == 30, "GL memory census records compact allocation");
    std::puts("PASS: real GL backend LA88 arguments, platform-specific alignment, payload and byte accounting (stub driver)");
}
