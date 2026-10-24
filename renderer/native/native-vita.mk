# vgx native library for the PS Vita: libvgx-native-vita.a (vitaGL back end, the renderer's Vita host), no recompilation runtime.
#
#   make native-vita                          (VitaSDK + clang as in one/vita/vita_clang.mk; objects and library under $(NOUT)/vita-vitagl)
#   make -f native/native-vita.mk syntax      syntax-check only, no objects (fast)
#
# The game's executable links this library, libnpgx-vita.a (np/platform/gx: make -C platform/gx glue-vita) and vitaGL (VDEPS), with VitaSDK's
# g++ as the link driver and the stub libraries the vitaGL host needs (see one/vita/Makefile.vita GL_LIBS: -lvitaGL -lvitashark -lSceShaccCgExt
# -lmathneon -ltaihen_stub -lSceShaccCg_stub -lSceGxm_stub -lSceCommonDialog_stub -lSceKernelDmacMgr_stub -lSceAppUtil_stub
# -lSceKernelThreadMgr_stub -lSceIofilemgr_stub -lSceLibKernel_stub -lSceDisplay_stub -lSceCtrl_stub -pthread -lm).
#
# NOT RUN: like the vitaGL host it derives from this has been compiled, never executed here (no console, no emulator in the container); the
# host file is the one that ran on the console in the recompiled build, with the runtime hooks replaced by vgx_native_vita.h services.
VITASDK ?= /opt/vitasdk
VDEPS   ?= /home/claude/msc/build/one/vdeps-strict
NOUT    ?= /home/claude/msc/build/np/NPR/vgx
HERE    := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT    := $(abspath $(HERE)/..)
SRC     := $(ROOT)/src
NAT     := $(ROOT)/native
OUT     := $(NOUT)/vita-vitagl
O       := $(OUT)/obj
VBIN    := $(VITASDK)/bin
VSYS    := $(VITASDK)/arm-vita-eabi
GXXV    := 15.2.0

# clang as a compiler for the VitaSDK ABI (one/vita/vita_clang.mk: why each flag).
VTARGET := --target=armv7a-none-eabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb \
          -fsigned-char -fshort-enums -femulated-tls -fno-pic -munaligned-access \
          -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE \
          -Wno-builtin-macro-redefined \
          -U__WINT_TYPE__ '-D__WINT_TYPE__=unsigned int' \
          -U__INT_FAST8_TYPE__ -D__INT_FAST8_TYPE__=int -U__INT_FAST16_TYPE__ -D__INT_FAST16_TYPE__=int \
          -U__UINT_FAST8_TYPE__ '-D__UINT_FAST8_TYPE__=unsigned int' \
          -U__UINT_FAST16_TYPE__ '-D__UINT_FAST16_TYPE__=unsigned int' \
          -nostdlibinc
CSYS   := -isystem $(VSYS)/include
CXXSYS := -isystem $(VSYS)/include/c++/$(GXXV) -isystem $(VSYS)/include/c++/$(GXXV)/arm-vita-eabi -isystem $(VSYS)/include/c++/$(GXXV)/backward $(CSYS)

VGL_ID := $(shell cat $(VDEPS)/vitagl/options.txt 2>/dev/null)
ifeq ($(strip $(VGL_ID)),)
VGL_ID := unidentified vitaGL
endif

CORE := vgx_api vgx_rec vgx_play vgx_guestmem vgx_dlcache vgx_state vgx_draw vgx_shadergen vgx_texdecode vgx_texcache vgx_stats vgx_interp vgx_present vgx_capture vgx_skin vgx_hdtex vgx_backend_gl
SRCS := $(addprefix $(SRC)/,$(addsuffix .cpp,$(CORE))) $(ROOT)/vita/vgx_host_vita.cpp $(NAT)/vgx_native_bridge.cpp $(NAT)/vgx_native_vita_services.cpp
OBJS := $(patsubst $(ROOT)/%.cpp,$(O)/%.o,$(SRCS))

FLAGS := -std=gnu++20 -O2 -w -pipe $(VTARGET) $(CXXSYS) -DTARGET_PC -DNOMINMAX -D__VGX_VITA__ -DVGX_NATIVE '-DMSC_VGX_COMMIT="np branch"' \
         -I$(ROOT)/vita/include -I$(VDEPS)/include -I$(SRC) -I$(ROOT)/vita -I$(NAT) -I$(NAT)/vita_shim -I$(ROOT)/include -I$(ROOT)/third_party $(EXTRA)

.PHONY: all clean syntax
all: $(OUT)/libvgx-native-vita.a

$(O)/src/vgx_api.o: $(SRC)/vgx_api.cpp $(NAT)/vgx_native_rename.h
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<) '(renamed API)'
	@clang++ $(FLAGS) -include $(NAT)/vgx_native_rename.h -c $< -o $@

$(O)/vita/vgx_host_vita.o: $(ROOT)/vita/vgx_host_vita.cpp $(wildcard $(NAT)/*.h $(NAT)/vita_shim/*.h $(ROOT)/vita/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@clang++ $(FLAGS) '-DMSC_VGL_ID="$(VGL_ID)"' -c $< -o $@

$(O)/native/%.o: $(NAT)/%.cpp $(wildcard $(NAT)/*.h $(NAT)/vita_shim/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@clang++ $(FLAGS) -c $< -o $@

$(O)/src/%.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@clang++ $(FLAGS) -c $< -o $@

$(OUT)/libvgx-native-vita.a: $(OBJS)
	@rm -f $@
	@$(VBIN)/arm-vita-eabi-gcc-ar rcs $@ $(OBJS)
	@echo "built $@ ($(words $(OBJS)) objects)"

# Syntax check of every source with the Vita flags (no objects).
SYNSRC := $(filter-out $(SRC)/vgx_api.cpp $(ROOT)/vita/vgx_host_vita.cpp,$(SRCS))
syntax:
	@set -e; for f in $(SYNSRC); do echo SYN $$(basename $$f); clang++ $(FLAGS) -fsyntax-only $$f; done
	@echo SYN vgx_api.cpp; clang++ $(FLAGS) -include $(NAT)/vgx_native_rename.h -fsyntax-only $(SRC)/vgx_api.cpp
	@echo SYN vgx_host_vita.cpp; clang++ $(FLAGS) '-DMSC_VGL_ID="$(VGL_ID)"' -fsyntax-only $(ROOT)/vita/vgx_host_vita.cpp
	@echo "vita syntax check: ok ($(words $(SRCS)) files)"

clean:
	rm -rf $(OUT)
