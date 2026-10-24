/* A GL that does nothing: <GLES2/gl2.h> for the "glnull" builds (make glnull, make armgl).
 * The real GL back end (vgx_backend_gl.cpp) is compiled against this header, so everything
 * it does up to the GL call itself runs: state caching, uniform and attribute de-duplication,
 * target pooling, the per-frame count of GL calls by kind. Used for
 *   - the call census without a software rasteriser (no heavy-job lock, whole survey in a minute),
 *   - instruction counts of the back end's own work on the armhf build under qemu.
 * It answers like the target driver where the back end asks something: depth24 but no depth
 * textures (vitaGL's renderbuffer path), S3TC present, every shader compiles and links.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define VGX_GL_STUB 1

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef void GLvoid;
typedef ptrdiff_t GLsizeiptr, GLintptr;

enum {
    GL_FALSE = 0, GL_TRUE = 1, GL_NO_ERROR = 0, GL_ZERO = 0, GL_ONE = 1,
    GL_POINTS = 0, GL_LINES = 1, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5,
    GL_DEPTH_BUFFER_BIT = 0x100, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_NEVER = 0x200, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
    GL_SRC_COLOR = 0x300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
    GL_ONE_MINUS_DST_COLOR,
    GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408, GL_CW = 0x900, GL_CCW = 0x901,
    GL_CULL_FACE = 0xB44, GL_DEPTH_TEST = 0xB71, GL_BLEND = 0xBE2, GL_DITHER = 0xBD0, GL_SCISSOR_TEST = 0xC11,
    GL_UNPACK_ALIGNMENT = 0xCF5, GL_PACK_ALIGNMENT = 0xD05, GL_TEXTURE_2D = 0xDE1,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406,
    GL_DEPTH_COMPONENT = 0x1902, GL_RGBA = 0x1908, GL_LUMINANCE_ALPHA = 0x190A,
    GL_VENDOR = 0x1F00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
    GL_LINEAR_MIPMAP_LINEAR,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_REPEAT = 0x2901,
    GL_FUNC_ADD = 0x8006, GL_FUNC_REVERSE_SUBTRACT = 0x800B, GL_CLAMP_TO_EDGE = 0x812F, GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_DEPTH_COMPONENT24_OES = 0x81A6, GL_MIRRORED_REPEAT = 0x8370, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1, GL_TEXTURE0 = 0x84C0,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_STREAM_DRAW = 0x88E0, GL_STATIC_DRAW = 0x88E4, GL_DYNAMIC_DRAW = 0x88E8,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_ACTIVE_UNIFORMS = 0x8B86,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_FRAMEBUFFER = 0x8D40,
    GL_RENDERBUFFER = 0x8D41,
};

static unsigned vgx_stub_name_ = 0;

/* Memory census: what the back end has asked the driver to hold, by kind, in bytes as vitaGL
 * would allocate them (RGBA8 and depth 4 bytes per pixel, a mip chain = 4/3 of level 0 whether
 * it was uploaded or generated, compressed levels at their size, buffers at their size).
 * Read with vgx_stub_mem(); the back end prints it with MSC_VGX_GLMEM=1 (vgx_backend_gl.cpp). */
enum { VGX_STUB_TEX_RGBA = 1, VGX_STUB_TEX_DXT, VGX_STUB_RT_COLOR, VGX_STUB_RT_DEPTH, VGX_STUB_RB, VGX_STUB_BUF_STATIC, VGX_STUB_BUF_STREAM, VGX_STUB_BUF_DYNAMIC, VGX_STUB_TEX_LA, VGX_STUB_KINDS };
#define VGX_STUB_MAXN (1u << 18)
static uint32_t vgx_stub_bytes_[VGX_STUB_MAXN], vgx_stub_base_[VGX_STUB_MAXN];
static uint8_t vgx_stub_kind_[VGX_STUB_MAXN];
static uint64_t vgx_stub_total_[VGX_STUB_KINDS];
static uint32_t vgx_stub_count_[VGX_STUB_KINDS];
static unsigned vgx_stub_curTex_ = 0, vgx_stub_curRb_ = 0, vgx_stub_curBuf_[2] = {0, 0};
static inline void vgx_stub_set(unsigned name, int kind, uint64_t bytes) {
    if (name == 0 || name >= VGX_STUB_MAXN) return;
    if (vgx_stub_kind_[name]) { vgx_stub_total_[vgx_stub_kind_[name]] -= vgx_stub_bytes_[name]; --vgx_stub_count_[vgx_stub_kind_[name]]; }
    vgx_stub_kind_[name] = (uint8_t)kind;
    vgx_stub_bytes_[name] = (uint32_t)bytes;
    if (kind) { vgx_stub_total_[kind] += bytes; ++vgx_stub_count_[kind]; }
}
static inline void vgx_stub_mem(uint64_t bytes[VGX_STUB_KINDS], uint32_t count[VGX_STUB_KINDS]) {
    for (int i = 0; i < VGX_STUB_KINDS; ++i) { bytes[i] = vgx_stub_total_[i]; count[i] = vgx_stub_count_[i]; }
}
#define VGX_STUB_GEN(fn) static inline void fn(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = ++vgx_stub_name_; }
VGX_STUB_GEN(glGenFramebuffers) VGX_STUB_GEN(glGenTextures) VGX_STUB_GEN(glGenRenderbuffers) VGX_STUB_GEN(glGenBuffers)
#undef VGX_STUB_GEN
static inline GLuint glCreateShader(GLenum) { return ++vgx_stub_name_; }
static inline GLuint glCreateProgram(void) { return ++vgx_stub_name_; }
static inline void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
static inline void glCompileShader(GLuint) {}
static inline void glDeleteShader(GLuint) {}
static inline void glGetShaderiv(GLuint, GLenum, GLint* p) { *p = 1; }
static inline void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar* log) { log[0] = 0; }
static inline void glAttachShader(GLuint, GLuint) {}
static inline void glBindAttribLocation(GLuint, GLuint, const GLchar*) {}
static inline void glDeleteProgram(GLuint) {}
static inline void glLinkProgram(GLuint) {}
static inline void glGetProgramiv(GLuint, GLenum pname, GLint* p) { *p = pname == GL_ACTIVE_UNIFORMS ? 0 : 1; }
static inline void glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar* log) { log[0] = 0; }
static inline void glGetActiveUniform(GLuint, GLuint, GLsizei, GLsizei*, GLint* size, GLenum* type, GLchar* name) { *size = 0; *type = 0; name[0] = 0; }
static inline GLint glGetUniformLocation(GLuint, const GLchar*) { return 1; }
static inline void glUseProgram(GLuint) {}
static inline void glUniform1i(GLint, GLint) {}
static inline void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat) {}
static inline void glUniform4fv(GLint, GLsizei, const GLfloat*) {}
static inline void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*) {}
static inline void glBindTexture(GLenum, GLuint name) { vgx_stub_curTex_ = name; }
static inline void glActiveTexture(GLenum) {}
static inline void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum, const GLvoid* data) {
    const unsigned t = vgx_stub_curTex_;
    if (t == 0 || t >= VGX_STUB_MAXN) return;
    if (level == 0) {
        vgx_stub_base_[t] = (uint32_t)w * (uint32_t)h * (format == GL_LUMINANCE_ALPHA ? 2u : 4u);
        vgx_stub_set(t, format == GL_DEPTH_COMPONENT ? VGX_STUB_RT_DEPTH : data ? (format == GL_LUMINANCE_ALPHA ? VGX_STUB_TEX_LA : VGX_STUB_TEX_RGBA) : VGX_STUB_RT_COLOR, vgx_stub_base_[t]);
    } else {
        vgx_stub_set(t, vgx_stub_kind_[t], (uint64_t)vgx_stub_base_[t] * 4u / 3u);
    }
}
static inline void glCompressedTexImage2D(GLenum, GLint level, GLenum, GLsizei, GLsizei, GLint, GLsizei size, const GLvoid*) {
    const unsigned t = vgx_stub_curTex_;
    if (t == 0 || t >= VGX_STUB_MAXN) return;
    vgx_stub_set(t, VGX_STUB_TEX_DXT, (level == 0 ? 0u : vgx_stub_bytes_[t]) + (uint64_t)size);
}
static inline void glGenerateMipmap(GLenum) {
    const unsigned t = vgx_stub_curTex_;
    if (t && t < VGX_STUB_MAXN && vgx_stub_kind_[t]) vgx_stub_set(t, vgx_stub_kind_[t], (uint64_t)vgx_stub_base_[t] * 4u / 3u);
}
static inline void glTexParameteri(GLenum, GLenum, GLint) {}
static inline void glDeleteTextures(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) vgx_stub_set(ids[i], 0, 0); }
static GLint vgx_stub_unpack_ = 4;
static inline void glPixelStorei(GLenum name, GLint value) { if (name == GL_UNPACK_ALIGNMENT) vgx_stub_unpack_ = value; }
static inline void glGetIntegerv(GLenum name, GLint* value) { *value = name == GL_UNPACK_ALIGNMENT ? vgx_stub_unpack_ : 0; }
static inline void glBindFramebuffer(GLenum, GLuint) {}
static inline void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
static inline void glBindRenderbuffer(GLenum, GLuint name) { vgx_stub_curRb_ = name; }
static inline void glRenderbufferStorage(GLenum, GLenum, GLsizei w, GLsizei h) { vgx_stub_set(vgx_stub_curRb_, VGX_STUB_RB, (uint64_t)w * (uint64_t)h * 4u); }
static inline void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
static inline GLenum glCheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }
static inline void glViewport(GLint, GLint, GLsizei, GLsizei) {}
static inline void glScissor(GLint, GLint, GLsizei, GLsizei) {}
static inline void glDepthRangef(GLfloat, GLfloat) {}
static inline void glEnable(GLenum) {}
static inline void glDisable(GLenum) {}
static inline void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean) {}
static inline void glDepthMask(GLboolean) {}
static inline void glDepthFunc(GLenum) {}
static inline void glBlendEquation(GLenum) {}
static inline void glBlendFunc(GLenum, GLenum) {}
static inline void glCullFace(GLenum) {}
static inline void glFrontFace(GLenum) {}
static inline void glLineWidth(GLfloat) {}
static inline void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
static inline void glClearDepthf(GLfloat) {}
static inline void glClear(GLbitfield) {}
static inline void glBindBuffer(GLenum target, GLuint name) { vgx_stub_curBuf_[target == GL_ELEMENT_ARRAY_BUFFER] = name; }
static inline void glBufferData(GLenum target, GLsizeiptr size, const GLvoid*, GLenum usage) {
    vgx_stub_set(vgx_stub_curBuf_[target == GL_ELEMENT_ARRAY_BUFFER], usage == GL_STREAM_DRAW ? VGX_STUB_BUF_STREAM : usage == GL_DYNAMIC_DRAW ? VGX_STUB_BUF_DYNAMIC : VGX_STUB_BUF_STATIC, (uint64_t)size);
}
static inline void glDeleteBuffers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) vgx_stub_set(ids[i], 0, 0); }
static inline void glEnableVertexAttribArray(GLuint) {}
static inline void glDisableVertexAttribArray(GLuint) {}
static inline void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*) {}
static inline void glVertexAttrib4f(GLuint, GLfloat, GLfloat, GLfloat, GLfloat) {}
static inline void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*) {}
static inline void glDrawArrays(GLenum, GLint, GLsizei) {}
static inline void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*) {}
static inline void glFinish(void) {}
static inline GLenum glGetError(void) { return GL_NO_ERROR; }
static inline const GLubyte* glGetString(GLenum name) {
    return (const GLubyte*)(name == GL_EXTENSIONS ? "GL_OES_depth24 GL_EXT_texture_compression_s3tc" : name == GL_VERSION ? "vgx GL stub (no rasteriser)" : "stub");
}
