# platform/gx/gx.mk: how np/Makefile builds and links the renderer bridge (NPR; docs/render.md section 9).
#
# In np/Makefile, after TARGET / BUILD_ROOT are known and BEFORE COMMON is defined (the wrapper header
# platform/gx/include/revolution/gx/GXHardware.h must come first on the include path):
#
#     include platform/gx/gx.mk
#     INCLUDES := $(GX_INCLUDES) $(INCLUDES)
#     ...
#     EXTRALIBS += $(GX_LIBS)            # libnpgx.a and libvgx-native.a, before the stub archive
#     LDTAIL    := $(LDTAIL) $(GX_LDLIBS)
#     $(EXE): $(GX_LIBS)                 # (or `make TARGET=x gxlibs` first)
#
# and the game's sources need the patches of platform/gx/patches (after NPA's patches/assets): 0001 .. 0003, `git apply` in order.
#
# Libraries per target (BUILD_ROOT/<target>/gx/):
#   host32  glue + vgx with the GL back end (EGL / GLESv2: the machine's 32-bit Mesa is private, see GX_I386ROOT)
#   armhf   glue + vgx with the null back end (the game under qemu-arm draws nothing)
#   vita    glue + vgx with vitaGL (compiled, never run; link line from vgx/native/native-vita.mk)
# Variables: GX_VGX (the vgx checkout on branch `np`), GX_I386ROOT, VDEPS / VITASDK (Vita).
GX_DIR      := platform/gx
GX_BUILD    := $(BUILD_ROOT)/$(TARGET)/gx
GX_VGX      ?= /home/claude/msc/npw/vgx-np
GX_I386ROOT ?= /home/claude/msc/build/np/NPR/i386apt/root
GX_INCLUDES := -I$(GX_DIR)/include

# host32 only: GX_BACKEND=null links vgx's null back end (the whole game, no pictures, no Mesa: the fast simulation-only run;
# EXESUF=-full-null). Default gl.
GX_BACKEND  ?= gl
ifeq ($(TARGET),host32)
GX_GLUE     := $(GX_BUILD)/libnpgx.a
GX_VGXLIB   := $(GX_BUILD)/vgx/host32-$(GX_BACKEND)/libvgx-native.a
GX_I386LIB  := $(GX_I386ROOT)/usr/lib/i386-linux-gnu
ifeq ($(GX_BACKEND),gl)
GX_LDLIBS   := -L$(GX_I386LIB) -l:libEGL.so.1 -l:libGLESv2.so.2 -Wl,-rpath,$(GX_I386LIB) -lpthread -ldl
else
GX_LDLIBS   := -lpthread -ldl
endif
GX_BUILD_VGX  = $(MAKE) -C $(GX_VGX) -f native/native.mk NOUT=$(abspath $(GX_BUILD))/vgx NBACKEND=$(GX_BACKEND)
GX_BUILD_GLUE = $(MAKE) -C $(GX_DIR) glue ARCH=host32 OUT=$(abspath $(GX_BUILD)) VGXOUT=$(abspath $(GX_BUILD))/vgx VGX=$(GX_VGX)
else ifeq ($(TARGET),armhf)
GX_GLUE     := $(GX_BUILD)/libnpgx.a
# W5 GLB: GX_ARM_BACKEND=glnull links the GL back end over a GL that does nothing (vgx src/glstub): the back end's own instruction count under qemu-arm.
GX_ARM_BACKEND ?= null
GX_VGXLIB   := $(GX_BUILD)/vgx/armhf-$(GX_ARM_BACKEND)/libvgx-native.a
GX_LDLIBS   := -lpthread -ldl
GX_BUILD_VGX  = $(MAKE) -C $(GX_VGX) -f native/native.mk NOUT=$(abspath $(GX_BUILD))/vgx NTARGET=armhf NBACKEND=$(GX_ARM_BACKEND)
GX_BUILD_GLUE = $(MAKE) -C $(GX_DIR) glue ARCH=armhf OUT=$(abspath $(GX_BUILD)) VGXOUT=$(abspath $(GX_BUILD))/vgx VGX=$(GX_VGX)
else ifeq ($(TARGET),vita)
VDEPS       ?= /home/claude/msc/build/one/vdeps-strict
GX_GLUE     := $(GX_BUILD)/vita/libnpgx-vita.a
GX_VGXLIB   := $(GX_BUILD)/vgx/vita-vitagl/libvgx-native-vita.a
# vitaGL and its stubs, as in vgx/native/native-vita.mk (not linked here yet: NPV owns the Vita link)
GX_LDLIBS   := -L$(VDEPS)/lib -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub -lSceShaccCg_stub -lSceGxm_stub \
               -lSceCommonDialog_stub -lSceKernelDmacMgr_stub -lSceAppUtil_stub -lSceKernelThreadMgr_stub -lSceIofilemgr_stub \
               -lSceLibKernel_stub -lSceDisplay_stub -lSceCtrl_stub -pthread -lm
# vgx's Vita library is compiled with the game's ABI contract too (docs/build.md): its flags come from vgx's native-vita.mk (VitaSDK's
# -fsigned-char -fshort-enums, as on the console in the recompiled build) and EXTRA is appended last, so these win (clang takes the last
# of a pair). Audit (NPV2): no vgx source relies on a signed plain char or on 1-byte enums (no char arithmetic; enums cross only as int
# arguments), so the library behaves as before and the link carries no Tag_ABI_enum_size / wchar mismatch against the game objects.
GX_VITA_ABI   ?= -fno-short-enums -funsigned-char -fshort-wchar
GX_BUILD_VGX  = $(MAKE) -C $(GX_VGX) -f native/native-vita.mk NOUT=$(abspath $(GX_BUILD))/vgx 'EXTRA=$(GX_VITA_ABI)'
GX_BUILD_GLUE = $(MAKE) -C $(GX_DIR) glue-vita OUT=$(abspath $(GX_BUILD)) VGX=$(GX_VGX)
endif
# (the vgx library first: the glue calls into it; both are static archives, the link order puts the glue before vgx)
GX_LIBS     := $(GX_GLUE) $(GX_VGXLIB)

.PHONY: gxlibs gxlibs-clean
gxlibs: $(GX_LIBS)
# The sub-makes are incremental: they always run (their own dependencies decide) and the archives are real files, so np/Makefile's
# executable is only relinked when one of them changed.
$(GX_VGXLIB): GX_FORCE
	+@$(GX_BUILD_VGX)
$(GX_GLUE): $(GX_VGXLIB) GX_FORCE
	+@$(GX_BUILD_GLUE)
.PHONY: GX_FORCE
GX_FORCE:
gxlibs-clean:
	rm -rf $(GX_BUILD)
