/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _VITAGL_H_
#define _VITAGL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <vitashark.h>

#ifndef HAVE_GL_HEADERS
// clang-format off
#define GLbitfield    uint32_t
#define GLboolean     uint8_t
#define GLbyte        int8_t
#define GLubyte       uint8_t
#define GLchar        char
#define GLshort       int16_t
#define GLushort      uint16_t
#define GLint         int32_t
#define GLuint        uint32_t
#define GLfixed       int32_t
#define GLint64       int64_t
#define GLuint64      uint64_t
#define GLsizei       int32_t
#define GLenum        uint32_t
#define GLintptr      int32_t
#define GLsizeiptr    uint32_t
#define GLsync        int32_t
#define GLfloat       float
#define GLclampf      float
#define GLdouble      double
#define GLclampd      double
#define GLvoid        void
#define GLclampx      int32_t

#define EGLBoolean    int32_t
#define EGLDisplay    void*
#define EGLenum       uint32_t
#define EGLSurface    void*
#define EGLContext    void*
#define EGLConfig     void*
#define EGLint        int32_t

#define EGLint64      int64_t
#define EGLuint64     uint64_t

#define NativeDisplayType void*

#define GL_FALSE                              0
#define GL_TRUE                               1
#define EGL_FALSE                             0
#define EGL_TRUE                              1

#define GL_NO_ERROR                           0

#define GL_ZERO                               0
#define GL_ONE                                1

#define GL_NONE                               0

#define GL_INVALID_INDEX                      0xFFFFFFFF

#define GL_POINTS                                       0x0000
#define GL_LINES                                        0x0001
#define GL_LINE_LOOP                                    0x0002
#define GL_LINE_STRIP                                   0x0003
#define GL_TRIANGLES                                    0x0004
#define GL_TRIANGLE_STRIP                               0x0005
#define GL_TRIANGLE_FAN                                 0x0006
#define GL_QUADS                                        0x0007
#define GL_QUAD_STRIP                                   0x0008
#define GL_POLYGON                                      0x0009
#define GL_ADD                                          0x0104
#define GL_NEVER                                        0x0200
#define GL_LESS                                         0x0201
#define GL_EQUAL                                        0x0202
#define GL_LEQUAL                                       0x0203
#define GL_GREATER                                      0x0204
#define GL_NOTEQUAL                                     0x0205
#define GL_GEQUAL                                       0x0206
#define GL_ALWAYS                                       0x0207
#define GL_SRC_COLOR                                    0x0300
#define GL_ONE_MINUS_SRC_COLOR                          0x0301
#define GL_SRC_ALPHA                                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA                          0x0303
#define GL_DST_ALPHA                                    0x0304
#define GL_ONE_MINUS_DST_ALPHA                          0x0305
#define GL_DST_COLOR                                    0x0306
#define GL_ONE_MINUS_DST_COLOR                          0x0307
#define GL_SRC_ALPHA_SATURATE                           0x0308
#define GL_FRONT                                        0x0404
#define GL_BACK                                         0x0405
#define GL_FRONT_AND_BACK                               0x0408
#define GL_INVALID_ENUM                                 0x0500
#define GL_INVALID_VALUE                                0x0501
#define GL_INVALID_OPERATION                            0x0502
#define GL_STACK_OVERFLOW                               0x0503
#define GL_STACK_UNDERFLOW                              0x0504
#define GL_OUT_OF_MEMORY                                0x0505
#define GL_EXP                                          0x0800
#define GL_EXP2                                         0x0801
#define GL_CW                                           0x0900
#define GL_CCW                                          0x0901
#define GL_CURRENT_COLOR                                0x0B00
#define GL_POLYGON_MODE                                 0x0B40
#define GL_CULL_FACE                                    0x0B44
#define GL_CULL_FACE_MODE                               0x0B45
#define GL_FRONT_FACE                                   0x0B46
#define GL_LIGHTING                                     0x0B50
#define GL_LIGHT_MODEL_AMBIENT                          0x0B53
#define GL_SHADE_MODEL                                  0x0B54
#define GL_COLOR_MATERIAL                               0x0B57
#define GL_FOG                                          0x0B60
#define GL_FOG_DENSITY                                  0x0B62
#define GL_FOG_START                                    0x0B63
#define GL_FOG_END                                      0x0B64
#define GL_FOG_MODE                                     0x0B65
#define GL_FOG_COLOR                                    0x0B66
#define GL_DEPTH_RANGE                                  0x0B70
#define GL_DEPTH_TEST                                   0x0B71
#define GL_DEPTH_WRITEMASK                              0x0B72
#define GL_DEPTH_CLEAR_VALUE                            0x0B73
#define GL_DEPTH_FUNC                                   0x0B74
#define GL_STENCIL_TEST                                 0x0B90
#define GL_STENCIL_CLEAR_VALUE                          0x0B91
#define GL_STENCIL_FUNC                                 0x0B92
#define GL_STENCIL_VALUE_MASK                           0x0B93
#define GL_STENCIL_FAIL                                 0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL                      0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS                      0x0B96
#define GL_STENCIL_REF                                  0x0B97
#define GL_STENCIL_WRITEMASK                            0x0B98
#define GL_MATRIX_MODE                                  0x0BA0
#define GL_NORMALIZE                                    0x0BA1
#define GL_VIEWPORT                                     0x0BA2
#define GL_MODELVIEW_MATRIX                             0x0BA6
#define GL_PROJECTION_MATRIX                            0x0BA7
#define GL_TEXTURE_MATRIX                               0x0BA8
#define GL_ALPHA_TEST                                   0x0BC0
#define GL_ALPHA_TEST_REF                               0x0BC2
#define GL_BLEND_DST                                    0x0BE0
#define GL_BLEND_SRC                                    0x0BE1
#define GL_BLEND                                        0x0BE2
#define GL_SCISSOR_BOX                                  0x0C10
#define GL_SCISSOR_TEST                                 0x0C11
#define GL_COLOR_CLEAR_VALUE                            0x0C22
#define GL_COLOR_WRITEMASK                              0x0C23
#define GL_DOUBLEBUFFER                                 0x0C32
#define GL_PERSPECTIVE_CORRECTION_HINT                  0x0C50
#define GL_UNPACK_ROW_LENGTH                            0x0CF2
#define GL_UNPACK_ALIGNMENT                             0x0CF5
#define GL_PACK_ALIGNMENT                               0x0D05
#define GL_ALPHA_SCALE                                  0x0D1C
#define GL_MAX_LIGHTS                                   0x0D31
#define GL_MAX_CLIP_PLANES                              0x0D32
#define GL_MAX_TEXTURE_SIZE                             0x0D33
#define GL_MAX_MODELVIEW_STACK_DEPTH                    0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH                   0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH                      0x0D39
#define GL_MAX_VIEWPORT_DIMS                            0x0D3A
#define GL_RED_BITS                                     0x0D52
#define GL_GREEN_BITS                                   0x0D53
#define GL_BLUE_BITS                                    0x0D54
#define GL_ALPHA_BITS                                   0x0D55
#define GL_DEPTH_BITS                                   0x0D56
#define GL_STENCIL_BITS                                 0x0D57
#define GL_TEXTURE_1D                                   0x0DE0
#define GL_TEXTURE_2D                                   0x0DE1
#define GL_DONT_CARE                                    0x1100
#define GL_FASTEST                                      0x1101
#define GL_NICEST                                       0x1102
#define GL_AMBIENT                                      0x1200
#define GL_DIFFUSE                                      0x1201
#define GL_SPECULAR                                     0x1202
#define GL_POSITION                                     0x1203
#define GL_CONSTANT_ATTENUATION                         0x1207
#define GL_LINEAR_ATTENUATION                           0x1208
#define GL_QUADRATIC_ATTENUATION                        0x1209
#define GL_COMPILE                                      0x1300
#define GL_COMPILE_AND_EXECUTE                          0x1301
#define GL_BYTE                                         0x1400
#define GL_UNSIGNED_BYTE                                0x1401
#define GL_SHORT                                        0x1402
#define GL_UNSIGNED_SHORT                               0x1403
#define GL_INT                                          0x1404
#define GL_UNSIGNED_INT                                 0x1405
#define GL_FLOAT                                        0x1406
#define GL_HALF_FLOAT                                   0x140B
#define GL_FIXED                                        0x140C
#define GL_INVERT                                       0x150A
#define GL_EMISSION                                     0x1600
#define GL_SHININESS                                    0x1601
#define GL_AMBIENT_AND_DIFFUSE                          0x1602
#define GL_MODELVIEW                                    0x1700
#define GL_PROJECTION                                   0x1701
#define GL_TEXTURE                                      0x1702
#define GL_COLOR_INDEX                                  0x1900
#define GL_DEPTH_COMPONENT                              0x1902
#define GL_RED                                          0x1903
#define GL_GREEN                                        0x1904
#define GL_BLUE                                         0x1905
#define GL_ALPHA                                        0x1906
#define GL_RGB                                          0x1907
#define GL_RGBA                                         0x1908
#define GL_LUMINANCE                                    0x1909
#define GL_LUMINANCE_ALPHA                              0x190A
#define GL_POINT                                        0x1B00
#define GL_LINE                                         0x1B01
#define GL_FILL                                         0x1B02
#define GL_FLAT                                         0x1D00
#define GL_SMOOTH                                       0x1D01
#define GL_KEEP                                         0x1E00
#define GL_REPLACE                                      0x1E01
#define GL_INCR                                         0x1E02
#define GL_DECR                                         0x1E03
#define GL_VENDOR                                       0x1F00
#define GL_RENDERER                                     0x1F01
#define GL_VERSION                                      0x1F02
#define GL_EXTENSIONS                                   0x1F03
#define GL_MODULATE                                     0x2100
#define GL_DECAL                                        0x2101
#define GL_TEXTURE_ENV_MODE                             0x2200
#define GL_TEXTURE_ENV_COLOR                            0x2201
#define GL_TEXTURE_ENV                                  0x2300
#define GL_NEAREST                                      0x2600
#define GL_LINEAR                                       0x2601
#define GL_NEAREST_MIPMAP_NEAREST                       0x2700
#define GL_LINEAR_MIPMAP_NEAREST                        0x2701
#define GL_NEAREST_MIPMAP_LINEAR                        0x2702
#define GL_LINEAR_MIPMAP_LINEAR                         0x2703
#define GL_TEXTURE_MAG_FILTER                           0x2800
#define GL_TEXTURE_MIN_FILTER                           0x2801
#define GL_TEXTURE_WRAP_S                               0x2802
#define GL_TEXTURE_WRAP_T                               0x2803
#define GL_CLAMP                                        0x2900
#define GL_REPEAT                                       0x2901
#define GL_POLYGON_OFFSET_UNITS                         0x2A00
#define GL_POLYGON_OFFSET_POINT                         0x2A01
#define GL_POLYGON_OFFSET_LINE                          0x2A02
#define GL_V2F                                          0x2A20
#define GL_V3F                                          0x2A21
#define GL_C4UB_V2F                                     0x2A22
#define GL_C4UB_V3F                                     0x2A23
#define GL_C3F_V3F                                      0x2A24
#define GL_T2F_V3F                                      0x2A27
#define GL_T4F_V4F                                      0x2A28
#define GL_T2F_C4UB_V3F                                 0x2A29
#define GL_T2F_C3F_V3F                                  0x2A2A
#define GL_CLIP_PLANE0                                  0x3000
#define GL_CLIP_PLANE1                                  0x3001
#define GL_CLIP_PLANE2                                  0x3002
#define GL_CLIP_PLANE3                                  0x3003
#define GL_CLIP_PLANE4                                  0x3004
#define GL_CLIP_PLANE5                                  0x3005
#define GL_CLIP_PLANE6                                  0x3006
#define GL_LIGHT0                                       0x4000
#define GL_LIGHT1                                       0x4001
#define GL_LIGHT2                                       0x4002
#define GL_LIGHT3                                       0x4003
#define GL_LIGHT4                                       0x4004
#define GL_LIGHT5                                       0x4005
#define GL_LIGHT6                                       0x4006
#define GL_LIGHT7                                       0x4007
#define GL_ABGR_EXT                                     0x8000
#define GL_FUNC_ADD                                     0x8006
#define GL_MIN                                          0x8007
#define GL_MAX                                          0x8008
#define GL_BLEND_EQUATION                               0x8009
#define GL_FUNC_SUBTRACT                                0x800A
#define GL_FUNC_REVERSE_SUBTRACT                        0x800B
#define GL_UNSIGNED_SHORT_4_4_4_4                       0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1                       0x8034
#define GL_UNSIGNED_INT_8_8_8_8                         0x8035
#define GL_POLYGON_OFFSET_FILL                          0x8037
#define GL_POLYGON_OFFSET_FACTOR                        0x8038
#define GL_INTENSITY                                    0x8049
#define GL_RGB8                                         0x8051
#define GL_RGBA4                                        0x8056
#define GL_RGB5_A1                                      0x8057
#define GL_RGBA8                                        0x8058
#define GL_TEXTURE_BINDING_2D                           0x8069
#define GL_VERTEX_ARRAY                                 0x8074
#define GL_NORMAL_ARRAY                                 0x8075
#define GL_COLOR_ARRAY                                  0x8076
#define GL_TEXTURE_COORD_ARRAY                          0x8078
#define GL_VERTEX_ARRAY_SIZE                            0x807A
#define GL_VERTEX_ARRAY_TYPE                            0x807B
#define GL_VERTEX_ARRAY_STRIDE                          0x807C
#define GL_NORMAL_ARRAY_TYPE                            0x807E
#define GL_NORMAL_ARRAY_STRIDE                          0x807F
#define GL_COLOR_ARRAY_SIZE                             0x8081
#define GL_COLOR_ARRAY_TYPE                             0x8082
#define GL_COLOR_ARRAY_STRIDE                           0x8083
#define GL_TEXTURE_COORD_ARRAY_SIZE                     0x8088
#define GL_TEXTURE_COORD_ARRAY_TYPE                     0x8089
#define GL_TEXTURE_COORD_ARRAY_STRIDE                   0x808A
#define GL_VERTEX_ARRAY_POINTER                         0x808E
#define GL_NORMAL_ARRAY_POINTER                         0x808F
#define GL_COLOR_ARRAY_POINTER                          0x8090
#define GL_TEXTURE_COORD_ARRAY_POINTER                  0x8092
#define GL_BLEND_DST_RGB                                0x80C8
#define GL_BLEND_SRC_RGB                                0x80C9
#define GL_BLEND_DST_ALPHA                              0x80CA
#define GL_BLEND_SRC_ALPHA                              0x80CB
#define GL_COLOR_TABLE                                  0x80D0
#define GL_BGR                                          0x80E0
#define GL_BGRA                                         0x80E1
#define GL_COLOR_INDEX8_EXT                             0x80E5
#define GL_MAX_ELEMENTS_VERTICES                        0x80E8
#define GL_MAX_ELEMENTS_INDICES                         0x80E9
#define GL_PHONG_WIN                                    0x80EA
#define GL_CLAMP_TO_EDGE                                0x812F
#define GL_DEPTH_COMPONENT16                            0x81A5
#define GL_DEPTH_COMPONENT24                            0x81A6
#define GL_DEPTH_COMPONENT32                            0x81A7
#define GL_DEPTH_STENCIL_ATTACHMENT                     0x821A
#define GL_MAJOR_VERSION                                0x821B
#define GL_MINOR_VERSION                                0x821C
#define GL_NUM_EXTENSIONS                               0x821D
#define GL_RG                                           0x8227
#define GL_R8                                           0x8229
#define GL_RG8                                          0x822B
#define GL_QUERY_TARGET                                 0x82EA
#define GL_UNSIGNED_SHORT_5_6_5                         0x8363
#define GL_UNSIGNED_SHORT_1_5_5_5_REV                   0x8366
#define GL_UNSIGNED_INT_8_8_8_8_REV                     0x8367
#define GL_MIRRORED_REPEAT                              0x8370
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT                 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT                0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT                0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT                0x83F3
#define GL_TEXTURE0                                     0x84C0
#define GL_TEXTURE1                                     0x84C1
#define GL_TEXTURE2                                     0x84C2
#define GL_TEXTURE3                                     0x84C3
#define GL_TEXTURE4                                     0x84C4
#define GL_TEXTURE5                                     0x84C5
#define GL_TEXTURE6                                     0x84C6
#define GL_TEXTURE7                                     0x84C7
#define GL_TEXTURE8                                     0x84C8
#define GL_TEXTURE9                                     0x84C9
#define GL_TEXTURE10                                    0x84CA
#define GL_TEXTURE11                                    0x84CB
#define GL_TEXTURE12                                    0x84CC
#define GL_TEXTURE13                                    0x84CD
#define GL_TEXTURE14                                    0x84CE
#define GL_TEXTURE15                                    0x84CF
#define GL_ACTIVE_TEXTURE                               0x84E0
#define GL_CLIENT_ACTIVE_TEXTURE                        0x84E1
#define GL_MAX_TEXTURE_UNITS                            0x84E2
#define GL_SUBTRACT                                     0x84E7
#define GL_MAX_RENDERBUFFER_SIZE                        0x84E8
#define GL_TEXTURE_COMPRESSION_HINT                     0x84EF
#define GL_TEXTURE_MAX_ANISOTROPY_EXT                   0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT               0x84FF
#define GL_TEXTURE_LOD_BIAS                             0x8501
#define GL_INCR_WRAP                                    0x8507
#define GL_DECR_WRAP                                    0x8508
#define GL_TEXTURE_CUBE_MAP                             0x8513
#define GL_TEXTURE_BINDING_CUBE_MAP                     0x8514
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X                  0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X                  0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y                  0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y                  0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z                  0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z                  0x851A
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE                    0x851C
#define GL_COMBINE                                      0x8570
#define GL_COMBINE_RGB                                  0x8571
#define GL_COMBINE_ALPHA                                0x8572
#define GL_RGB_SCALE                                    0x8573
#define GL_ADD_SIGNED                                   0x8574
#define GL_INTERPOLATE                                  0x8575
#define GL_CONSTANT                                     0x8576
#define GL_PRIMARY_COLOR                                0x8577
#define GL_PREVIOUS                                     0x8578
#define GL_SRC0_RGB                                     0x8580
#define GL_SRC1_RGB                                     0x8581
#define GL_SRC2_RGB                                     0x8582
#define GL_SRC0_ALPHA                                   0x8588
#define GL_SRC1_ALPHA                                   0x8589
#define GL_SRC2_ALPHA                                   0x858A
#define GL_OPERAND0_RGB                                 0x8590
#define GL_OPERAND1_RGB                                 0x8591
#define GL_OPERAND2_RGB                                 0x8592
#define GL_OPERAND0_ALPHA                               0x8598
#define GL_OPERAND1_ALPHA                               0x8599
#define GL_OPERAND2_ALPHA                               0x859A
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED                  0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE                     0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE                   0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE                     0x8625
#define GL_CURRENT_VERTEX_ATTRIB                        0x8626
#define GL_VERTEX_ATTRIB_ARRAY_POINTER                  0x8645
#define GL_PROGRAM_ERROR_POSITION_ARB                   0x864B
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS               0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS                   0x86A3
#define GL_DOT3_RGB                                     0x86AE
#define GL_DOT3_RGBA                                    0x86AF
#define GL_PROGRAM_BINARY_LENGTH                        0x8741
#define GL_MIRROR_CLAMP_EXT                             0x8742
#define GL_BUFFER_SIZE                                  0x8764
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD              0x87EE
#define GL_NUM_PROGRAM_BINARY_FORMATS                   0x87FE
#define GL_RGBA16F                                      0x881A
#define GL_BLEND_EQUATION_ALPHA                         0x883D
#define GL_POINT_SPRITE                                 0x8861
#define GL_QUERY_RESULT                                 0x8866
#define GL_QUERY_RESULT_AVAILABLE                       0x8867
#define GL_MAX_VERTEX_ATTRIBS                           0x8869
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED               0x886A
#define GL_MAX_TEXTURE_COORDS                           0x8871
#define GL_MAX_TEXTURE_IMAGE_UNITS                      0x8872
#define GL_ARRAY_BUFFER                                 0x8892
#define GL_ELEMENT_ARRAY_BUFFER                         0x8893
#define GL_ARRAY_BUFFER_BINDING                         0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING                 0x8895
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING           0x889F
#define GL_READ_ONLY                                    0x88B8
#define GL_WRITE_ONLY                                   0x88B9
#define GL_READ_WRITE                                   0x88BA
#define GL_STREAM_DRAW                                  0x88E0
#define GL_STREAM_READ                                  0x88E1
#define GL_STREAM_COPY                                  0x88E2
#define GL_STATIC_DRAW                                  0x88E4
#define GL_STATIC_READ                                  0x88E5
#define GL_STATIC_COPY                                  0x88E6
#define GL_DYNAMIC_DRAW                                 0x88E8
#define GL_DYNAMIC_READ                                 0x88E9
#define GL_DYNAMIC_COPY                                 0x88EA
#define GL_DEPTH24_STENCIL8                             0x88F0
#define GL_CG_VERTEX_SHADER_EXT                         0x890E
#define GL_CG_FRAGMENT_SHADER_EXT                       0x890F
#define GL_SAMPLES_PASSED                               0x8914
#define GL_SAMPLER_BINDING                              0x8919
#define GL_UNIFORM_BUFFER                               0x8A11
#define GL_FRAGMENT_SHADER                              0x8B30
#define GL_VERTEX_SHADER                                0x8B31
#define GL_MAX_VARYING_FLOATS                           0x8B4B
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS              0x8B49
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS                0x8B4A
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS               0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS             0x8B4D
#define GL_SHADER_TYPE                                  0x8B4F
#define GL_FLOAT_VEC2                                   0x8B50
#define GL_FLOAT_VEC3                                   0x8B51
#define GL_FLOAT_VEC4                                   0x8B52
#define GL_INT_VEC2                                     0x8B53
#define GL_INT_VEC3                                     0x8B54
#define GL_INT_VEC4                                     0x8B55
#define GL_FLOAT_MAT2                                   0x8B5A
#define GL_FLOAT_MAT3                                   0x8B5B
#define GL_FLOAT_MAT4                                   0x8B5C
#define GL_SAMPLER_2D                                   0x8B5E
#define GL_SAMPLER_CUBE                                 0x8B60
#define GL_DELETE_STATUS                                0x8B80
#define GL_COMPILE_STATUS                               0x8B81
#define GL_LINK_STATUS                                  0x8B82
#define GL_VALIDATE_STATUS                              0x8B83
#define GL_INFO_LOG_LENGTH                              0x8B84
#define GL_ATTACHED_SHADERS                             0x8B85
#define GL_ACTIVE_UNIFORMS                              0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH                    0x8B87
#define GL_SHADER_SOURCE_LENGTH                         0x8B88
#define GL_ACTIVE_ATTRIBUTES                            0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH                  0x8B8A
#define GL_SHADING_LANGUAGE_VERSION                     0x8B8C
#define GL_CURRENT_PROGRAM                              0x8B8D
#define GL_PALETTE4_RGB8_OES                            0x8B90
#define GL_PALETTE4_RGBA8_OES                           0x8B91
#define GL_PALETTE4_R5_G6_B5_OES                        0x8B92
#define GL_PALETTE4_RGBA4_OES                           0x8B93
#define GL_PALETTE4_RGB5_A1_OES                         0x8B94
#define GL_PALETTE8_RGB8_OES                            0x8B95
#define GL_PALETTE8_RGBA8_OES                           0x8B96
#define GL_PALETTE8_R5_G6_B5_OES                        0x8B97
#define GL_PALETTE8_RGBA4_OES                           0x8B98
#define GL_PALETTE8_RGB5_A1_OES                         0x8B99
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG              0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG              0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG             0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG             0x8C03
#define GL_ANY_SAMPLES_PASSED                           0x8C2F
#define GL_SRGB                                         0x8C40
#define GL_SRGB8                                        0x8C41
#define GL_SRGB_ALPHA                                   0x8C42
#define GL_SRGB8_ALPHA8                                 0x8C43
#define GL_SLUMINANCE_ALPHA                             0x8C44
#define GL_SLUMINANCE8_ALPHA8                           0x8C45
#define GL_SLUMINANCE                                   0x8C46
#define GL_SLUMINANCE8                                  0x8C47
#define GL_COMPRESSED_SRGB                              0x8C48
#define GL_COMPRESSED_SRGB_ALPHA                        0x8C49
#define GL_COMPRESSED_SRGB_S3TC_DXT1                    0x8C4C
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1              0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3              0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5              0x8C4F
#define GL_ATC_RGB_AMD                                  0x8C92
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD                  0x8C93
#define GL_FRAMEBUFFER_BINDING                          0x8CA6
#define GL_RENDERBUFFER_BINDING                         0x8CA7
#define GL_READ_FRAMEBUFFER                             0x8CA8
#define GL_DRAW_FRAMEBUFFER                             0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING                     0x8CAA
#define GL_COLOR_ATTACHMENT0                            0x8CE0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE           0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME           0x8CD1
#define GL_FRAMEBUFFER_COMPLETE                         0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT    0x8CD7
#define GL_MAX_COLOR_ATTACHMENTS                        0x8CDF
#define GL_DEPTH_ATTACHMENT                             0x8D00
#define GL_STENCIL_ATTACHMENT                           0x8D20
#define GL_FRAMEBUFFER                                  0x8D40
#define GL_RENDERBUFFER                                 0x8D41
#define GL_STENCIL_INDEX8                               0x8D48
#define GL_HALF_FLOAT_OES                               0x8D61
#define GL_ETC1_RGB8_OES                                0x8D64
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE              0x8D6A
#define GL_DEPTH_COMPONENT32F                           0x8DAB
#define GL_DEPTH32F_STENCIL8                            0x8DAC
#define GL_FRAMEBUFFER_SRGB                             0x8DB9
#define GL_COMPRESSED_RED_RGTC1                         0x8DBB
#define GL_COMPRESSED_RG_RGTC2                          0x8DBD
#define GL_SHADER_BINARY_FORMATS                        0x8DF8
#define GL_NUM_SHADER_BINARY_FORMATS                    0x8DF9
#define GL_SHADER_COMPILER                              0x8DFA
#define GL_MAX_VERTEX_UNIFORM_VECTORS                   0x8DFB
#define GL_MAX_VARYING_VECTORS                          0x8DFC
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS                 0x8DFD
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX         0x9047
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX   0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG             0x9137
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG             0x9138
#define GL_QUERY_RESULT_NO_WAIT                         0x9194
#define GL_COMPRESSED_RGBA8_ETC2_EAC                    0x9278

#define VGL_YUV420P_NV12_BT601                         0x18E70
#define VGL_YVU420P_NV21_BT601                         0x18E71
#define VGL_YUV420P_NV12_BT709                         0x18E72
#define VGL_YVU420P_NV21_BT709                         0x18E73
#define VGL_YUV420P_BT601                              0x18E74
#define VGL_YVU420P_BT601                              0x18E75
#define VGL_YUV420P_BT709                              0x18E76
#define VGL_YVU420P_BT709                              0x18E77

#define EGL_SUCCESS                                     0x3000
#define EGL_BAD_ATTRIBUTE                               0x3004
#define EGL_BAD_CONTEXT                                 0x3006
#define EGL_BAD_PARAMETER                               0x300C
#define EGL_BAD_SURFACE                                 0x300D
#define EGL_BUFFER_SIZE                                 0x3020
#define EGL_ALPHA_SIZE                                  0x3021
#define EGL_BLUE_SIZE                                   0x3022
#define EGL_GREEN_SIZE                                  0x3023
#define EGL_RED_SIZE                                    0x3024
#define EGL_DEPTH_SIZE                                  0x3025
#define EGL_STENCIL_SIZE                                0x3026
#define EGL_CONFIG_CAVEAT                               0x3027
#define EGL_CONFIG_ID                                   0x3028
#define EGL_LEVEL                                       0x3029
#define EGL_MAX_PBUFFER_HEIGHT                          0x302A
#define EGL_MAX_PBUFFER_PIXELS                          0x302B
#define EGL_MAX_PBUFFER_WIDTH                           0x302C
#define EGL_NATIVE_RENDERABLE                           0x302D
#define EGL_NATIVE_VISUAL_ID                            0x302E
#define EGL_NATIVE_VISUAL_TYPE                          0x302F
#define EGL_SAMPLES                                     0x3031
#define EGL_SAMPLE_BUFFERS                              0x3032
#define EGL_SURFACE_TYPE                                0x3033
#define EGL_TRANSPARENT_TYPE                            0x3034
#define EGL_TRANSPARENT_BLUE_VALUE                      0x3035
#define EGL_TRANSPARENT_GREEN_VALUE                     0x3036
#define EGL_TRANSPARENT_RED_VALUE                       0x3037
#define EGL_NONE                                        0x3038
#define EGL_BIND_TO_TEXTURE_RGB                         0x3039
#define EGL_BIND_TO_TEXTURE_RGBA                        0x303A
#define EGL_MIN_SWAP_INTERVAL                           0x303B
#define EGL_MAX_SWAP_INTERVAL                           0x303C
#define EGL_LUMINANCE_SIZE                              0x303D
#define EGL_ALPHA_MASK_SIZE                             0x303E
#define EGL_COLOR_BUFFER_TYPE                           0x303F
#define EGL_RENDERABLE_TYPE                             0x3040
#define EGL_CONFORMANT                                  0x3042
#define EGL_VENDOR                                      0x3053
#define EGL_VERSION                                     0x3054
#define EGL_EXTENSIONS                                  0x3055
#define EGL_HEIGHT                                      0x3056
#define EGL_WIDTH                                       0x3057
#define EGL_LARGEST_PBUFFER                             0x3058
#define EGL_TEXTURE_RGBA                                0x305E
#define EGL_TEXTURE_2D                                  0x305F
#define EGL_TEXTURE_FORMAT                              0x3080
#define EGL_TEXTURE_TARGET                              0x3081
#define EGL_MIPMAP_TEXTURE                              0x3082
#define EGL_MIPMAP_LEVEL                                0x3083
#define EGL_BACK_BUFFER                                 0x3084
#define EGL_RENDER_BUFFER                               0x3086
#define EGL_VG_COLORSPACE                               0x3087
#define EGL_VG_ALPHA_FORMAT                             0x3088
#define EGL_VG_COLORSPACE_LINEAR                        0x308A
#define EGL_VG_ALPHA_FORMAT_NONPRE                      0x308B
#define EGL_CLIENT_APIS                                 0x308D
#define EGL_RGB_BUFFER                                  0x308E
#define EGL_HORIZONTAL_RESOLUTION                       0x3090
#define EGL_VERTICAL_RESOLUTION                         0x3091
#define EGL_PIXEL_ASPECT_RATIO                          0x3092
#define EGL_SWAP_BEHAVIOR                               0x3093
#define EGL_BUFFER_PRESERVED                            0x3094
#define EGL_CONTEXT_CLIENT_TYPE                         0x3097
#define EGL_CONTEXT_CLIENT_VERSION                      0x3098
#define EGL_MULTISAMPLE_RESOLVE                         0x3099
#define EGL_MULTISAMPLE_RESOLVE_DEFAULT                 0x309A
#define EGL_OPENGL_ES_API                               0x30A0
#define EGL_OPENGL_API                                  0x30A2
#define EGL_TIMESTAMPS_ANDROID                          0x3430

#define EGL_DISPLAY_SCALING                              10000

#define EGL_OPENGL_ES_BIT                               0x0001
#define EGL_OPENGL_ES2_BIT                              0x0004
#define EGL_WINDOW_BIT                                  0x0004
#define EGL_OPENGL_BIT                                  0x0008

#define EGL_DEFAULT_DISPLAY ((NativeDisplayType)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)

#define GL_MAX_TEXTURE_LOD_BIAS               31

#define GL_POINT_BIT          0x00000002
#define GL_LINE_BIT           0x00000004
#define GL_POLYGON_BIT        0x00000008
#define GL_LIGHTING_BIT       0x00000040
#define GL_FOG_BIT            0x00000080
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_VIEWPORT_BIT       0x00000800
#define GL_TRANSFORM_BIT      0x00001000
#define GL_ENABLE_BIT         0x00002000
#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_HINT_BIT           0x00008000
#define GL_SCISSOR_BIT        0x00080000
#define GL_ALL_ATTRIB_BITS    0xFFFFFFFF

#define GL_MAP_READ_BIT                   0x0001
#define GL_MAP_WRITE_BIT                  0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT       0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT      0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT         0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT         0x0020

// Aliases
#define GL_DRAW_FRAMEBUFFER_BINDING GL_FRAMEBUFFER_BINDING
#define GL_BLEND_EQUATION_RGB GL_BLEND_EQUATION

// clang-format on

// gl*
void glActiveTexture(GLenum texture);
void glAlphaFunc(GLenum func, GLfloat ref);
void glAlphaFuncx(GLenum func, GLfixed ref);
void glAttachShader(GLuint prog, GLuint shad);
void glBegin(GLenum mode);
void glBeginQuery(GLenum target, GLuint id);
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
void glBindBuffer(GLenum target, GLuint buffer);
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glBindSampler(GLuint unit, GLuint smp);
void glBindTexture(GLenum target, GLuint texture);
void glBindVertexArray(GLuint array);
void glBlendEquation(GLenum mode);
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
void glBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
void glBufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void glCallList(GLuint list);
void glCallLists(GLsizei n, GLenum type, const void *lists);
GLenum glCheckFramebufferStatus(GLenum target);
GLenum glCheckNamedFramebufferStatus(GLuint target, GLenum dummy);
void glClear(GLbitfield mask);
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClearColorx(GLclampx red, GLclampx green, GLclampx blue, GLclampx alpha);
void glClearDepth(GLdouble depth);
void glClearDepthf(GLclampf depth);
void glClearDepthx(GLclampx depth);
void glClearStencil(GLint s);
void glClientActiveTexture(GLenum texture);
void glClipPlane(GLenum plane, const GLdouble *equation);
void glClipPlanef(GLenum plane, const GLfloat *equation);
void glClipPlanex(GLenum plane, const GLfixed *equation);
void glColor3f(GLfloat red, GLfloat green, GLfloat blue);
void glColor3fv(const GLfloat *v);
void glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
void glColor3ubv(const GLubyte *v);
void glColor3us(GLushort red, GLushort green, GLushort blue);
void glColor3usv(const GLushort *c);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor4fv(const GLfloat *v);
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void glColor4ubv(const GLubyte *v);
void glColor4us(GLushort red, GLushort green, GLushort blue, GLushort alpha);
void glColor4usv(const GLushort *c);
void glColor4x(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void glColorMaterial(GLenum face, GLenum mode);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glColorTable(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const GLvoid *data);
void glCompileShader(GLuint shader);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
void glCompressedTextureImage2D(GLuint texture, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
void glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
void glCopyTextureImage1D(GLuint texture, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
void glCopyTextureImage2D(GLuint texture, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void glCopyTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
void glCopyTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
void glCreateBuffers(GLsizei n, GLuint *buffers);
void glCreateFramebuffers(GLsizei n, GLuint *framebuffers);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum shaderType);
void glCreateTextures(GLenum target, GLsizei n, GLuint *textures);
void glCullFace(GLenum mode);
void glDeleteBuffers(GLsizei n, const GLuint *gl_buffers);
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void glDeleteLists(GLuint list, GLsizei range);
void glDeleteProgram(GLuint prog);
void glDeleteQueries(GLsizei n, const GLuint *ids);
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
void glDeleteSamplers(GLsizei n, const GLuint *smp);
void glDeleteShader(GLuint shad);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glDeleteVertexArrays(GLsizei n, const GLuint *gl_arrays);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDepthRange(GLdouble nearVal, GLdouble farVal);
void glDepthRangef(GLfloat nearVal, GLfloat farVal);
void glDepthRangex(GLfixed nearVal, GLfixed farVal);
void glDisable(GLenum cap);
void glDisableClientState(GLenum array);
void glDisableVertexAttribArray(GLuint index);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei primcount);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *gl_indices, GLint baseVertex);
void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *gl_indices, GLsizei primcount);
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices);
void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, void *indices, GLint basevertex);
void glEnable(GLenum cap);
void glEnableClientState(GLenum array);
void glEnableVertexAttribArray(GLuint index);
void glEnd(void);
void glEndList(void);
void glEndQuery(GLenum target);
void glFinish(void);
void glFlush(void);
void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
void glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length);
void glFogf(GLenum pname, GLfloat param);
void glFogfv(GLenum pname, const GLfloat *params);
void glFogi(GLenum pname, const GLint param);
void glFogx(GLenum pname, GLfixed param);
void glFogxv(GLenum pname, const GLfixed *params);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glFrontFace(GLenum mode);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal);
void glFrustumf(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat nearVal, GLfloat farVal);
void glFrustumx(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed nearVal, GLfixed farVal);
void glGenBuffers(GLsizei n, GLuint *buffers);
void glGenerateMipmap(GLenum target);
void glGenerateTextureMipmap(GLuint target);
void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void glGenQueries(GLsizei n, GLuint *ids);
GLuint glGenLists(GLsizei range);
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void glGenSamplers(GLsizei n, GLuint *smps);
void glGenTextures(GLsizei n, GLuint *textures);
void glGenVertexArrays(GLsizei n, GLuint *res);
void glGetActiveAttrib(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void glGetActiveUniform(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void glGetAttachedShaders(GLuint prog, GLsizei maxCount, GLsizei *count, GLuint *shads);
GLint glGetAttribLocation(GLuint prog, const GLchar *name);
void glGetBooleanv(GLenum pname, GLboolean *params);
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);
void glGetDoublev(GLenum pname, GLdouble *data);
GLenum glGetError(void);
void glGetFloatv(GLenum pname, GLfloat *data);
void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params);
void glGetIntegerv(GLenum pname, GLint *data);
void glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint *params);
void glGetPointerv(GLenum pname, void **params);
void glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary);
void glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params);
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params);
void glGetShaderInfoLog(GLuint handle, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
void glGetShaderiv(GLuint handle, GLenum pname, GLint *params);
void glGetShaderSource(GLuint handle, GLsizei bufSize, GLsizei *length, GLchar *source);
const GLubyte *glGetString(GLenum name);
const GLubyte *glGetStringi(GLenum name, GLuint index);
void glGetTexEnviv(GLenum target, GLenum pname, GLint *params);
GLuint glGetUniformBlockIndex(GLuint prog, const GLchar *uniformBlockName);
GLint glGetUniformLocation(GLuint prog, const GLchar *name);
void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params);
void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params);
void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer);
void glHint(GLenum target, GLenum mode);
void glInterleavedArrays(GLenum format, GLsizei stride, const void *pointer);
GLboolean glIsEnabled(GLenum cap);
GLboolean glIsFramebuffer(GLuint fb);
GLboolean glIsProgram(GLuint program);
GLboolean glIsRenderbuffer(GLuint rb);
GLboolean glIsTexture(GLuint texture);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightModelxv(GLenum pname, const GLfixed *params);
void glLightxv(GLenum light, GLenum pname, const GLfixed *params);
void glLineWidth(GLfloat width);
void glLineWidthx(GLfixed width);
void glLinkProgram(GLuint progr);
void glListBase(GLuint base);
void glLoadIdentity(void);
void glLoadMatrixd(const GLdouble *m);
void glLoadMatrixf(const GLfloat *m);
void glLoadMatrixx(const GLfixed *m);
void glLoadTransposeMatrixf(const GLfloat *m);
void glLoadTransposeMatrixx(const GLfixed *m);
void *glMapBuffer(GLenum target, GLbitfield access);
void *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
void *glMapNamedBuffer(GLuint buffer, GLenum access);
void *glMapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glMateriali(GLenum face, GLenum pname, GLint param);
void glMaterialx(GLenum face, GLenum pname, const GLfixed param);
void glMaterialxv(GLenum face, GLenum pname, const GLfixed *params);
void glMatrixFrustum(GLenum matrixMode, GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glMatrixLoadd(GLenum mode, const GLdouble *matrix);
void glMatrixLoadf(GLenum mode, const GLfloat *matrix);
void glMatrixLoadIdentity(GLenum mode);
void glMatrixMode(GLenum mode);
void glMatrixMultd(GLenum mode, const GLdouble *matrix);
void glMatrixMultf(GLenum mode, const GLfloat *matrix);
void glMatrixOrtho(GLenum matrixMode, GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glMatrixPop(GLenum matrixMode);
void glMatrixPush(GLenum matrixMode);
void glMatrixRotated(GLenum matrixMode, GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
void glMatrixRotatef(GLenum matrixMode, GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glMatrixScaled(GLenum matrixMode, GLdouble x, GLdouble y, GLdouble z);
void glMatrixScalef(GLenum matrixMode, GLfloat x, GLfloat y, GLfloat z);
void glMatrixTranslated(GLenum matrixMode, GLdouble x, GLdouble y, GLdouble z);
void glMatrixTranslatef(GLenum matrixMode, GLfloat x, GLfloat y, GLfloat z);
void glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei drawcount);
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);
void glMultiTexCoord2fv(GLenum target, GLfloat *f);
void glMultiTexCoord2i(GLenum target, GLint s, GLint t);
void glMultMatrixd(const GLdouble *m);
void glMultMatrixf(const GLfloat *m);
void glMultMatrixx(const GLfixed *m);
void glMultTransposeMatrixd(const GLdouble *m);
void glMultTransposeMatrixf(const GLfloat *m);
void glMultTransposeMatrixx(const GLfixed *m);
void glNamedBufferData(GLuint buffer, GLsizei size, const void *data, GLenum usage);
void glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void *data);
void glNamedFramebufferRenderbuffer(GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void glNamedFramebufferTexture(GLuint target, GLenum attachment, GLuint texture, GLint level);
void glNamedFramebufferTexture2D(GLuint target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glNamedRenderbufferStorage(GLuint target, GLenum internalformat, GLsizei width, GLsizei height);
void glNewList(GLuint list, GLenum mode);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3fv(const GLfloat *v);
void glNormal3s(GLshort x, GLshort y, GLshort z);
void glNormal3x(GLfixed x, GLfixed y, GLfixed z);
void glNormalPointer(GLenum type, GLsizei stride, const void *pointer);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal);
void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat nearVal, GLfloat farVal);
void glOrthox(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed nearVal, GLfixed farVal);
void glPixelStorei(GLenum pname, GLint param);
void glPointSize(GLfloat size);
void glPointSizex(GLfixed size);
void glPolygonMode(GLenum face, GLenum mode);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glPolygonOffsetx(GLfixed factor, GLfixed units);
void glPopAttrib(void);
void glPopGroupMarker(void);
void glPopMatrix(void);
void glProgramBinary(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length);
void glProgramUniform1f(GLuint program, GLint location, GLfloat v0);
void glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat *value);
void glProgramUniform1i(GLuint program, GLint location, GLint v0);
void glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint *value);
void glProgramUniform2f(GLuint program, GLint location, GLfloat v0, GLfloat v1);
void glProgramUniform2fv(GLuint program, GLint location, GLsizei count, const GLfloat *value);
void glProgramUniform2i(GLuint program, GLint location, GLint v0, GLint v1);
void glProgramUniform2iv(GLuint program, GLint location, GLsizei count, const GLint *value);
void glProgramUniform3f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void glProgramUniform3fv(GLuint program, GLint location, GLsizei count, const GLfloat *value);
void glProgramUniform3i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2);
void glProgramUniform3iv(GLuint program, GLint location, GLsizei count, const GLint *value);
void glProgramUniform4f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glProgramUniform4fv(GLuint program, GLint location, GLsizei count, const GLfloat *value);
void glProgramUniform4i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
void glProgramUniform4iv(GLuint program, GLint location, GLsizei count, const GLint *value);
void glProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glPushAttrib(GLbitfield mask);
void glPushGroupMarker(GLsizei length, const GLchar *marker);
void glPushMatrix(void);
void glReadBuffer(GLenum mode);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *data);
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2);
void glReleaseShaderCompiler(void);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glRotatex(GLfixed angle, GLfixed x, GLfixed y, GLfixed z);
void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param);
void glSamplerParameteri(GLuint target, GLenum pname, GLint param);
void glScaled(GLdouble x, GLdouble y, GLdouble z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glScalex(GLfixed x, GLfixed y, GLfixed z);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glShadeModel(GLenum mode);
void glShaderBinary(GLsizei count, const GLuint *handles, GLenum binaryFormat, const void *binary, GLsizei length);
void glShaderSource(GLuint handle, GLsizei count, const GLchar *const *string, const GLint *length);
void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
void glStencilMask(GLuint mask);
void glStencilMaskSeparate(GLenum face, GLuint mask);
void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass);
void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(GLfloat *f);
void glTexCoord2i(GLint s, GLint t);
void glTexCoord2s(GLshort s, GLshort t);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, GLfloat *param);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvx(GLenum target, GLenum pname, GLfixed param);
void glTexEnvxv(GLenum target, GLenum pname, GLfixed *param);
void glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *data);
void glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *data);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameteriv(GLenum target, GLenum pname, GLint *param);
void glTexParameterx(GLenum target, GLenum pname, GLfixed param);
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void glTextureImage1D(GLuint texture, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *data);
void glTextureImage2D(GLuint texture, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTextureParameterf(GLuint texture, GLenum pname, GLfloat param);
void glTextureParameteri(GLuint texture, GLenum pname, GLint param);
void glTextureParameteriv(GLuint texture, GLenum pname, GLint *param);
void glTextureParameterx(GLuint texture, GLenum pname, GLfixed param);
void glTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels);
void glTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTranslatex(GLfixed x, GLfixed y, GLfixed z);
void glUniform1f(GLint location, GLfloat v0);
void glUniform1fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform1i(GLint location, GLint v0);
void glUniform1iv(GLint location, GLsizei count, const GLint *value);
void glUniform2f(GLint location, GLfloat v0, GLfloat v1);
void glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform2i(GLint location, GLint v0, GLint v1);
void glUniform2iv(GLint location, GLsizei count, const GLint *value);
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void glUniform3fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2);
void glUniform3iv(GLint location, GLsizei count, const GLint *value);
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
void glUniform4iv(GLint location, GLsizei count, const GLint *value);
void glUniformBlockBinding(GLuint prog, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLboolean glUnmapBuffer(GLenum target);
GLboolean glUnmapNamedBuffer(GLuint buffer);
void glUseProgram(GLuint program);
void glVertex2d(GLdouble x, GLdouble y);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex2i(GLint x, GLint y);
void glVertex2dv(const GLdouble *v);
void glVertex2fv(const GLfloat *v);
void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3i(GLint x, GLint y, GLint z);
void glVertex3dv(const GLdouble *v);
void glVertex3fv(const GLfloat *v);
void glVertexAttrib1f(GLuint index, GLfloat v0);
void glVertexAttrib1fv(GLuint index, const GLfloat *v);
void glVertexAttrib2f(GLuint index, GLfloat v0, GLfloat v1);
void glVertexAttrib2fv(GLuint index, const GLfloat *v);
void glVertexAttrib3f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2);
void glVertexAttrib3fv(GLuint index, const GLfloat *v);
void glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glVertexAttrib4fv(GLuint index, const GLfloat *v);
void glVertexAttribDivisor(GLuint index, GLuint divisor);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

// glu*
void gluBuild2DMipmaps(GLenum target, GLint internalFormat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *data);
void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ, GLdouble centerX, GLdouble centerY, GLdouble centerZ, GLdouble upX, GLdouble upY, GLdouble upZ);
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn, GLenum typeIn, const void * dataIn, GLsizei wOut, GLsizei hOut, GLenum typeOut, void* dataOut);

// egl*
EGLBoolean eglBindAPI(EGLenum api);
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void * win, const EGLint *attrib_list);
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value);
EGLBoolean eglGetConfigs(EGLDisplay display, EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLContext eglGetCurrentContext(void);
EGLDisplay eglGetDisplay(NativeDisplayType native_display);
EGLint eglGetError(void);
void (*eglGetProcAddress(char const *procname))(void);
EGLuint64 eglGetSystemTimeFrequencyNV(void);
EGLuint64 eglGetSystemTimeNV(void);
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLenum eglQueryAPI(void);
EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value);
char const * eglQueryString(EGLDisplay display, EGLint name);
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface eglSurface, EGLint attribute, EGLint *value);
EGLBoolean eglSwapInterval(EGLDisplay display, EGLint interval);
EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface);
EGLBoolean eglTerminate(EGLDisplay dpy);
#endif

// VGL_EXT_gpu_objects_array extension
// glColorPointer equivalent for legacy vgl* draw pipeline.
void vglColorPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);

// Overloads the color object with a pre-GPU mapped memory block with a copy-less action for legacy vgl* draw pipeline.
void vglColorPointerMapped(GLenum type, const GLvoid *pointer);

// Performs a draw with the legacy vgl* draw pipeline.
void vglDrawObjects(GLenum mode, GLsizei count);

// Sets up the index list to use for the subsequent legacy vgl* draw pipeline draw action.
void vglIndexPointer(GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);

// Overloads the index list with an internal progressive index buffer normally used for glDrawArrays call.
void vglIndexPointerDefault();

// Overloads the index list with a pre-GPU mapped memory block with a copy-less action for legacy vgl* draw pipeline.
void vglIndexPointerMapped(const GLvoid *pointer);

// glTexCoordPointer equivalent for legacy vgl* draw pipeline.
void vglTexCoordPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);

// Overloads the texture coords object with a pre-GPU mapped memory block with a copy-less action for legacy vgl* draw pipeline.
void vglTexCoordPointerMapped(const GLvoid *pointer);

// glVertexPointer equivalent for legacy vgl* draw pipeline.
void vglVertexPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);

// Overloads the vertex object with a pre-GPU mapped memory block with a copy-less action for legacy vgl* draw pipeline.
void vglVertexPointerMapped(GLint size, const GLvoid *pointer);

// VGL_EXT_gxp_shaders extension implementation
// glBindAttribLocation equivalent for legacy vgl* draw pipeline.
void vglBindAttribLocation(GLuint prog, GLuint index, const GLchar *name, const GLuint num, const GLenum type);

// glBindAttribLocation equivalent for legacy vgl* draw pipeline with packed attributes support.
GLint vglBindPackedAttribLocation(GLuint prog, const GLchar *name, const GLuint num, const GLenum type, GLuint offset, GLint stride);

// glVertexAttribPointer equivalent for legacy vgl* draw pipeline.
void vglVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, GLuint count, const GLvoid *pointer);

//Overloads a vertex attrib pointer with a pre-GPU mapped memory block with a copy-less action for legacy vgl* draw pipeline.
void vglVertexAttribPointerMapped(GLuint index, const GLvoid *pointer);

// Direct draw (from the Galaxy port): the current program's enabled attributes (a bit per attribute index in `enabled`) from one interleaved buffer object `vbo_name`
// starting `base` bytes in, at the given offsets; the others read their glVertexAttrib* constants, which must have been set with four components (a patched
// program is kept by `key`, so all of a key's draws must use the same count). 16-bit indices from the bound element array buffer at byte offset idx_offset.
// `key` must identify everything in the layout but `base` (same key, same table). GL_TRIANGLES only. GL_FALSE: nothing was drawn, the
// caller draws the ordinary way. GL_TRUE also when no scene could be started (as glDrawElements: nothing submitted, no error).
#define VGL_HAS_DIRECT_DRAW 1
enum { VGL_DIRECT_F32 = 0, VGL_DIRECT_U8N = 1, VGL_DIRECT_U8 = 2 };
typedef struct {
	uint32_t key;
	uint32_t enabled;
	uint16_t stride;
	uint8_t size[16];
	uint8_t type[16];
	uint16_t offset[16];
} vglDirectLayout;
GLboolean vglDrawElementsDirect(GLenum mode, GLsizei count, uint32_t idx_offset, GLuint vbo_name, uint32_t base, const vglDirectLayout *layout);

// 1.3 (from the Galaxy port's draw caches): frag_memo keeps per program what the shader patcher answered; uni_reuse lets a program whose uniforms did not change since its last
// copy in this scene name that copy again. Neither changes what is drawn.
#define VGL_HAS_DRAW_CACHES 1
void vglSetDrawCaches(GLboolean frag_memo, GLboolean uni_reuse); // uni_reuse 2 (be26): also an older copy of the scene that holds the same bytes (VGL_UNI_HIST per program and stage)
void vglSetUniHistCap(uint32_t bytes); // gem2b: the uni_reuse 2 history only for default uniform buffers of at most this many bytes (0: all)
void vglSetViewportExact(GLboolean exact); // 1.3 glViewport halves as floats (patch 0025; default GL_TRUE)
void vglSetMaskKeep(GLboolean on); // 1.3 MASK_KEEP (patch 0026; default GL_FALSE): a scissor mask rebuild the depth/stencil surface already holds is not drawn
void vglGetMaskKeepStats(uint32_t *drawn, uint32_t *kept); // 1.3 MASK_KEEP: mask rebuilds drawn and not drawn so far (counted with the switch off too)
// 0033 scene census: scenes begun per render target in one picture against the target's scenesPerFrame (vglSetupRenderTargetScenesNum). Off until
// vglSceneCensusSetup(1). The worst values since the last reset (reset != 0) and how many pictures had a target past its capacity.
typedef struct {
	uint32_t frames;                          // pictures counted
	uint32_t worst_display, worst_fbo;        // most scenes begun on the display target, on any one framebuffer-object target, in one picture
	uint32_t worst_fbo_targets;               // most distinct framebuffer-object targets in one picture
	uint32_t cap_display, cap_fbo;            // scenesPerFrame the two kinds of target were created with
	uint32_t over_display, over_fbo;          // counts (a target in a picture) past that capacity
	uint32_t slots_full;                      // scenes on a 33rd target in one picture (not counted per target)
} vglSceneCensus;
void vglSceneCensusSetup(int on);
void vglGetSceneCensus(vglSceneCensus *out, int reset);

// 0034 free guard (lab): with vglSetFreeGuard(presents > 0) every glDeleteTextures / glDeleteFramebuffers is held back that many presents (vglSwapBuffers deletes it then), and a delete
// of a texture that a scene drew into within the last FRAME_PURGE_FREQ presents (vitaGL frees such a block at once: only a texture that was SAMPLED recently is deferred) or of the colour
// texture of the framebuffer that is bound is reported as an event (vglFreeGuardTake), once per call site (the return address of the delete) and kind. Off (0, the default) it changes nothing.
typedef struct {
	uint32_t deletes, postponed, referenced, ring_full, sites;
	uint32_t recovery_flushed;   // 0050: held deletes done early by the allocation recovery
	uint32_t purge_peak, rt_purge_peak;   // most elements one picture put on the collector's lists (capacity 16384 and 128: the lists have no bounds check, a longer one writes into the next picture's list)
} vglFreeGuardStats;
typedef struct {
	uint32_t kind;     // 0 texture, 1 framebuffer
	uint32_t id;
	uint32_t reason;   // 1 a scene drew into it recently, 2 colour texture of the bound framebuffer, 3 both
	uint32_t age;      // presents since the scene that drew into it (reason 1, 3)
	uint32_t caller;   // return address of the delete call (arm-vita-eabi-addr2line on the image)
	uint32_t frame;    // present count at the delete
} vglFreeGuardEvent;
void vglSetFreeGuard(int presents);
void vglSetFreeGuardReferencedOnly(int on); // 0047: with the guard on, hold (and report) only the deletes of a block a scene drew into within the last FRAME_PURGE_FREQ presents or that is bound as a framebuffer's colour; all other deletes happen at once as without the guard. Default 0
void vglGetFreeGuardStats(vglFreeGuardStats *out);
int vglFreeGuardTake(vglFreeGuardEvent *out); // 1: an event was returned
void vglSetMaskBackground(int mode); // 1.3 MASK_BG (patch 0029; default 0): a scene that does not load its depth/stencil surface and has the scissor test off begins with the background mask set, and the full-target quad that sets it is not drawn. Bit 0: framebuffer-object scenes, bit 1: the display's scene
void vglGetMaskBackgroundStats(uint32_t *armed, uint32_t *kept); // 1.3 MASK_BG: scenes begun that way, quads left out
void vglSetMaskClip(GLboolean on); // 1.3 MASK_CLIP (patch 0055; default GL_FALSE): the clear quad of a scissor mask rebuild is drawn over the region the mask holds now (tile region clip), not the whole target
void vglGetMaskClipStats(uint32_t *narrowed, uint32_t *kpx_saved); // 1.3 MASK_CLIP: clears narrowed, kilopixels of whole-target fill left out
// 1.3 MEMFREE (patch 0028): vglMemFree / vgl_mem_get_free_space without walking the heap. The allocator wrappers keep per pool the usable bytes and the count of live blocks;
// a pool's free figure is then (free when empty) - (usable + overhead * count), O(1), where `overhead` is the allocator's per-block cost measured at start-up with probe blocks.
// A pool whose probes do not show one constant overhead (or whose statistics call answers nothing: Vita3K) is never answered from the counters.
// mode 0: walk the heap as before (the library default); 1: the counters; 2: the counters, and every answer is also checked against a walk (counted, logged).
void vglSetMemFreeMode(int mode);
// calls answered from the counters / by a walk; checks made, found different, skipped because another thread allocated or freed meanwhile; the largest difference seen (bytes);
// pools whose counters are usable (bit per vglMemType). Any pointer may be NULL.
void vglGetMemFreeStats(uint32_t *fast, uint32_t *walked, uint32_t *checks, uint32_t *mismatches, uint32_t *raced, int32_t *worst, uint32_t *pools);

// 0036: the wait after a failed GPU allocation. Every pool refusing: the GPU is finished once, up to FRAME_PURGE_FREQ collector cycles run with a try after each, and only then
// the thread waits up to ms (default 16; 0 = not at all; 1000 = about what patch 0005 did, 1 s per level) trying every 2 ms. vglGetGcRecoveryStats: recoveries entered,
// resolved, failed (the caller got NULL), those that needed the timed wait, collector-not-running-yet waits, collector cycles forced, longest recovery (us, GPU finish and
// waits included), total time (us), time in the timed wait (us), bytes asked.
void vglSetGcPauseMs(int ms);
void vglSetGcTestInject(int every, int fails); // test only: every `every`-th allocation of 64 KiB or more runs the recovery, whose first `fails` tries are refused too
uint32_t vglGetGcTestInjectHits(void);
void vglGetGcRecoveryStats(uint32_t *recoveries, uint32_t *resolved, uint32_t *failed, uint32_t *pauses, uint32_t *legacy, uint32_t *cycles, uint32_t *max_us, uint64_t *total_us, uint64_t *pause_us, uint64_t *bytes);
GLboolean vglGetUniformShadow(GLuint prog, GLint location, void **vptr, void **fptr, uint32_t *bytes); // 1.3 vgx uniform shadows (patch 0024)
void vglUniformShadowWritten(GLuint prog, uint32_t stages);

// Get the compiled shader binary of a given GL shader.
void vglGetShaderBinary(GLuint index, GLsizei bufSize, GLsizei *length, void *binary);

typedef enum {
	VGL_MEM_VRAM, // CDRAM
	VGL_MEM_RAM, // USER_RW RAM
	VGL_MEM_PHYCONT, // PHYCONT_USER_RW RAM
	VGL_MEM_BUDGET, // CDLG RAM
	VGL_MEM_EXTERNAL, // newlib mem
	VGL_MEM_ALL
} vglMemType;

typedef enum {
	VGL_TYPE_NONE, // No semantic
	VGL_TYPE_TEXCOORD, // TEXCOORD#
	VGL_TYPE_TEXCOORD_CENTROID, // TEXCOORD#_CENTROID
	VGL_TYPE_COLOR, // COLOR#
	VGL_TYPE_COLOR_CENTROID, // COLOR#_CENTROID
	VGL_TYPE_FOG, // FOG
	VGL_TYPE_FOG_CENTROID, // FOG_CENTROID
	VGL_TYPE_CLIP, // CLP#
} vglSemanticType;

typedef enum {
	VGL_MODE_SHADER_PAIR, // Assumes glCompileShader is always called in couple of Vertex + Fragment shaders that will then be linked in the same program. Great results if this premise is respected.
	VGL_MODE_GLOBAL, // Uses a global semantics pool for translating shaders. Less accuracy than VGL_MODE_SHADER_PAIR but has no premise that must be respected.
	VGL_MODE_POSTPONED // Moves shaders compilation into glLinkProgram. Best results since will always have correct shader couples for the translation for accurate semantic bindings resolution.
} vglSemanticMode;

typedef struct {
	SceGxmContext *context; // Usable gxm context during a custom splashscreen rendering
	SceGxmShaderPatcher *patcher; // Usable gxm shader patcher during a custom splashscreen rendering
	float elapsed_seconds; // Elapsed time since splashscreen start in seconds
	float delta_seconds; // Elapsed time since previous splahscreen frame in seconds
	GLboolean must_stop; // Flag whether splashscreen is supposed to end
} vglSplashDetails;

typedef GLboolean (*vglSplashCallback)(const vglSplashDetails *details);

// vgl*
// Add a new global custom semantic binding for the GLSL translator.
void vglAddSemanticBinding(const GLchar *const *varying, GLint index, GLenum type);

// Add a new global custom semantic binding hint for the GLSL translator.
void vglAddSemanticBindingHint(const GLchar *const *varying, GLenum type);

// Alloc memory from vitaGL internal memory pools. Needs to be freed with vglFree.
void *vglAlloc(uint32_t size, vglMemType type);

// Alloc temporary memory from vitaGL internal scratch pool. Allocation is extremely fast if NO_CIRCULAR_POOL is not used. Memory lifetime is guaranteed for 3 frames.
void *vglAllocFromScratch(size_t size);

// Overloads a buffer object with a pre-GPU mapped memory block with a copy-less action.
void vglBufferData(GLenum target, const GLvoid *data);

// calloc implementation for vitaGL internal memory pools.
void *vglCalloc(uint32_t nmember, uint32_t size);

// Alloc memory from vitaGL internal memory pools. If the memory pools exhausted, vitaGL will attempt to free enough memory to not fail this allocation. Needs to be freed with vglFree.
void *vglForceAlloc(uint32_t size, GLboolean for_gpu);

// Alloc aligned memory from vitaGL internal memory pools. If the memory pools exhausted, vitaGL will attempt to free enough memory to not fail this allocation. Needs to be freed with vglFree.
void *vglForceAllocAligned(uint32_t alignment, uint32_t size, GLboolean for_gpu);

// Frees a previously allocated memory block in the vitaGL internal memory pools.
void vglFree(void *addr);

// Get the memory region that stores compressed splashscreen data during boot. This memory region can be safely used as a general purpose buffer after the splashscreen stops rendering.
void *vglGetCaveBuffer(size_t *sz);

// Get the current frame number.
uint32_t vglGetFrameNumber();

// Get a GL function name given a function address.
char *vglGetFuncName(uint32_t func);

// Get the internal sceGxm texture descriptor of a GL texture.
SceGxmTexture *vglGetGxmTexture(GLenum target);

// Get a GL function address given a function name.
void *vglGetProcAddress(const char *name);

// Get the internal texture data pointer of a GL texture.
void *vglGetTexDataPointer(GLenum target);

// Get the internal texture palette data pointer of a GL texture.
void *vglGetTexPaletteDataPointer(GLenum target);

// Simple vitaGL init function. Legacy pool size is the amount of memory to reserve to handle immediate mode usage.
GLboolean vglInit(int legacy_pool_size);

// vitaGL init function with customizable resolution, RAM threshold and MSAA setup.
GLboolean vglInitExtended(int legacy_pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa);

// vitaGL init function with customizable resolution, memory pools sizes and MSAA setup.
GLboolean vglInitWithCustomSizes(int legacy_pool_size, int width, int height, int ram_pool_size, int cdram_pool_size, int phycont_pool_size, int cdlg_pool_size, SceGxmMultisampleMode msaa);

// vitaGL init function with customizable resolution, memory pools thresholds and MSAA setup.
GLboolean vglInitWithCustomThreshold(int pool_size, int width, int height, int ram_threshold, int cdram_threshold, int phycont_threshold, int cdlg_threshold, SceGxmMultisampleMode msaa);

// Mark a memory block from a vitaGL internal memory pool to be deleted as soon as GPU finishes using it.
void vglLazyFree(void *addr);

// malloc implementation for vitaGL internal memory pools.
void *vglMalloc(uint32_t size);

// malloc_usable_size implementation for vitaGL internal memory pools.
size_t vglMallocUsableSize(void *ptr);

// memalign implementation for vitaGL internal memory pools.
void *vglMemalign(uint32_t alignment, uint32_t size);

// Gets the total amount of free memory in a given internal memory pool.
size_t vglMemFree(vglMemType type);

// Gets the total amount of free and used memory in a given internal memory pool.
size_t vglMemTotal(vglMemType type);

// Setups a GL texture with custom internal values.
void vglSetupTexture(GLenum target, void *data, SceGxmTextureFormat format, SceGxmTextureType type, GLsizei width, GLsizei height, GLsizei mip_count);

// Replaces original texture data pointer with a new one in a GL texture.
void vglOverloadTexDataPointer(GLenum target, void *data);
GLboolean vglSwizzleBoundTexture(void); // 0020: the bound linear texture and its mip levels go to the swizzled layout (MSC_VGX_TEXSWIZZLE)

// Replaces original texture palette data pointer with a new one in a GL texture.
void vglOverloadTexPaletteDataPointer(GLenum target, void *data);

// Allows to override texture format of a GL texture. Requires HAVE_UNPURE_TEXFORMATS.
void vglOverrideTexFormat(GLenum target);

// Allows to init phycont memory heap (VGL_MEM_PHYCONT) after vglInit* calls. Useful for when SceAvPlayer is used only for an intro video.
void vglPhycontMemLazyInit(size_t size);

// Variant of glReadPixels that uses GPU underneat to perform the readback. The passed data pointer must be GPU mapped (eg: heap memory).
void vglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *data);

// realloc implementation for vitaGL internal memory pools.
void *vglRealloc(void *ptr, uint32_t size);

// Change the total memory to use for internal circular pools to use in vitaGL. Disabled with NO_CIRCULAR_POOL. Default value: 32 * 1024 * 1024.
void vglSetCircularPoolSize(uint32_t size);

// Set color for the commit hash text during a custom splashscreen rendering
void vglSetCustomSplashscreenHashColor(float r, float g, float b, float a);

// Sets the number of buffers to use for the display swapchain. Default value: 3.
void vglSetDisplayBufferCount(int count);

// Setup a callback executed everytime a new frame is sent to the display. Useful to setup a CPU rendered overlay on-screen.
void vglSetDisplayCallback(void (*cb)(void *framebuf));

// Setup the fragment ring buffer size of sceGxm. Must be called before vglInit*. Default value: SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE.
void vglSetFragmentBufferSize(uint32_t size);

// Setup the parameter buffer size of sceGxm. Must be called before vglInit*. Default value: SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE.
void vglSetParamBufferSize(uint32_t size);

// Change the currently used semantics binding resolution mode for the GLSL translator. Default value: VGL_MODE_POSTPONED.
void vglSetSemanticBindingMode(GLenum mode);

// Sets path where to save shader association data for Razor. If an empty string is supplied, shader association data is disabled. Default value: "".
void vglSetShaderAssociationPath(const char *path);

// Overloads the shader cache path when HAVE_SHADER_CACHE=1 is used. Must be called before vglInit*. Default value: ux0:data/shader_cache.
void vglSetShaderCachePath(const char *path);
void vglSetShaderPackLookup(const void *(*fn)(int fragment, uint64_t hash, uint32_t *size)); // W5 BOOT: in-memory compiled shaders, asked first (NULL = none)
void vglSetShaderSeedPath(const char *path); // W5 SEED: read-only second shader cache folder (<path>/{v,f}/HH/hash.gxp), tried when the first has no file
int vglShaderCompilerLockInit(void); // 1.3 ASYNC_SHADERS (0027): creates the compiler lock every compile then holds (0 = ok)
int vglShaderCacheProbe(GLenum type, const char *const *parts, int count); // 1.3 (0027): 1 = the pack / cache / seed has this text's compiled form
int vglCompileShaderToCache(GLenum type, const char *const *parts, int count); // 1.3 (0027): compile into the cache, any thread (1 done, 2 cached, 0 failed, <0 no compiler)

// Overloads the default vitaGL boot splashscreen with a custom one. Not compatible with NO_SPLASHSCREEN=1.
void vglSetupCustomSplashscreen(vglSplashCallback init_callback, vglSplashCallback term_callback, vglSplashCallback main_callback, GLboolean clear_screen);

// Change the lifetime for a texture to be considered cacheable. Requires HAVE_TEXTURE_CACHE.
void vglSetTextureCacheFrequency(GLuint freq);

// Setup the fragment USSE ring buffer size of sceGxm. Must be called before vglInit*. Default value: SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE.
void vglSetUSSEBufferSize(uint32_t size);

// Setup the VDM ring buffer size of sceGxm. Must be called before vglInit*. Default value: SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE.
void vglSetVDMBufferSize(uint32_t size);

// Setup the circular pools size used for generic attribute values. There is a unique pool per vertex array object. Default values: Main: 256 * 1024, Auxiliary: 64 * 1024.
void vglSetVertexAttribPoolSize(uint32_t main_size, uint32_t aux_size);

// Setup the vertex ring buffer size of sceGxm. Must be called before vglInit*. Default value: SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE.
void vglSetVertexBufferSize(uint32_t size);

// Change the priority and affinity to use for the garbage collector thread. Must be called before vglInit*.
void vglSetupGarbageCollector(int priority, int affinity);

// Change the scenes per frame value to use for the display and fbos rendertargets. Default value: 1, 1.
void vglSetupRenderTargetScenesNum(uint8_t display_size, uint8_t fbo_size);

// Change what kind of vertex buffer objects are considered eligible for scratch memory usage in order to reduce allocation costs. Requires USE_SCRATCH_MEMORY.
void vglSetupScratchMemory(GLboolean scratch_for_dynamic, GLboolean scratch_for_stream);

// Setup the buffer sizes of the sceGxm shader patcher. Must be called before vglInit*. Default values: 1024 * 1024 each.
void vglSetupShaderPatcher(uint32_t buffer_mem_size, uint32_t vertex_usse_mem_size, uint32_t fragment_usse_mem_size);

// Change optimizations configuration for the runtime shader compiler.
void vglSetupRuntimeShaderCompiler(shark_opt opt_level, int32_t use_fastmath, int32_t use_fastprecision, int32_t use_fastint);

// Load a precompiled gxp binary to a given shader handle
void vglShaderGxpBinary(GLsizei count, const GLuint *handles, const void *binary, GLsizei length);

// 0042: the display's depth/stencil surface without memory, a real one only while a common dialog is up. Must be called before vglInit*.
void vglSetDisplayNoDepth(GLint on);
void vglDisplayDepthStats(uint32_t *mode, uint32_t *real, uint32_t *made, uint32_t *freed, uint32_t *failed);

// 0042: parameter buffer in main memory (SCE_GXM_INITIALIZE_FLAG_PB_LPDDR) in a normal application. Must be called before vglInit*.
void vglSetParamBufferLpddr(GLint on);
int vglParamBufferLpddrState(int *rc, uint32_t *shrink);

// 0049: display queue pending limit (0 = buffers - 1, vitaGL's own) before vglInit*; timing of sceGxmDisplayQueueAddEntry.
void vglSetDisplayQueuePending(GLint n);
void vglSetDisplayQueueTiming(GLint on);
void vglDisplayQueueStats(uint32_t *last_us, uint32_t *max_us, uint32_t *adds, uint32_t *slow, uint32_t *pending, uint64_t *total_us);

// Perform a display buffer swap. Equivalent of eglSwapBuffers but allows support with Common Dialog.
void vglSwapBuffers(GLboolean has_commondialog);

// Enqueue a request for display resolution change that will happen at next vglSwapBuffers call.
GLboolean vglSwapResolution(int width, int height);

// Loads the depth buffer of the currently bound renderbuffer into the currently bound GL texture.
void vglTexImageDepthBuffer(GLenum target);
GLboolean vglFramebufferDepthShare(GLuint fb_id, GLint cw, GLint ch); // 0044: this object's depth surface is a window onto a block shared at cw x ch (framebuffers.c); 0 turns it off
void vglDepthShareStats(uint32_t *out4); // 0044: depth blocks made, objects joined, joins refused, blocks live (mem_utils.c)
GLboolean vglFramebufferMultisample(GLuint fb_id, GLint samples); // 1, 2 or 4 samples a pixel for this framebuffer object alone (framebuffers.c)
GLboolean vglFramebufferNoDepth(GLuint fb_id, GLint on); // 0038: 1: this framebuffer object's scenes get no depth/stencil memory (tile memory only: nothing loaded or stored; for objects whose scenes never test or write depth). 0: as before. GL_FALSE for an unknown object
void vglFramebufferDepthStats(uint32_t *kib_with, uint32_t *objs_with, uint32_t *objs_without, uint32_t *nodepth_scenes); // 0038: depth/stencil memory (KiB) of the live framebuffer objects that have a surface, how many have one, how many run without (nodepth), scenes begun without
// 0062 (MSC_VITA_FBO_KEEP): glFramebufferTexture2D(GL_COLOR_ATTACHMENT0, texture 0) keeps the framebuffer object's width, height, render target and depth surface (default off: it dropped them, and read
// the sizes of texture slot 0 into the object). A later attach of a texture of the same size takes them back, a different size makes them again. Dropped at glDeleteFramebuffers as always.
void vglSetFramebufferDetachKeep(GLboolean on);
GLboolean vglFramebufferDualRT(GLuint fb_id, GLint on); // 0063: 1: this object keeps a second render target (the scene slots only: the colour and depth surfaces are the same) and the pictures of odd frame number begin their scenes on it, so two pictures in flight never share the slots of one target; 0: one target as before. GL_FALSE for an unknown object
void vglGetDualRTStats(uint32_t *made, uint32_t *kib, uint32_t *odd_scenes, uint32_t *failed); // 0063: second targets made, their driver memory in KiB (sceGxmGetRenderTargetMemSize), scenes begun on a second target, makes that failed
GLboolean vglGetFramebufferDetachKeep(void);
void vglSetRenderTargetPool(uint32_t count, uint32_t kib); // 0039: keep up to `count` framebuffer render targets (and `kib` KiB of driver memory, 0: no cap) for the next object of the same shape; 0: off (gxm.c)
void vglGetRenderTargetPoolStats(uint32_t *hits, uint32_t *creates, uint32_t *kept, uint32_t *let_go, uint32_t *kept_now, uint32_t *kib_now); // 0039: reused, newly made, kept by the collector, let go, kept now, KiB kept now
void vglGetGcJobStats(uint32_t *jobs, uint32_t *destroys, uint32_t *job_max_us, uint32_t *destroy_max_us, uint32_t *wait_max_us, uint32_t *destroy_total_us, uint32_t *wait_total_us); // 0064: collector jobs and render target destroys so far; the longest job, the longest destroy and the longest credit wait of the render thread since the last call (cleared by it); the totals of the destroys' and the waits' microseconds
void vglBackendTimingTake(uint32_t *calls, uint64_t *us, uint32_t *max_us); // 0040: [0] shader registrations, [1] glLinkProgram calls, [2] first vglDrawElementsDirect draws of linked programs: count, total and longest microseconds since the last call (arrays of 3), then reset (gxm.c)
int vglStrikersBindFramebufferDepth(GLuint fb_id); // Strikers 1.1: a framebuffer's depth buffer as the bound texture (returns its row width, 0: none yet)

// Makes vitaGL use cached memory instead of uncached memory for its internal memory pools. Must be called before vglInit*.
void vglUseCachedMem(GLboolean use);

// Makes the GLSL translator use low precision variables (eg: float -> half).
void vglUseLowPrecision(GLboolean val);

// Allows to swap between triple and double buffering. Default value: GL_TRUE.
void vglUseTripleBuffering(GLboolean usage);

// Allows to set a preference on the kind of memory to use for the internal USSE buffers in sceGxm. By default vitaGL will not use VRAM memory.
void vglUseVramForUSSE(GLboolean usage);

// Allows vitaGL to use newlib memory once all internal mempools are exhausted. Default value: GL_TRUE.
void vglUseExtraMem(GLboolean usage);
void vglSetOneStream(GLboolean enable); // glDrawElements: one hardware stream for an interleaved layout (DRAW_SPEEDHACK builds only; off by default)

// Simplified function to enable or disable V-Sync. For more fine granularity on the swap interval use eglSwapInterval.
void vglWaitVblankStart(GLboolean enable);

// ---- GLB (wave 5 of the Mario Strikers Charged port): added on top of cdbba42, see platform/vita/vitagl/*.patch ----
#define VITAGL_GLB 1

typedef void (*vglCacheFlushFn)(void *addr, uint32_t size);

// GL_STREAM_DRAW buffers (with scratch memory for streams) are placed in an ARENA of slice_bytes per display buffer instead of the circular pool (uncached RAM):
//   VGL_STREAM_CACHED   cached RAM: CPU writes run at cache speed; the used range is written back with flush(addr, size) (addr and size are multiples of 64)
//                       before the first draw that can read it. flush == NULL: sceKernelSyncVMDomain on the arena's memblock (only meaningful for VM-domain blocks:
//                       use a kubridge callback on the console).
//   VGL_STREAM_CDRAM    CDRAM (write-combined, what the GPU reads fastest): nothing to write back, flush is ignored.
//   VGL_STREAM_UNCACHED uncached RAM, as the circular pool (diagnostic: the arena mechanism alone).
// slice_bytes == 0: off (the default). Must be called before vglInit*. A frame that streams more than a slice falls back to the circular pool for the excess
// (slower, correct). Requires HAVE_SCRATCH_MEMORY.
typedef enum { VGL_STREAM_CACHED = 0, VGL_STREAM_CDRAM = 1, VGL_STREAM_UNCACHED = 2 } vglStreamMem;
void vglSetupStreamArena(uint32_t slice_bytes, int mem, vglCacheFlushFn flush);
void vglSetupCachedStream(uint32_t slice_bytes, vglCacheFlushFn flush); // = vglSetupStreamArena(slice_bytes, VGL_STREAM_CACHED, flush)
// 0060 (MSC_VITA_STREAM_CACHED): with a cached arena, write it back once before sceGxmEndScene instead of before every draw (the GPU reads a scene's vertex and index data after the submit). Default off.
void vglSetStreamFlushAtScene(GLboolean on);

// The uniform ring (8 MiB in our builds; every draw copies its uniform buffers into it, ~330 KB per open-play frame) in CDRAM (write-combined) instead of the
// uncached RAM of the CPU-visible pool. Default: off. Must be called before vglInit*; if the CDRAM block cannot be had the ring stays where it was.
void vglSetUniformRingMem(int cdram);
// 0045 (MSC_VITA_WRCOPY): how data is copied INTO the uncached pools (stream buffers, uniform ring): 0 sceClibMemcpy (default), 1 two aligned 32 byte NEON stores per 64 bytes,
// 2 one 16-register vstm per 128 bytes, 3 sceDmacMemcpy for 8 KiB and more. The same bytes either way. vglWriteCopyVariant makes one copy with a variant (benchmark and check); vglWriteCopyStats: NEON copies, bytes, other copies, bytes.
void vglSetWriteCopy(int mode);
int vglGetWriteCopy(void);
void vglWriteCopyVariant(int mode, void *dst, const void *src, uint32_t n);
void vglWriteCopyStats(uint32_t *out4);
// 0061 (MSC_VITA_WRCOPY_FB): mode 3 gives a copy the DMA engine does not take (under 8 KiB, an address at or above 0x81000000, an error) to mode 1's NEON stores (1, default) or to sceClibMemcpy (0, as 0045).
// vglWriteCopyStats8: the four above, then DMA copies, DMA bytes, 8 KiB+ copies not given to the DMA engine because of an address, DMA errors.
void vglSetWriteCopyFallback(int on);
int vglGetWriteCopyFallback(void);
void vglWriteCopyStats8(uint32_t *out8);
// 0051 (MSC_VITA_TEXCOPY): the copy of a texture's decoded level 0 into its video memory (glTexImage2D / glTexSubImage2D, one contiguous copy): 0 the library's own copy (default), 1 sceDmacMemcpy for 8 KiB and more
// (upstream's vgl_memcpy does this for other copies; these two were left to the CPU), 2 as 1 and every DMA copy is compared with its source and made again by the CPU on a difference (counted).
// vglTextureCopyStats: DMA copies, DMA bytes, DMA microseconds, copies left to the CPU, differences, refusals. The same bytes either way.
void vglSetTextureCopyDma(int mode);
void vglTextureCopyStats(uint32_t *out6);

// Skip sceGxm calls that would set what the context already has (vertex program, fragment program, vertex streams, fragment textures); the cache is
// forgotten at every sceGxmBeginScene / sceGxmEndScene and after calls that change state behind its back. Default: off.
void vglSetSkipRedundantSets(GLboolean enable);

// 1.2 (GXM_STATE_CACHE builds): the same for the state setters of the draw path and of GL's state calls (depth function and write mode, stencil function and
// reference, polygon and cull mode, two-sided, depth bias, viewport, region clip, vertex textures, default uniform buffers and uniform buffer objects): a setter is
// not forwarded when its arguments are those of its last forwarded call since the cache was forgotten (scene begin and end, precomputed draws, reserved uniform
// buffers). mask: 0 off (default), else the groups: 0x01 depth, 0x02 stencil, 0x04 cull/polygon/two-sided/depth bias, 0x08 viewport/region clip, 0x10 vertex
// textures, 0x20 uniform buffers (0x3F = all). Returns the mask in force (0 in a library built without GXM_STATE_CACHE). vglSetSkipRedundantSets stays the switch of
// the program / stream / fragment texture part.
#define VGL_GXS_KINDS 24
uint32_t vglSetStateCache(uint32_t mask);

// 1.2 (LAZY_CLEAR builds, patch 0015): glClear keeps the sceGxm state it set for its quad until something needs the GL state again (the next draw, scene change or
// scene end, a setter of that state), and a clear that follows a clear sets only what differs. Same picture, far fewer state calls. Default: off (the classic glClear).
// No effect in a library built without LAZY_CLEAR (vglGlbGetStats says which).
#define VITAGL_LAZY_CLEAR 1
void vglSetLazyClear(GLboolean on);

typedef struct {
	uint32_t version;
	uint32_t pool_slice_bytes, pool_frame_peak, pool_peak, pool_overruns, pool_overrun_bytes, pool_odd; // circular pool (bytes per display-buffer slice, peaks over the run); pool_odd: reservations whose size was not a multiple of 4 (upstream: the next one started misaligned)
	uint32_t stream_slice_bytes, stream_frame_peak, stream_peak, stream_overruns, stream_overrun_bytes, stream_flushes, stream_flushed_bytes; // stream arena
	uint32_t stream_mem; // 0 no arena, else 1 + vglStreamMem
	uint32_t ring_bytes, ring_frame_peak, ring_peak, ring_wraps, ring_cdram; // uniform ring (ring_cdram: 1 when it is in CDRAM)
	uint32_t programs_live, shaders_live, programs_max, shaders_max; // custom program / shader tables
	uint32_t patcher_host, patcher_buffer, patcher_vusse, patcher_fusse; // shader patcher memory in use (bytes)
	uint32_t skip_calls[4], skip_skipped[4]; // vertex program, fragment program, vertex stream, fragment texture: calls seen / not forwarded
	// version 2 (1.2):
	uint32_t frame; // vglGetFrameNumber() when the statistics were taken
	uint32_t state_mask; // the groups of vglSetStateCache in force
	uint32_t state_built; // 1: the library has GXM_STATE_CACHE
	uint32_t state_calls[VGL_GXS_KINDS], state_skipped[VGL_GXS_KINDS]; // per setter, in the order of gxm_cache.h's GXS_* (front/back depth func, ..., fragment UBO)
	// version 3 (1.2, patch 0015):
	uint32_t lazy_built, lazy_on; // the library has LAZY_CLEAR; vglSetLazyClear is on
	uint32_t lazy_clears, lazy_restores, lazy_continued; // glClear calls made lazily; restores of the state a clear left; clears that found the previous clear's state still in place
	// version 4 (be26): default uniform buffers (vertex and fragment together): copies made to the ring and their bytes; draws that named the last copy again (vglSetDrawCaches uni_reuse >= 1) and the bytes not copied; draws that named an older copy of the
	// same scene (uni_reuse 2) and the bytes not copied
	uint32_t uni_copies, uni_copy_bytes, uni_last_hits, uni_last_bytes, uni_hist_hits, uni_hist_bytes, uni_bad; // uni_bad: only in uni_reuse 3 (verify): reuses whose ring copy differed from the buffer at that draw (must be 0)
	// version 5 (0060): flushes of the cached stream arena made at a scene submit (vglSetStreamFlushAtScene); stream_flushes counts all write-back calls, stream_flushed_bytes their bytes
	uint32_t stream_scene_flushes;
} vglGlbStats;
// Fills *s (all counters are cumulative; the *_frame_peak ones are the last finished frame's).
void vglGlbGetStats(vglGlbStats *s);

// ---- RT (wave 5): scene depth/stencil load and store, scene and GPU timers (source/rt.c) ----
#define VITAGL_RT 1
// vglRtSetup flags (call after vglInit*; default: none of them, the upstream behaviour)
#define VGL_RT_HINTS 0x1       // honour vglRtSceneHint
#define VGL_RT_AUTO_NOLOAD 0x2 // a scene whose first operation is a glClear of depth (and of stencil where the surface has stencil) over the whole target, depth writes on, no scissor, does not load depth/stencil
#define VGL_RT_CPU_TIMERS 0x4  // time sceGxmEndScene, sceGxmBeginScene and the scene set-up after it (sceKernelGetProcessTimeLow)
#define VGL_RT_GPU_TIMERS 0x8  // a fragment notification per scene and a watcher thread (priority, affinity): GPU completion of every scene. Occlusion queries do not work then.
void vglRtSetup(uint32_t flags, int watcher_priority, int watcher_affinity);
// Hints for the scene begun by the next draw, clear or blit on `framebuffer` (0: the display). Consumed by that operation whether or not it begins
// a scene. NOLOAD: nothing in the scene reads depth/stencil that was there before it began. NOSTORE: nothing reads its depth/stencil after it.
#define VGL_RT_NOLOAD 0x1
#define VGL_RT_NOSTORE 0x2
void vglRtSceneHint(GLuint framebuffer, uint32_t hints);
typedef struct {
	uint32_t switch_us;   // CPU time of the frame's scene changes (end + begin + set-up), with VGL_RT_CPU_TIMERS
	uint16_t scenes, noload, nostore; // scenes begun in the frame, of those without depth/stencil load / store
	uint16_t pad;
	uint32_t skipped_kib; // depth/stencil bytes not loaded or stored
} vglRtCpuFrame;
typedef struct {
	uint32_t busy_us, span_us, tail_us; // see rt.c: GPU busy time, first submission to last completion, last submission to last completion
	uint16_t scenes, timed;
	uint32_t pixel_us, tiling_us; // 0021 vglRtSetSplit: the part of busy_us after each scene's tiling ended; tiling seen after submission (a lower bound: scenes overlap)
	uint32_t display_us; // 0030: the part of busy_us spent in the display's scene (it includes the wait for a display buffer; subtract it, add back the blit)
} vglRtGpuFrame;
void vglRtSetSplit(uint32_t enabled);
typedef struct { // 0043 vglRtSceneKinds: cumulative GPU time of the scenes of one kind (target size, display or not), microseconds as the watcher saw them
	uint64_t busy_us, tiling_us, pixel_us;
	uint32_t scenes;
	uint16_t w, h;
	uint8_t display, used, pad[2];
} vglRtSceneKind;
uint32_t vglRtSceneKinds(vglRtSceneKind *out, uint32_t max); // (needs vglRtSetSplit(1) and the GPU timers); 17 entries, the last is "other" (kinds beyond 16)
typedef struct {
	uint32_t version, flags;
	uint32_t scenes, scenes_display, scenes_noload, scenes_noload_auto, scenes_nostore, hints_given, hints_used;
	uint64_t ds_loaded, ds_stored, ds_load_skipped, ds_store_skipped; // depth/stencil bytes
	uint64_t cpu_end_us, cpu_begin_us, cpu_setup_us;
	uint32_t cpu_timed, cpu_switch_max_us;
	float clock_cost_us; // one sceKernelGetProcessTimeLow, measured at vglRtSetup
	uint32_t mask_invalidations_skipped; // scenes that started from the background mask: no full-target mask-clear quad
	uint32_t gpu_scenes_timed, gpu_scenes_untimed, gpu_scenes_late, gpu_pending;
	uint32_t cpu_frame_head, gpu_frame_head; // frames written so far into the rings below (index = n % 128)
	vglRtCpuFrame cpu_frames[128];
	vglRtGpuFrame gpu_frames[128];
	uint64_t begin_disp_us, begin_fbo_us; // 0065 (appended: the layout before it is unchanged): time of the sceGxmBeginScene calls alone on the display target and on framebuffer object targets, with VGL_RT_CPU_TIMERS
	uint32_t begin_disp_max_us, begin_fbo_max_us;
} vglRtStats;
void vglRtGetStats(vglRtStats *s);
uint32_t vglRtLastGpuFrame(vglRtGpuFrame *out);
void vglRtSetRingGuard(uint32_t enabled); // 1.2: default 1; 0 = a full timer ring is overwritten as before. Call before vglRtSetup.
uint32_t vglRtRingDrops(void);            // scenes that were submitted without a timer record because the watcher still held the oldest one
// 1.2 (source/gxm.c): scene entry that fails (no memory for a framebuffer object's depth/stencil or render target, or sceGxmBeginScene refuses) leaves no
// open scene and makes the draw, clear or blit that asked skip itself; the same framebuffer is tried again in the next frame at the earliest.
void vglSetRobustScenes(uint32_t enabled); // default 1; 0 = the 1.1 behaviour (before any drawing)
uint32_t vglRobustFailures(uint32_t *scene_begins, uint32_t *shader_patches); // scene entries refused / shader programs not made, so far (returns their sum)
void vglPatchTimingTake(uint32_t *calls, uint64_t *us, uint32_t *max_us); // 1.3 wave 10 (0035): patcher vertex / fragment program creates and render target creates since the last call (3 entries each: counts, microseconds, longest), then reset
void vglRtSceneProgress(uint32_t *submitted, uint32_t *done); // scenes ended / seen completed by the GPU watcher (GPU timers on; else 0 / 0)
int vglRtWaitScenesDone(uint32_t target, uint32_t timeout_us); // 0022: blocks until done reaches target (0), timeout (1), or -1: poll vglRtSceneProgress instead
void vglSetAnisotropicFilter(uint32_t sceFilter); // 0 off; 2 or 3: the minification filter of trilinear textures (see utils/gxm_utils.h) // the newest finished GPU frame; returns frames finished so far (0: none)
void vglSetAnisotropicMinSize(uint32_t texels); // Strikers 1.3 (0031): textures whose larger side is below this keep trilinear when the anisotropic filter is on (0: none excluded)
void vglTexAnisoOff(GLboolean off); // Strikers 1.3 wave 24 (0057): the texture on the active unit samples trilinear (GL_TRUE) or, again, with the anisotropic filter vglSetAnisotropicFilter chose (GL_FALSE); its descriptor's filter bits only
void vglSetTiledTargets(GLint mode); // Strikers 1.3 wave 24 (0057): allow vglTexMakeTiledTarget: 0 off (default), 1 any size from 32 x 32, 2 only whole-tile sizes
GLboolean vglTexMakeTiledTarget(void); // 0057: the fresh RGBA8 render-target texture on the active unit becomes a tiled one (32 x 32 tiles, zeroed); GL_FALSE: unchanged
void vglTiledStats(uint32_t *made, uint32_t *refused); // 0057: tiled targets made, glReadPixels from one refused
GLboolean vglTexIsTiled(void); // 0057: the texture on the active unit is tiled
void vglFlushStreamArena(void); // 0066: flush cached stream arena before kicking TA mid-scene

#ifdef __cplusplus
}
#endif

#endif
