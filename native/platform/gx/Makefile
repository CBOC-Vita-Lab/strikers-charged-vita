# np/platform/gx: the native port's GX glue (libnpgx.a) and its tests.
#
#   make glue                  -> $(OUT)/libnpgx.a   (host32; the game is ILP32)
#   make test                  -> builds and runs the standalone drawing test (host32: null back end, then GL)
#   make abi                   -> tools/gx_abi.py: function / enumerator / struct conformance against vgx's headers
#
# Variables:
#   VGX      the vgx checkout on branch `np`      (default /home/claude/msc/npw/vgx-np)
#   VGXOUT   vgx's native build output root      (default /home/claude/msc/build/np/NPR/vgx)
#   RVL_INC  the SDK headers the glue is compiled against (default np/libs/RVL_SDK/include, the game's own)
#   OUT      output root                          (default /home/claude/msc/build/np/NPR/gx)
#   SHIM     header shim for `make abi` only (platform/gx/tools/shim: an empty revolution/os.h and size_t.h: the ABI tool
#            parses the SDK headers without the game's compatibility header)
#
# The glue's SDK-facing units (those that include <revolution/gx.h>) are compiled with the game's ABI flags (np/Makefile:
# COMMON / CXXFLAGS; docs/build.md "ABI contract"): unsigned char, 4-byte enums, 16-bit wchar_t, no strict aliasing, np/compat.h.
# They must agree with the game's objects on struct layouts (GXRenderModeObj, GXTexObj, ...). Keep NPFLAGS in step with np/Makefile.
HERE     := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT     := $(abspath $(HERE))
NPROOT   := $(abspath $(ROOT)/../..)
VGX      ?= /home/claude/msc/npw/vgx-np
VGXOUT   ?= /home/claude/msc/build/np/NPR/vgx
RVL_INC  ?= $(NPROOT)/libs/RVL_SDK/include
ARCH     ?= host32
ifeq ($(ARCH),armhf)
OUT      ?= /home/claude/msc/build/np/NPR/gx-armhf
else
OUT      ?= /home/claude/msc/build/np/NPR/gx
endif
SHIM     ?= $(ROOT)/tools/shim
CXX      := clang++
AR       := ar
ifeq ($(ARCH),armhf)
# Cortex-A9 Linux, the console proxy (runs under qemu-arm; null back end only)
ARCHFLAGS := --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -DNP_TARGET_ARMHF=1
LINKCXX  := arm-linux-gnueabihf-g++
LINKARCH := -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
TESTBACK := null
else
ARCHFLAGS := -m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1
LINKCXX  := clang++
LINKARCH := -m32
endif

# Glue translation units that include the SDK headers (no standard headers in them) ...
SDK_SRC  := gx_core gx_texture gx_frame gx_light gx_transform gx_rmode
# ... and those that do not (standard library allowed).
STD_SRC  := gx_copytrack gx_selfcheck
OBJS     := $(patsubst %,$(OUT)/obj/%.o,$(SDK_SRC) $(STD_SRC))

# The wrapper header of platform/gx/include comes first so that revolution/gx/GXHardware.h resolves to it (it then
# #include_next's the SDK's own).
NPDEFS   := -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1
NPINC    := -I$(ROOT)/include -I$(NPROOT) -I$(NPROOT)/include -I$(RVL_INC) -I$(NPROOT)/libs/Runtime/include \
            -idirafter $(NPROOT)/libs/MSL_C/include -idirafter $(NPROOT)/libs/MetroTRK/include
NPABI    := -funsigned-char -fno-short-enums -fshort-wchar -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
NPFLAGS  := $(NPDEFS) $(NPINC) $(NPABI) -include np/compat.h -std=gnu++11 -fno-exceptions -fno-rtti
SDKFLAGS := $(NPFLAGS) -Wno-macro-redefined -Wno-unknown-pragmas
# (units without SDK headers: C++17, standard library)
CXXFLAGS := -std=c++17 -O2 -g -pipe $(ARCHFLAGS) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-braces -I$(ROOT)/src
SDKCXX   := -O2 -g -pipe $(ARCHFLAGS) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-braces -I$(ROOT)/src

.PHONY: glue clean abi
glue: $(OUT)/libnpgx.a

$(OUT)/obj/gx_selfcheck.o: $(ROOT)/src/gx_selfcheck.cpp $(wildcard $(ROOT)/include/*.h) $(wildcard $(ROOT)/src/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/include -I$(VGX)/src -I$(VGX)/native -c $< -o $@

$(OUT)/obj/gx_copytrack.o: $(ROOT)/src/gx_copytrack.cpp $(wildcard $(ROOT)/include/*.h) $(wildcard $(ROOT)/src/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/include -I$(VGX)/native -c $< -o $@

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp $(wildcard $(ROOT)/include/*.h) $(wildcard $(ROOT)/include/revolution/gx/*.h) $(wildcard $(ROOT)/src/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(SDKCXX) $(SDKFLAGS) -I$(VGX)/native -c $< -o $@

$(OUT)/libnpgx.a: $(OBJS)
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)
	@echo "built $@"

abi:
	python3 -I $(ROOT)/tools/gx_abi.py --rvl $(RVL_INC) --aurora $(VGX)/include --out $(OUT)/abi $(if $(CALLS),--calls $(CALLS),)

clean:
	rm -rf $(OUT)

# ---- the standalone drawing test (no game, no recompilation runtime): scene.cpp (SDK headers) + main.cpp
I386ROOT ?= /home/claude/msc/build/np/NPR/i386apt/root
I386LIB  := $(I386ROOT)/usr/lib/i386-linux-gnu
TESTBACK ?= gl
VGXLIB    = $(VGXOUT)/$(if $(filter armhf,$(ARCH)),armhf,host32)-$(TESTBACK)/libvgx-native.a
GLLIBS    = -L$(I386LIB) -l:libEGL.so.1 -l:libGLESv2.so.2 -Wl,-rpath,$(I386LIB)
TESTOUT   = $(OUT)/test-$(TESTBACK)
.PHONY: test test-exe
test-exe: $(TESTOUT)/gx_native_test

$(TESTOUT)/scene.o: $(ROOT)/test/scene.cpp $(wildcard $(ROOT)/include/*.h) $(wildcard $(ROOT)/include/revolution/gx/*.h)
	@mkdir -p $(dir $@)
	@echo CXX scene.cpp
	@$(CXX) $(SDKCXX) $(SDKFLAGS) -I$(VGX)/native -c $< -o $@

$(TESTOUT)/main.o: $(ROOT)/test/main.cpp $(wildcard $(ROOT)/include/*.h)
	@mkdir -p $(dir $@)
	@echo CXX main.cpp
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/include -I$(VGX)/native -c $< -o $@

$(TESTOUT)/gx_native_test: $(TESTOUT)/scene.o $(TESTOUT)/main.o $(OUT)/libnpgx.a $(VGXLIB)
	@echo LINK gx_native_test '($(TESTBACK))'
	@$(LINKCXX) $(LINKARCH) -o $@ $(TESTOUT)/scene.o $(TESTOUT)/main.o $(OUT)/libnpgx.a $(VGXLIB) $(if $(filter gl,$(TESTBACK)),$(GLLIBS),) -lpthread -ldl

# ---- integration test with the game's own drawing code (host32 GL / null): real game TUs, patched as NPB applies the patches
GTOUT     = $(OUT)/gt-$(TESTBACK)
GT_FILES  = src/NL/glx/GXVertexColourMaterialProgram.cpp src/NL/glx/GXVertexColourMaterialProgramRender.cpp src/NL/glx/glxGX.cpp \
            src/NL/glx/glxDisplayList.cpp src/NL/glx/glxModel.cpp src/Game/GL/glModelBuilder.cpp src/Game/GL/GLColourMeshWriter.cpp
GT_P2     = $(ROOT)/patches/0002-model-stream-host-order-flag.patch
GT_OBJS   = $(patsubst src/%.cpp,$(GTOUT)/obj/%.o,$(GT_FILES))
GTCXX     = -O1 -g -pipe $(ARCHFLAGS) -w $(SDKFLAGS) -I$(VGX)/native
.PHONY: game-test game-test-exe
game-test-exe: $(GTOUT)/gx_game_test

# the sources are copied from np (main already carries NPD's display-list patch and, since NPB's merge, ours: 0002 / 0003; each is applied only if it is not in the copy yet)
$(GTOUT)/patched.stamp: $(addprefix $(NPROOT)/,$(GT_FILES)) $(GT_P2) $(ROOT)/tools/gen_stream_patch.py
	@rm -rf $(GTOUT)/src
	@mkdir -p $(GTOUT)
	@for f in $(GT_FILES); do mkdir -p $(GTOUT)/$$(dirname $$f); cp $(NPROOT)/$$f $(GTOUT)/$$f; done
	@cd $(GTOUT) && { grep -q NP_GL_STREAM_HOST_ORDER src/Game/GL/glModelBuilder.cpp || git apply --include=src/Game/GL/glModelBuilder.cpp $(GT_P2); } \
	  && python3 -I $(ROOT)/tools/gen_stream_patch.py $(GTOUT)
	@touch $@

$(GTOUT)/obj/%.o: $(GTOUT)/patched.stamp $(wildcard $(ROOT)/include/*.h) $(wildcard $(ROOT)/include/revolution/gx/*.h)
	@mkdir -p $(dir $@)
	@echo CXX game $*.cpp
	@$(CXX) $(GTCXX) -c $(GTOUT)/src/$*.cpp -o $@

$(GTOUT)/game_test.o: $(ROOT)/test/game_test.cpp $(wildcard $(ROOT)/include/*.h)
	@mkdir -p $(dir $@)
	@echo CXX game_test.cpp
	@$(CXX) $(GTCXX) -c $< -o $@

$(GTOUT)/gx_game_test: $(GT_OBJS) $(GTOUT)/game_test.o $(OUT)/libnpgx.a $(VGXLIB)
	@echo LINK gx_game_test '($(TESTBACK))'
	@$(LINKCXX) $(LINKARCH) -o $@ $(GT_OBJS) $(GTOUT)/game_test.o $(OUT)/libnpgx.a $(VGXLIB) $(if $(filter gl,$(TESTBACK)),$(GLLIBS),) -lpthread -ldl

game-test: game-test-exe
	cd $(GTOUT) && $(VGX)/native/test/run32.sh ./gx_game_test --png gx_game_test.png
	cd $(GTOUT) && $(VGX)/native/test/run32.sh ./gx_game_test --no-ram --png gx_game_test_noram.png

# run: GL with the arena registered, GL with it unregistered, and the null back end; pictures in $(TESTOUT)/
test: test-exe
	cd $(TESTOUT) && $(VGX)/native/test/run32.sh ./gx_native_test --png gx_native_test.png
	cd $(TESTOUT) && $(VGX)/native/test/run32.sh ./gx_native_test --no-ram --png gx_native_test_noram.png

# ---- Vita: libnpgx-vita.a (VitaSDK ABI through clang, as vgx/native/native-vita.mk and one/vita/vita_clang.mk)
VITASDK ?= /opt/vitasdk
VSYS    := $(VITASDK)/arm-vita-eabi
GXXV    := 15.2.0
# (the VitaSDK flags of one/vita/vita_clang.mk, except the two ABI choices the game's build overrides: plain char is unsigned and
# enums are 4 bytes, np/Makefile TARGET=vita; the SDK-facing units must agree with the game's objects on struct layouts.
# The vgx library (native-vita.mk) keeps -fsigned-char -fshort-enums: its interface is plain C ints, nothing of that crosses.)
VTARGET := --target=armv7a-none-eabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb \
           -funsigned-char -fno-short-enums -femulated-tls -fno-pic -munaligned-access \
           -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE -Wno-builtin-macro-redefined \
           -U__WINT_TYPE__ '-D__WINT_TYPE__=unsigned int' \
           -U__INT_FAST8_TYPE__ -D__INT_FAST8_TYPE__=int -U__INT_FAST16_TYPE__ -D__INT_FAST16_TYPE__=int \
           -U__UINT_FAST8_TYPE__ '-D__UINT_FAST8_TYPE__=unsigned int' -U__UINT_FAST16_TYPE__ '-D__UINT_FAST16_TYPE__=unsigned int' \
           -nostdlibinc
VCXXSYS := -isystem $(VSYS)/include/c++/$(GXXV) -isystem $(VSYS)/include/c++/$(GXXV)/arm-vita-eabi -isystem $(VSYS)/include/c++/$(GXXV)/backward -isystem $(VSYS)/include
VFLAGS  := -std=gnu++17 -O2 -w -pipe $(VTARGET) $(VCXXSYS) -I$(ROOT)/src -I$(VGX)/native
VOUT    := $(OUT)/vita
VOBJS   := $(patsubst %,$(VOUT)/%.o,$(SDK_SRC) $(STD_SRC))
.PHONY: glue-vita
glue-vita: $(VOUT)/libnpgx-vita.a

$(VOUT)/gx_selfcheck.o: $(ROOT)/src/gx_selfcheck.cpp
	@mkdir -p $(dir $@)
	@echo VCXX $(notdir $<)
	@clang++ $(VFLAGS) -I$(ROOT)/include -I$(VGX)/src -c $< -o $@
$(VOUT)/gx_copytrack.o: $(ROOT)/src/gx_copytrack.cpp
	@mkdir -p $(dir $@)
	@echo VCXX $(notdir $<)
	@clang++ $(VFLAGS) -I$(ROOT)/include -c $< -o $@
$(VOUT)/%.o: $(ROOT)/src/%.cpp $(wildcard $(ROOT)/include/*.h) $(wildcard $(ROOT)/include/revolution/gx/*.h)
	@mkdir -p $(dir $@)
	@echo VCXX $(notdir $<)
	@clang++ $(VFLAGS) $(NPDEFS) -DNP_TARGET_VITA=1 $(NPINC) -fshort-wchar -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -include np/compat.h -std=gnu++11 -fno-exceptions -fno-rtti -Wno-macro-redefined -Wno-unknown-pragmas -c $< -o $@
$(VOUT)/libnpgx-vita.a: $(VOBJS)
	@rm -f $@
	@$(VITASDK)/bin/arm-vita-eabi-gcc-ar rcs $@ $(VOBJS)
	@echo "built $@"
