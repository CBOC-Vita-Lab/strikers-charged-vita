// The little of vitaGL's shared.h that its GLSL translator (utils/glsl_utils.c, utils/preprocessor) uses, so that the real translator runs on the host: run_shader_forms_test.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
typedef unsigned char GLboolean; typedef unsigned GLenum; typedef int GLsizei; typedef char GLchar; typedef int GLint; typedef unsigned GLuint;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define MAX_CG_TEXCOORD_ID 10
#define MAX_CG_COLOR_ID 2
typedef enum {VGL_TYPE_NONE,VGL_TYPE_TEXCOORD,VGL_TYPE_TEXCOORD_CENTROID,VGL_TYPE_COLOR,VGL_TYPE_COLOR_CENTROID,VGL_TYPE_FOG,VGL_TYPE_FOG_CENTROID,VGL_TYPE_CLIP} vglSemanticType;
enum {VGL_MODE_SHADER_PAIR,VGL_MODE_GLOBAL,VGL_MODE_POSTPONED};
typedef struct { GLboolean texcoord_used[MAX_CG_TEXCOORD_ID]; GLboolean color_used[MAX_CG_COLOR_ID]; char texcoord_names[MAX_CG_TEXCOORD_ID][64]; char color_names[MAX_CG_COLOR_ID][64]; } binds_map;
typedef struct { GLenum type; char *source; uint32_t size; GLboolean is_glsl; binds_map semantics; void* prog; uint8_t sized_samplers_num; } shader;
#define vgl_fast_memcpy memcpy
#define vgl_memset memset
#define vglMalloc malloc
#define vgl_malloc(a,b) malloc(a)
#define vgl_free free
#define vgl_realloc realloc
#define vglRealloc realloc
#define vgl_log printf
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
void vglAddSemanticBinding(const GLchar *const *varying, GLint index, GLenum type);
#define sceClibMemmove memmove
