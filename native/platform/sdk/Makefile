# platform/sdk: the Wii SDK layer of the native port.
#
#   make T=host32 [-j2]          libnpsdk.a for x86 Linux, 32-bit (ILP32 like the console)
#   make T=armhf  [-j2]          same for Cortex-A9 Linux (runs under qemu-arm)
#   make T=vita   [-j2]          same with VitaSDK
#   make T=host32 check          builds and runs the unit tests (armhf: under qemu-arm)
#
# Output: $(B) = /home/claude/msc/build/np/NPS/<T>  (libnpsdk.a, obj/, tests/, sdkinc/)
# Heavy jobs go through /home/claude/msc/tools/heavy.sh (see the notes of the NPS agent).
# NPB integration: include sources.mk (the source lists) and the flags below in the main build.

T ?= host32
HERE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NP ?= $(abspath $(HERE)/../..)
REF ?= $(NP)/ref/RVL_SDK
B ?= /home/claude/msc/build/np/NPS/$(T)$(if $(PLATFAKE),vf)
SDKINC := $(B)/sdkinc

include $(HERE)/sources.mk

# The ABI contract of docs/build.md (flags every object sharing the SDK headers must agree on):
# -funsigned-char, -fno-short-enums, -fshort-wchar, -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks, and
# no -malign-double on host32.
# COMMON and ARCH may be overridden on the command line (the main build does, with its own ABI flags); what this
# library needs in any case (defines, the GCC loop-bound switch) lives in PDEF, which is never overridden.
COMMON := -O2 -g -fno-strict-aliasing -fwrapv -fshort-wchar -funsigned-char -fno-short-enums \
          -fno-delete-null-pointer-checks -fno-common -ffunction-sections -fdata-sections
PDEF_ALL := -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1
ifeq ($(T),host32)
  CC := gcc
  CXX := g++
  ARCH := -m32 -msse2 -mfpmath=sse -idirafter /usr/include/x86_64-linux-gnu
  PLAT := $(HERE)/host/np_plat_host.cpp
  LIBS := -lpthread -lm
  RUN :=
  # 64-bit file offsets and inode numbers: a 32-bit stat()/readdir() fails with EOVERFLOW on file systems whose inode
  # numbers exceed 32 bits (overlay and network mounts, qemu-user on a 64-bit host)
  # GCC infers "this loop runs at most once" from one-element arrays used as flexible arrays (the SDK's
  # `u8 buffer[1]` in NETHMACContext): MWCC has no such rule. (In PDEF so that an ARCH override keeps it.)
  PDEF := -DNP_HOST=1 -DNP_TARGET_HOST32=1 -D_FILE_OFFSET_BITS=64 -fno-aggressive-loop-optimizations
else ifeq ($(T),armhf)
  CC := clang --target=arm-linux-gnueabihf
  CXX := clang++ --target=arm-linux-gnueabihf
  ARCH := -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
  PLAT := $(HERE)/host/np_plat_host.cpp
  LIBS := -lpthread -lm
  RUN := /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf
  PDEF := -DNP_HOST=1 -DNP_TARGET_ARMHF=1 -D_FILE_OFFSET_BITS=64
else ifeq ($(T),vita)
  VITASDK ?= /opt/vitasdk
  export VITASDK
  CC := $(VITASDK)/bin/arm-vita-eabi-gcc
  CXX := $(VITASDK)/bin/arm-vita-eabi-g++
  ARCH := -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
  PLAT := $(HERE)/vita/np_plat_vita.cpp
  # what vita/np_plat_vita.cpp calls (the audio sink and the input source are NPV's: sceAudio, sceCtrl, sceTouch)
  LIBS := -lSceLibKernel_stub -lSceKernelThreadMgr_stub -lSceSysmem_stub -lSceIofilemgr_stub -lpthread -lm
  RUN :=
  PDEF := -DNP_VITA=1 -DNP_TARGET_VITA=1 -fno-aggressive-loop-optimizations
else
  $(error T must be host32, armhf or vita)
endif
PDEF += $(PDEF_ALL)
ifeq ($(T),armhf)
  LD := arm-linux-gnueabihf-g++
else
  LD := $(CXX)
endif

INCS := -I$(HERE)/shim -I$(HERE)/shim/internal -I$(NP)/include -I$(HERE)/include -I$(HERE)/src -I$(SDKINC) -include np/compat.h
CFLAGS   := $(ARCH) $(COMMON) -std=gnu11 -Wall -Wno-unknown-pragmas -Wno-parentheses -Wno-unused-variable -Wno-format-truncation -Wno-unused-function $(PDEF) $(INCS)
CXXFLAGS := $(ARCH) $(COMMON) -std=gnu++11 -fno-exceptions -fno-rtti -Wall -Wno-unknown-pragmas -Wno-parentheses -Wno-format-truncation -Wno-unused-function $(PDEF) $(INCS)
CXX17FLAGS := $(subst -std=gnu++11,-std=gnu++17,$(CXXFLAGS)) -Wno-unused-function
# SDK sources taken over unchanged from np/ref: their warnings are not ours.
REFFLAGS := $(ARCH) $(COMMON) -std=gnu11 -w $(PDEF) $(INCS) -include np_ref_prelude.h

# PLATFAKE=1 (host32 only): the Vita platform file over a pthread fake of the sce kernel calls (tests/fake_sce), so that
# the thread/semaphore/mutex/memory logic of vita/np_plat_vita.cpp runs in the unit tests. Output: ...NPS/host32vf.
ifeq ($(PLATFAKE),1)
  ifneq ($(T),host32)
    $(error PLATFAKE=1 needs T=host32)
  endif
  PLAT := $(HERE)/vita/np_plat_vita.cpp
  PDEF += -DNP_FAKE_SCE=1
  INCS += -I$(HERE)/tests/fake_sce
  CFLAGS += -I$(HERE)/tests/fake_sce
  CXXFLAGS += -DNP_FAKE_SCE=1 -I$(HERE)/tests/fake_sce
  CXX17FLAGS += -I$(HERE)/tests/fake_sce
endif
PLATDIR := $(dir $(PLAT))
OBJ := $(B)/obj
OWN_OBJS := $(patsubst %,$(OBJ)/src/%.o,$(SRC_OWN)) $(patsubst %,$(OBJ)/plat/%.o,$(notdir $(basename $(PLAT)))) $(if $(PLATFAKE),$(OBJ)/plat/fake_sce.o)
REF_OBJS := $(patsubst %,$(OBJ)/ref/%.o,$(REF_SRC))
REF_CXX_OBJS := $(patsubst %,$(OBJ)/ref/%.o,$(REF_CXX_SRC))
ALL_OBJS := $(OWN_OBJS) $(REF_OBJS) $(REF_CXX_OBJS)
LIB := $(B)/libnpsdk.a

all: $(LIB)

$(SDKINC)/.stamp: $(HERE)/tools/mkinc.sh $(wildcard $(HERE)/shim/revolution/*/*.h)
	$(HERE)/tools/mkinc.sh $(SDKINC)

$(OBJ)/src/%.c.o: $(HERE)/src/%.c $(SDKINC)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@
# the AX mixer (src/ax) is C++17 and includes no SDK header
$(OBJ)/src/ax/%.cpp.o: $(HERE)/src/ax/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXX17FLAGS) -MMD -c $< -o $@
$(OBJ)/src/ax/%.c.o: $(HERE)/src/ax/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@
$(OBJ)/src/%.cpp.o: $(HERE)/src/%.cpp $(SDKINC)/.stamp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@
$(OBJ)/plat/%.o: $(PLATDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@
$(OBJ)/plat/fake_sce.o: $(HERE)/tests/fake_sce/fake_sce.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@
# ref sources with a sed patch (patches/<path>.sed: endianness fixes) are compiled from the patched copy
PATCHED := $(patsubst $(HERE)/patches/%.sed,%,$(shell find $(HERE)/patches -name '*.sed' 2>/dev/null))
$(OBJ)/gen/%.c: $(REF)/%.c $(HERE)/patches/%.c.sed
	@mkdir -p $(dir $@)
	sed -f $(HERE)/patches/$*.c.sed $< > $@
$(foreach f,$(PATCHED),$(OBJ)/ref/$(f).o): $(OBJ)/ref/%.o: $(OBJ)/gen/%
	@mkdir -p $(dir $@)
	$(CC) $(REFFLAGS) -I$(REF)/$(dir $*) -MMD -c $< -o $@
# the game's movie player (sed-patched C++ of the decomp's SDK tree, see sources.mk)
$(OBJ)/gen/%.cpp: $(REF)/%.cpp $(HERE)/patches/%.cpp.sed
	@mkdir -p $(dir $@)
	sed -f $(HERE)/patches/$*.cpp.sed $< > $@
$(REF_CXX_OBJS): $(OBJ)/ref/%.o: $(OBJ)/gen/% $(SDKINC)/.stamp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fpermissive -I$(REF)/$(dir $*) -MMD -c $< -o $@
$(OBJ)/ref/%.c.o: $(REF)/%.c $(SDKINC)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(REFFLAGS) -MMD -c $< -o $@


$(LIB): $(ALL_OBJS)
	@rm -f $@
	$(if $(filter vita,$(T)),$(VITASDK)/bin/arm-vita-eabi-ar,$(if $(filter armhf,$(T)),arm-linux-gnueabihf-ar,ar)) rcs $@ $^

# ---- unit tests (host32 / armhf) ---------------------------------------------------------------
TESTS := $(basename $(notdir $(wildcard $(HERE)/tests/test_*.cpp)))
TEST_BINS := $(patsubst %,$(B)/tests/%,$(TESTS))
$(B)/tests/%: $(HERE)/tests/%.cpp $(LIB) $(SDKINC)/.stamp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@.o -c
	$(LD) $(ARCH) $@.o $(LIB) $(LIBS) -o $@
# the game's own movie player on the SDK layer (needs the game's include path for NL headers)
$(B)/tests/test_thp_player: $(HERE)/tests/game/thp_player.cpp $(LIB) $(SDKINC)/.stamp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fpermissive $(HERE)/tests/game/thp_player.cpp -c -o $@.o
	$(LD) $(ARCH) $@.o $(LIB) $(LIBS) -o $@
TEST_BINS += $(B)/tests/test_thp_player
check: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; $(RUN) $$t; done; \
	echo "== $(B)/tests/test_input_default (NP_PAD_PRESENT=0)"; NP_PAD_PRESENT=0 $(RUN) $(B)/tests/test_input_default off; \
	echo "all tests passed ($(T))"

clean:
	rm -rf $(B)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
.PHONY: all check clean
