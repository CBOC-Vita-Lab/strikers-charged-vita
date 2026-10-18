# Native port build (NPB).  Plain GNU make.
#
#   make TARGET=host32 -j2            # x86 Linux 32-bit (clang -m32)       -> $(OUT)/np-host32
#   make TARGET=armhf  -j2            # Cortex-A9 Linux (clang, qemu-arm)   -> $(OUT)/np-armhf
#   make TARGET=vita   -j2 syntax     # VitaSDK gcc, compile check only
#   make TARGET=vita   -j2 WITH_SDK=1 WITH_GX=1 vpk   # the Vita package (NPV2: platform/vita/Makefile.vpk does the link and packing)
#        ... LTO=thin   ThinLTO (clang bitcode + LLVM gold plugin in the GNU link, NPV3)      ... NP_PLAYER=1  the build the maintainer plays (MSCN00003, no lab)
#   make TARGET=host32 syntax         # -fsyntax-only over every TU (no objects)
#   make TARGET=host32 SUBSET=src/NL  # restrict to one directory (any prefix of a source path)
#   make TARGET=host32 stubs          # regenerate platform/stubs/sdk_stubs.c from a link attempt
# Wrap any run of more than ~10 files in tools/heavy.sh (see docs/build.md); use -j2 at most.
# Outputs go to /home/claude/msc/build/np/<target>/ only (override with BUILD_ROOT=...).

TARGET     ?= host32
.DEFAULT_GOAL := all
BUILD_ROOT ?= /home/claude/msc/build/np
OUT        := $(BUILD_ROOT)/$(TARGET)
OPT        ?= -O2
DEBUG      ?= -g1
WARN       ?= -w
SUBSET     ?= src

ARCH_ARM := -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb

ifeq ($(TARGET),host32)
  CC      := clang
  CXX     := clang++
  LD      := clang++
  TFLAGS  := -m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1
  LDTAIL  := -m32 -rdynamic -lpthread -lm -ldl
  AR      := ar
  EXTRA_CXX := -Wno-c++11-narrowing -Wno-enum-constexpr-conversion -fno-strict-return
else ifeq ($(TARGET),armhf)
  CC      := clang
  CXX     := clang++
  LD      := arm-linux-gnueabihf-g++
  TFLAGS  := --target=arm-linux-gnueabihf $(ARCH_ARM) -DNP_TARGET_ARMHF=1
  LDTAIL  := -rdynamic -lpthread -lm -ldl
  AR      := arm-linux-gnueabihf-ar
  EXTRA_CXX := -Wno-c++11-narrowing -Wno-enum-constexpr-conversion -fno-strict-return
else ifeq ($(TARGET),vita)
  VITASDK ?= /opt/vitasdk
  VSYS    := $(VITASDK)/arm-vita-eabi
  LD      := $(VITASDK)/bin/arm-vita-eabi-g++
  LDTAIL  := -lm
  AR      := $(VITASDK)/bin/arm-vita-eabi-ar
  # VCOMP=clang (default, NPV2): the compiler of the host32 and armhf builds, so the console build is the code the armhf proxy
  # measures and that ran 2885 frames under qemu; flags as platform/gx (VitaSDK ABI through clang, minus the two ABI choices of the
  # contract). VCOMP=gcc: VitaSDK GCC 15.2 (the first Vita build; GCC's optimiser exploits more of the decomp's undefined behaviour:
  # see docs/vita.md "GCC").
  VCOMP   ?= clang
  ifeq ($(VCOMP),clang)
    CC      := clang
    CXX     := clang++
    TFLAGS  := --target=armv7a-none-eabihf $(ARCH_ARM) -femulated-tls -fno-pic -munaligned-access \
               -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE -Wno-builtin-macro-redefined \
               -U__WINT_TYPE__ '-D__WINT_TYPE__=unsigned int' \
               -U__INT_FAST8_TYPE__ -D__INT_FAST8_TYPE__=int -U__INT_FAST16_TYPE__ -D__INT_FAST16_TYPE__=int \
               -U__UINT_FAST8_TYPE__ '-D__UINT_FAST8_TYPE__=unsigned int' -U__UINT_FAST16_TYPE__ '-D__UINT_FAST16_TYPE__=unsigned int' \
               -nostdlibinc -DNP_TARGET_VITA=1
    # C++ headers before the C ones (#include_next), C files only the C ones
    EXTRA_C   := -isystem $(VSYS)/include
    EXTRA_CXX := -isystem $(VSYS)/include/c++/15.2.0 -isystem $(VSYS)/include/c++/15.2.0/arm-vita-eabi \
                 -isystem $(VSYS)/include/c++/15.2.0/backward -isystem $(VSYS)/include \
                 -Wno-c++11-narrowing -Wno-enum-constexpr-conversion -fno-strict-return
  else
    CC      := $(VITASDK)/bin/arm-vita-eabi-gcc
    CXX     := $(VITASDK)/bin/arm-vita-eabi-g++
    # -fno-devirtualize: GCC turns a virtual call whose target it cannot find in the DECLARED type of the object into
    # __builtin_unreachable(), and RegistryOwner::Relocate (packed registries are built in place over a storage class) lost
    # everything after its first GetValueSlot() call: the first Vita3K run ran off the end of the function into its literal pool.
    # -fno-lifetime-dse: stores before a placement-new constructor are not dead (disc data is loaded in place).
    TFLAGS  := $(ARCH_ARM) -fno-aggressive-loop-optimizations -fno-devirtualize -fno-lifetime-dse -DNP_TARGET_VITA=1
    EXTRA_CXX := -Wno-narrowing
  endif
else
  $(error unknown TARGET $(TARGET))
endif

INCLUDES := -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include \
            -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include
# NP_PARALLEL=1: the game's PortParallelFor() job sites (RenderSnapshot Grab/Blend) use the job pool of platform/vita/src/np_port_parallel.cpp
NPDEF_PAR := $(if $(filter 1,$(NP_PARALLEL)),-DNP_HAVE_PARALLEL=1,)
DEFS     := $(NPDEF_PAR) -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1

# NPR's renderer bridge (platform/gx): the game sources carry its patches (np_gx.h, NP_GX_SETARRAY_STREAM), so the
# wrapper header platform/gx/include/revolution/gx/GXHardware.h (WGPIPE stand-in) must be FIRST on the include path in
# every build. Without WITH_GX=1 the glue functions it refers to are simply the generated stubs; WITH_GX=1 builds and
# links the real glue + vgx (libnpgx.a, libvgx-native.a).
INCLUDES := -Iplatform/gx/include $(INCLUDES)
ifeq ($(WITH_GX),1)
include platform/gx/gx.mk
endif

# CCTOOL=gcc (armhf only, NPP): compile the GAME objects (src/, platform/compat) with the container's arm-linux-gnueabihf GCC 13 and the
# flags of the Vita build, as a proxy for VitaSDK's GCC 15.2 (the SDK / GX / platform libraries stay clang objects; the link is the same)
GTF      := $(TFLAGS)
ifeq ($(CCTOOL),gcc)
ifneq ($(TARGET),armhf)
$(error CCTOOL=gcc is for TARGET=armhf)
endif
CC       := arm-linux-gnueabihf-gcc
CXX      := arm-linux-gnueabihf-g++
GTF      := $(ARCH_ARM) -fno-aggressive-loop-optimizations -DNP_TARGET_ARMHF=1
EXTRA_CXX := -Wno-narrowing
endif
COMMON   := $(GTF) $(DEFS) $(INCLUDES) -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar \
            -fno-delete-null-pointer-checks $(WARN) -include np/compat.h
# Code-generation flags that are not part of the ABI contract (NPP): -fno-math-errno turns sqrtf & co. into the VFP instruction without the
# errno fallback call (nlSqrt 52 -> 40, nlRecipSqrt 51 -> 37 static instructions; the game never reads errno). PERF= switches them off.
PERF     ?= -fno-math-errno
# NONEON=1: the C code of the SDK's PSMTXConcat / PSMTX44Concat (A/B of the NEON versions)
PERFX    := $(PERF) $(if $(filter 1,$(NONEON)),-DNP_NO_NEON,)
COMMON   += $(PERFX)
# AUTOINIT=zero|pattern (NPB3): -ftrivial-auto-var-init for the game and compat objects (clang >= 8, GCC >= 12 incl. VitaSDK's 15.2). zero makes every
# automatic variable start as 0, i.e. no code can depend on stack garbage (and stack garbage is process layout: the simulation differs between
# stack positions otherwise); pattern is the opposite probe (0xAA bytes; tools/det/ compares the two). Empty (default): the compiler's behaviour.
COMMON   += $(if $(AUTOINIT),-ftrivial-auto-var-init=$(AUTOINIT),)
# HEAPZERO=1 (VRS): every game-heap allocation starts zeroed (src/NL/nlMemory.cpp; NP_HEAP_POISON=off in the environment switches it off again).
COMMON   += $(if $(filter 1,$(HEAPZERO)),-DNP_HEAP_ZERO_DEFAULT=1,)
# NOPIC=1 (armhf / vita-like): game and SDK objects compiled position-dependent and linked -no-pie, as the Vita's fixed-address ELF is;
# the PIE default of the container's compilers adds a GOT load to every access to a global defined in another TU.
ifeq ($(NOPIC),1)
COMMON   += -fno-pic -fno-pie
endif
CXXFLAGS := $(COMMON) -std=gnu++11 -fno-exceptions -fno-rtti $(EXTRA_CXX) $(OPT) $(DEBUG)
CFLAGS   := $(COMMON) -std=gnu11 $(EXTRA_C) $(OPT) $(DEBUG)
# LTO=thin|full (clang targets, armhf/host32): game and platform/compat objects are compiled to bitcode and linked with lld
# (NPP, docs/wave2-NPP.md). The SDK/GX/platform libraries stay ordinary objects (their archives are built by their own makefiles).
# NPP2: ThinLTO is the DEFAULT of the optimised armhf build (-O2 / -O3; it takes 8-12 % off the game thread in every scene,
# docs/wave3-NPP2.md); LTO=none switches it off, host32 (the fast simulation build) and -O1 stay as they were.
ifeq ($(TARGET)-$(origin LTO),armhf-undefined)
ifneq ($(filter -O2 -O3,$(OPT)),)
LTO := thin
endif
endif
ifeq ($(NOPIC),1)
LDTAIL   := $(LDTAIL) -no-pie
endif
ifneq ($(filter thin full,$(LTO)),)
LTOFLAG  := -flto=$(LTO)
CXXFLAGS += $(LTOFLAG)
CFLAGS   += $(LTOFLAG)
ifeq ($(TARGET),vita)
# TARGET=vita (NPV3): the game objects are bitcode; the link stays VitaSDK's GNU ld (the image layout vita-elf-create / the loader know) and
# LLVM's gold plugin does the ThinLTO code generation inside it (platform/vita/Makefile.vpk NP_CLANG_LTO; docs/wave3-NPV3.md).
# GCC's own -flto is NP_LTO=1 of vita.mk and needs VCOMP=gcc.
ifneq ($(VCOMP),clang)
$(error LTO=thin|full on the Vita needs VCOMP=clang (GCC: NP_LTO=1))
endif
else
LLDBIN   ?= $(BUILD_ROOT)/lldbin
LD       := $(LD) -B$(LLDBIN)   # a dir whose "ld" is ld.lld (the cross g++ driver does not find ld.lld by itself); made on demand
LDTAIL   := $(LDTAIL) -Wl,--thinlto-jobs=2 $(LTOLINK)
endif
endif
DEPFLAGS  = -MMD -MP -MF $(@:.o=.d)
# clang folds constructor C1 into C2 and then emits no C1 symbol where the inline definition is only used here;
# the instances TU must define C1 because other TUs reference it (gcc always emits both). Compiler of the game objects: clang, except the
# Vita target (VCOMP) and CCTOOL=gcc (NPP: armhf objects with GCC)
ifneq ($(filter-out gcc,$(if $(filter vita,$(TARGET)),$(VCOMP),$(if $(filter gcc,$(CCTOOL)),gcc,clang))),)
$(OUT)/pobj/platform/compat/np_inline_instances.cpp.o: EXTRA_DEFS := -Xclang -mno-constructor-aliases
else
# GCC: at -O2 it emits some of these inline functions only as LOCAL IPA-SRA clones (`GetActiveSlide() [clone .isra.0]`), which
# satisfy no other TU; -O0 (after the -O2 of CXXFLAGS) keeps every function whole (the calls in np_keep_inline_functions are real calls)
$(OUT)/pobj/platform/compat/np_inline_instances.cpp.o: EXTRA_DEFS := -O0
endif
# glxSwapPre is wrapped by the linker (-Wl,--wrap=_Z10glxSwapPreb: the harness counts presented frames there). A bitcode definition could be
# inlined into its caller before the linker redirects the call, so on the Vita (GNU ld + the LLVM plugin, which does not know --wrap) this one
# TU is an ordinary object.
ifneq ($(LTOFLAG),)
ifeq ($(TARGET),vita)
$(OUT)/obj/src/NL/glx/glxSwap.cpp.o: CXXFLAGS := $(filter-out $(LTOFLAG),$(CXXFLAGS))
endif
endif
# the game's own main() becomes np_game_main(); platform/compat/np_main.cpp provides main()
$(OUT)/obj/src/Game/main.cpp.o $(OUT)/syn/src/Game/main.cpp.ok: EXTRA_DEFS := -Dmain=np_game_main

# TUs that are #include'd by another TU (e.g. Foo_interp.cpp) are not compiled on their own.
INCLUDED_TUS := $(shell grep -rhoE '\#[[:space:]]*include[[:space:]]+"src/[^"]*\.cpp"' src include | sed -E 's/.*"(.*)"/\1/')
SRCS_ALL := $(filter-out $(INCLUDED_TUS),$(shell find src -name '*.cpp' -o -name '*.c' | sort))
# Metrowerks' plain char is unsigned; the decomp builds these TUs with -char signed (tools/char_signed.txt)
CSIGNED  := $(shell cat tools/char_signed.txt)
charflag  = $(if $(filter $(addsuffix %,$(CSIGNED)),$1),-fsigned-char,-funsigned-char)
SRCS     := $(filter $(SUBSET)%,$(SRCS_ALL))
OBJS     := $(patsubst %,$(OUT)/obj/%.o,$(SRCS))
SYNS     := $(patsubst %,$(OUT)/syn/%.ok,$(SRCS))
STUBS    := platform/stubs/sdk_stubs.c
# platform code compiled into every executable; other workstreams append their directories here
# (or pass PLAT_DIRS="... platform/sdk ...") as their branches are merged.
PLAT_DIRS ?= platform/compat platform/assets platform/sdk/net platform/sdk/input platform/sdk/save
PLATSRC  := $(shell find $(PLAT_DIRS) -name '*.c' -o -name '*.cpp' 2>/dev/null | sort) platform/stubs/np_stub_support.c ref/Runtime/NMWException.cpp \
            $(if $(filter vita,$(TARGET)),,platform/harness/src/np_host_main.cpp)
ifneq ($(WITH_SDK),1)
PLATSRC  := $(filter-out platform/compat/np_sdk_boot.cpp,$(PLATSRC))
endif
PLATOBJ  := $(patsubst %,$(OUT)/pobj/%.o,$(PLATSRC))
STUBOBJ  := $(OUT)/pobj/$(STUBS).o
EXESUF   ?=
LDEXTRA  ?=
EXE      := $(OUT)/np-$(TARGET)$(EXESUF)

.PHONY: all syntax objs clean-out stubs failed
all: $(EXE)
objs: $(OBJS)
syntax: $(SYNS)

$(OUT)/obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@) $(OUT)/log/$(dir $<)
	@$(CXX) $(CXXFLAGS) $(call charflag,$<) $(EXTRA_DEFS) $(DEPFLAGS) -c $< -o $@ 2> $(OUT)/log/$<.err || { echo "FAIL $<"; head -c 3000 $(OUT)/log/$<.err | grep -m3 error; rm -f $@; exit 1; }
$(OUT)/obj/%.c.o: %.c
	@mkdir -p $(dir $@) $(OUT)/log/$(dir $<)
	@$(CC) $(CFLAGS) $(call charflag,$<) $(EXTRA_DEFS) $(DEPFLAGS) -c $< -o $@ 2> $(OUT)/log/$<.err || { echo "FAIL $<"; head -c 3000 $(OUT)/log/$<.err | grep -m3 error; rm -f $@; exit 1; }

$(OUT)/syn/%.cpp.ok: %.cpp
	@mkdir -p $(dir $@) $(OUT)/log/$(dir $<)
	@$(CXX) $(CXXFLAGS) $(call charflag,$<) $(EXTRA_DEFS) -fsyntax-only -c $< 2> $(OUT)/log/$<.err && touch $@ || { echo "FAIL $<"; grep -m3 error $(OUT)/log/$<.err; exit 1; }
$(OUT)/syn/%.c.ok: %.c
	@mkdir -p $(dir $@) $(OUT)/log/$(dir $<)
	@$(CC) $(CFLAGS) $(call charflag,$<) $(EXTRA_DEFS) -fsyntax-only -c $< 2> $(OUT)/log/$<.err && touch $@ || { echo "FAIL $<"; grep -m3 error $(OUT)/log/$<.err; exit 1; }

$(OUT)/pobj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -funsigned-char $(EXTRA_DEFS) $(DEPFLAGS) -c $< -o $@
$(OUT)/pobj/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -funsigned-char $(DEPFLAGS) -c $< -o $@

# The stubs go into an archive that is searched LAST: a real (strong) definition from any other object or
# library wins because the stub member is only pulled in for symbols that are still undefined.
STUBLIB  := $(OUT)/libnpstubs.a
ifneq ($(LTOFLAG),)
$(STUBOBJ): CFLAGS := $(filter-out $(LTOFLAG),$(CFLAGS))
endif
$(STUBLIB): $(STUBOBJ)
	@rm -f $@
	$(AR) rcs $@ $^

# extra libraries of merged workstreams
EXTRALIBS :=

# WITH_SDK=1: link NPS's platform/sdk (libnpsdk.a) in front of the stubs. Its own Makefile builds it, but with
# OUR ABI flags (COMMON/ARCH are overridden on the command line) so that game and SDK layer agree on layouts.
ifeq ($(WITH_SDK),1)
SDKLIB     := $(OUT)/sdk/libnpsdk.a
SDK_COMMON := $(PERFX) $(if $(filter 1,$(NOPIC)),-fno-pic -fno-pie,) -fno-strict-aliasing -fwrapv -fshort-wchar -funsigned-char -fno-short-enums -fno-common \
              -ffunction-sections -fdata-sections $(if $(filter vita,$(TARGET)),-fno-aggressive-loop-optimizations,) $(OPT) $(DEBUG)
SDK_ARCH   := $(if $(filter vita,$(TARGET)),$(ARCH_ARM) -fno-aggressive-loop-optimizations -fno-devirtualize -fno-lifetime-dse -DNP_TARGET_VITA=1,$(TFLAGS)) -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1
CXXFLAGS   += -Iplatform/sdk/include
CFLAGS     += -Iplatform/sdk/include
$(SDKLIB): FORCE
	$(MAKE) -C platform/sdk T=$(TARGET) B=$(abspath $(OUT)/sdk) COMMON="$(SDK_COMMON)" ARCH="$(SDK_ARCH)"
EXTRALIBS  += $(SDKLIB)
FORCE:
endif

ifeq ($(WITH_GX),1)
EXTRALIBS += $(GX_LIBS)
LDTAIL    := $(LDTAIL) $(GX_LDLIBS)
endif

# WITH_PLAT=1: NPV's host platform library (libnpplat.a: arenas/paths/audio queue/input stubs, the nav-script harness and
# its frame hook, job pool). The harness counts presented frames by wrapping glxSwapPre (called once per GXCopyDisp).
ifeq ($(WITH_PLAT),1)
PLATLIB := $(OUT)/plat/$(TARGET)/libnpplat.a
$(PLATLIB): PLAT_FORCE
	$(MAKE) -C platform/vita TARGET=$(TARGET) OUT=$(abspath $(OUT))/plat hostlib
PLAT_FORCE:
EXTRALIBS += $(PLATLIB)
LDTAIL    := $(LDTAIL) -Wl,--wrap=_Z10glxSwapPreb -Wl,-u,NpFsFork   # (the game's references to the frame-send worker are weak: -u pulls it from the archive)
endif

$(LLDBIN)/ld:
	@mkdir -p $(LLDBIN) && ln -sf "$$(command -v ld.lld)" $@

$(EXE): $(OBJS) $(PLATOBJ) $(STUBLIB) $(EXTRALIBS) $(if $(LTOFLAG),| $(LLDBIN)/ld)
	$(LD) -o $@ $(OBJS) $(PLATOBJ) $(EXTRALIBS) $(STUBLIB) $(LDTAIL) $(LDEXTRA) 2> $(OUT)/link.err || { grep -c 'undefined reference' $(OUT)/link.err | sed 's/^/unresolved references: /'; grep -v 'undefined reference' $(OUT)/link.err | head -20; false; }

# Link every game object (without the generated stubs) and turn the unresolved symbols into stubs.
stubs: $(OBJS) $(PLATOBJ)
	-$(LD) -Wl,--no-demangle -o /dev/null $(OBJS) $(PLATOBJ) $(LDTAIL) 2> $(OUT)/link0.err
	$(LD) -r -nostdlib -o $(OUT)/all.ro $(OBJS) $(PLATOBJ) $(if $(filter host32,$(TARGET)),-m32,)
	readelf -rW $(OUT)/all.ro > $(OUT)/relocs.txt
	python3 tools/gen_stubs.py $(OUT)/link0.err $(OUT)/relocs.txt $(STUBS) docs/sdk-symbols.tsv tools/stub_overrides.txt
	rm -f $(OUT)/all.ro

# ---- TARGET=vita: the package. The game's objects, the stubs, NPS's SDK layer and NPR's renderer libraries are built by this
# makefile with the ABI contract; the platform layer (platform/vita, platform/harness), the link, the conversion to a Vita
# executable and the VPK are platform/vita/Makefile.vpk's. The link line of the game is the one of the host builds
# (objects, then libnpsdk, libnpgx, libvgx, the stub archive last) plus vitaGL and the Sce stubs (GX_LDLIBS).
#   make TARGET=vita -j2 WITH_SDK=1 WITH_GX=1 vpk [NP_RELEASE=1] [LAB=0] [NP_SEED_DIR=...]  -> $(OUT)/pkg/np/mscharged-np.vpk
# (pkg/np/mscharged-np.elf is the linked image, with its .map, for symbolising crash dumps.)
ifeq ($(TARGET),vita)
VPK_OUT   ?= $(OUT)/pkg
VPK_WRAPS ?= -Wl,--wrap=_Z10glxSwapPreb
.PHONY: vpk
vpk: $(OBJS) $(PLATOBJ) $(STUBLIB) $(EXTRALIBS)
	@# MAKEFLAGS is dropped: it would carry TARGET=vita into Makefile.vpk (vita_clang.mk has a variable of that name)
	env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL $(MAKE) -f platform/vita/Makefile.vpk vpk NP_VBUILD=$(abspath $(VPK_OUT)) \
	  NP_GAME_OBJS='$(OBJS) $(PLATOBJ)' NP_GAME_LIBS='$(EXTRALIBS) $(STUBLIB) $(GX_LDLIBS)' NP_EXTRA_WRAPS='$(VPK_WRAPS)' \
	  $(if $(NP_RELEASE),NP_RELEASE=$(NP_RELEASE)) $(if $(NP_PLAYER),NP_PLAYER=$(NP_PLAYER)) $(if $(LAB),LAB=$(LAB)) $(if $(NP_TITLE_ID),NP_TITLE_ID=$(NP_TITLE_ID)) \
	  $(if $(NP_SEED_DIR),NP_SEED_DIR=$(abspath $(NP_SEED_DIR))) $(if $(NP_SHADER_SEED),NP_SHADER_SEED=$(abspath $(NP_SHADER_SEED))) $(if $(NP_LTO),NP_LTO=$(NP_LTO)) $(if $(NP_COMPILER),NP_COMPILER=$(NP_COMPILER)) $(if $(LTOFLAG),NP_CLANG_LTO=$(LTO))
endif

-include $(OBJS:.o=.d) $(PLATOBJ:.o=.d) $(STUBOBJ:.o=.d)
