#ifndef PRIVATE_RT_PLATFORM_TYPES_H
#define PRIVATE_RT_PLATFORM_TYPES_H
#include <stdint.h>
#include <stddef.h>

/* Exact declaration from pinned vitaShaRK source/vitashark.h:29-35. */
typedef enum shark_opt {
	SHARK_OPT_SLOW,      //!< Equivalent to O0
	SHARK_OPT_SAFE,      //!< Equivalent to O1
	SHARK_OPT_DEFAULT,   //!< Equivalent to O2
	SHARK_OPT_FAST,      //!< Equivalent to O3
	SHARK_OPT_UNSAFE     //!< Equivalent to Ofast
} shark_opt;

/* Platform boundary declarations only. Public RT structs come from real vitaGL.h. */
typedef struct SceGxmContext SceGxmContext;
typedef struct SceGxmShaderPatcher SceGxmShaderPatcher;
typedef struct SceGxmTexture SceGxmTexture;
typedef int SceGxmMultisampleMode;
typedef int SceGxmTextureFormat;
typedef int SceGxmTextureType;
typedef int SceUID;
typedef unsigned int SceSize;
typedef struct { volatile unsigned int *address; uint32_t value; } SceGxmNotification;
typedef struct { void *depthData; void *stencilData; int load, store; } SceGxmDepthStencilSurface;   // (depthData: 0038, NULL = no depth memory)
#endif
