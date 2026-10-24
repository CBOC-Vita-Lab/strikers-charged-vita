# vgx native library: the renderer without the recompilation runtime.
#
#   make native                      -> host32 GL    (libvgx-native.a, x86 -m32, GLES2 through EGL / Mesa)
#   make native-null                 -> host32 null  (no GL: functional runs, hash checks)
#   make native NBACKEND=glnull      -> the GL back end over a GL that does nothing (call census, instruction counts; NTARGET=host32 or armhf)
#   make native NTARGET=host64 ...   -> the same for a 64-bit host (convenience; the game itself is ILP32)
#   make native-vita                 -> Vita (vitaGL back end, vita/vgx_host_vita.cpp), VitaSDK + clang as in
#                                       one/vita/Makefile.vita (see native/native-vita.mk)
#
#   NOUT=<dir>   output root (default /home/claude/msc/build/np/NPR/vgx); objects and library go to
#                $(NOUT)/<target>-<backend>/
#
# The library contains vgx/src/*.cpp (without the desktop host and the other back end) plus native/*.cpp.
# vgx_api.cpp is compiled with native/vgx_native_rename.h (see there). Nothing here uses the runtime.
NTARGET  ?= host32
NBACKEND ?= gl
NOUT     ?= /home/claude/msc/build/np/NPR/vgx
HERE     := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT     := $(abspath $(HERE)/..)
SRC      := $(ROOT)/src
NAT      := $(ROOT)/native
OUT      := $(NOUT)/$(NTARGET)-$(NBACKEND)
O        := $(OUT)/obj
CXX      := clang++
CC       := clang
AR       := ar

# Private 32-bit Mesa (EGL, GLESv2, llvmpipe) unpacked from Ubuntu i386 packages: this machine has no i386
# multiarch. See np/docs/render.md "host32 GL" (tools: native/test/i386_mesa.sh makes it).
I386ROOT ?= /home/claude/msc/build/np/NPR/i386apt/root
I386LIB  := $(I386ROOT)/usr/lib/i386-linux-gnu
ifeq ($(NTARGET),host32)
  ARCHFLAGS := -m32 -msse2 -mfpmath=sse -I/usr/include/x86_64-linux-gnu
  GLLIBS    := -L$(I386LIB) -l:libEGL.so.1 -l:libGLESv2.so.2 -Wl,-rpath,$(I386LIB)
else ifeq ($(NTARGET),host64)
  ARCHFLAGS := -march=x86-64-v3
  GLLIBS    := -lEGL -lGLESv2
else ifeq ($(NTARGET),armhf)
  # Cortex-A9 Linux (the console proxy): runs under qemu-arm; null back end only (no ARM GL libraries here)
  ARCHFLAGS := --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
  GLLIBS    :=
  LINKCXX   := arm-linux-gnueabihf-g++
else
  $(error NTARGET must be host32, host64 or armhf here; the Vita is native-vita)
endif
LINKCXX ?= $(CXX)

CORE := vgx_api vgx_rec vgx_play vgx_guestmem vgx_dlcache vgx_state vgx_draw vgx_shadergen vgx_texdecode vgx_texcache vgx_stats vgx_interp vgx_present vgx_capture vgx_skin vgx_hdtex
ifeq ($(NBACKEND),null)
  BACK := vgx_backend_null
else ifeq ($(NBACKEND),glnull)
  # W5 GLB: the GL back end over a GL that does nothing (src/glstub/GLES2/gl2.h defines VGX_GL_STUB and the host's CreateContext): the call census
  # (MSC_VGX_GLSTATS) and the back end's own instruction count without Mesa; armhf runs under qemu-arm. Not a picture.
  BACK := vgx_backend_gl
else
  BACK := vgx_backend_gl vgx_host_egl
endif
SRCS := $(addprefix $(SRC)/,$(addsuffix .cpp,$(CORE) $(BACK))) $(NAT)/vgx_native_bridge.cpp $(NAT)/vgx_host_native.cpp
OBJS := $(patsubst $(ROOT)/%.cpp,$(O)/%.o,$(SRCS))

GLSTUB   := $(if $(filter glnull,$(NBACKEND)),-I$(SRC)/glstub,)
CXXFLAGS := -std=c++20 -O2 -g -pipe $(ARCHFLAGS) $(GLSTUB) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -DTARGET_PC -DNOMINMAX -DVGX_NATIVE -I$(SRC) -I$(NAT) -I$(ROOT)/include -I$(ROOT)/third_party $(EXTRA)

.PHONY: all clean
all: $(OUT)/libvgx-native.a

$(O)/src/vgx_api.o: $(SRC)/vgx_api.cpp $(NAT)/vgx_native_rename.h $(wildcard $(SRC)/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<) '(renamed API)'
	@$(CXX) $(CXXFLAGS) -include $(NAT)/vgx_native_rename.h -c $< -o $@

$(O)/native/%.o: $(NAT)/%.cpp $(wildcard $(SRC)/*.h) $(wildcard $(NAT)/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(O)/%.o: $(ROOT)/%.cpp $(wildcard $(SRC)/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/libvgx-native.a: $(OBJS)
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)
	@echo "built $@ ($(words $(OBJS)) objects)"

clean:
	rm -rf $(OUT)

# ---- smoke test (vgx's own immediate-mode API through the native host): compiled with clang, linked with LINKCXX
.PHONY: smoke
smoke: $(OUT)/smoke
$(OUT)/smoke: $(NAT)/test/smoke.cpp $(OUT)/libvgx-native.a
	@echo LINK smoke
	@$(CXX) $(CXXFLAGS) -include $(NAT)/vgx_native_rename.h -c $< -o $(OUT)/smoke.o
	@$(LINKCXX) $(ARCHFLAGS) -o $@ $(OUT)/smoke.o $(OUT)/libvgx-native.a $(if $(filter gl,$(NBACKEND)),$(GLLIBS),) -lpthread -ldl
