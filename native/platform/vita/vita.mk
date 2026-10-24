# vita.mk: the Vita target of the native port: compiler choice and flags, link libraries and wraps,
# package identity. Include it from the game's Makefile (NPB) or use Makefile.vpk, which does.
#
#   export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
#
# Compiler (NP_COMPILER):
#   gcc    VitaSDK's GCC 15.2: the PRODUCTION compiler (LTO and PGO work only here: np/docs/vita.md
#          "LTO and PGO"); about 100 s for a large translation unit of the recompiled game, so an
#          iteration build of the game uses clang.
#   clang  clang 18 with the flags of vita_clang.mk (what makes its objects link with VitaSDK's GCC
#          libraries: signed char, short enums, emulated TLS, GCC's wint_t...). Fast compiles; no LTO.
# Never mix the two in one link with -flto.
#
# Variables this file defines for the game's own rules:
#   NP_VCC / NP_VCXX         compiler drivers
#   NP_VARCH                 target flags (cpu, fpu, float abi, thumb, ABI flags for clang)
#   NP_VDEFS                 -D flags every Vita object takes (-D__vita__ is predefined by GCC)
#   NP_VCXXSYS               clang only: system include paths
#   NP_VOPT_BASE / NP_VOPT_LTO / NP_VPGO_GEN / NP_VPGO_USE   optimisation flag sets (see below)
#   NP_VLIBS / NP_VWRAPS     link libraries and --wrap options (before the game's own)
#   NP_VLINK                 the link driver
# and the package identity (NP_TITLE_ID etc.) used by Makefile.vpk.
ifndef NP_VITA_MK
NP_VITA_MK := 1
NPV_DIR  := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
VITASDK  ?= /opt/vitasdk
VBIN     := $(VITASDK)/bin
VSYS     := $(VITASDK)/arm-vita-eabi
NP_COMPILER ?= gcc

# ABI flags of NPB's contract (docs/build.md): 4-byte enums, 16-bit wchar_t, unsigned plain char (also the
# ARM default). Same as the game objects, so the link carries no ABI-attribute mismatch warnings. This layer
# uses neither wide characters nor short enums, and its headers use fixed-width types only.
NP_VABI := -fno-short-enums -fshort-wchar -funsigned-char
NP_VARCH_ISA := -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb $(NP_VABI)
ifeq ($(NP_COMPILER),gcc)
NP_VCC   := $(VBIN)/arm-vita-eabi-gcc
NP_VCXX  := $(VBIN)/arm-vita-eabi-g++
NP_VARCH := $(NP_VARCH_ISA)
NP_VCXXSYS :=
else ifeq ($(NP_COMPILER),clang)
include $(NPV_DIR)/vita_clang.mk
NP_VCC   := clang
NP_VCXX  := clang++
NP_VARCH := $(TARGET) $(NP_VABI)
NP_VCXXSYS := $(CXXSYS)
else
$(error NP_COMPILER must be gcc or clang)
endif
NP_VLINK := $(VBIN)/arm-vita-eabi-g++

# kubridge's header (no package installed here; no licence file in the clone, so it is used in place,
# not copied). Only the weak crash-handler hook uses it (np_vita_crash.cpp).
KUBRIDGE ?= /home/claude/research/vita-refs/bythos14_kubridge
NP_VINC  := -I$(KUBRIDGE)

# Package identity. Two ids, one app (W5-ONE):
#   MSCH00002  "Mario Strikers Charged"  the FINAL id: NP_RELEASE=1. It is the player build (NP_PLAYER=1 follows from it: no lab, np-player.txt,
#              the Settings button of the LiveArea) and it is the recompiled game's id, with the same data directory ux0:data/mscharged:
#              installing it REPLACES the recompiled app on the console. Only when the owner has approved that.
#   MSCN00003  "Mario Strikers Charged"  the TEST id of the player build: NP_PLAYER=1 alone. Sits next to the recompiled app. (Titled as the
#              game since 17 Oct, the maintainer: "call it the actual game"; it was "Mario Strikers Charged (native)".)
#   MSCN00001  the lab build (no flags): lab server, env.txt, nav scripts.
# Both ids can be changed from the make command line (NP_TITLE_ID=... NP_TITLE=...): `make ... NP_PLAYER=1 NP_TITLE_ID=MSCH00002` is the same package
# as NP_RELEASE=1 (an id that is not MSCN0000x gets the final icon).
NP_RELEASE ?= 0
# NP_PLAYER=1 (NPV3): the build the maintainer plays. An ordinary application: no lab server, no nav scripts, no env.txt (it reads np-player.txt when it
# exists), its own title id so that it can sit next to the lab build and the recompiled game; the save data goes to ux0:data/mscharged/np-data
# (all native builds; the first start copies the recompiled game's save there when there is none: np_vita_save.cpp), the game data is the
# recompiled build's extracted disc.
NP_PLAYER ?= $(if $(filter 1,$(NP_RELEASE)),1,0)
NP_LOGGING ?= 1
ifeq ($(NP_RELEASE),1)
NP_TITLE_ID ?= MSCH00002
NP_TITLE    ?= Mario Strikers Charged
else ifeq ($(NP_PLAYER),1)
NP_TITLE_ID ?= MSCN00003
NP_TITLE    ?= Mario Strikers Charged
else
NP_TITLE_ID ?= MSCN00001
NP_TITLE    ?= Mario Strikers Charged (native test)
endif
NP_APP_VER  ?= 01.00

# Lab mode (lab/vitalab.py over Wi-Fi: upload builds, start runs, fetch logs; see lab/README.md):
# on for test titles, off for the release build.
LAB ?= $(if $(filter 1,$(NP_RELEASE) $(NP_PLAYER)),0,1)

NP_VDEFS := -DNP_VITA=1 $(if $(filter 1,$(NP_PLAYER)),-DNP_PLAYER=1) -DNP_TITLE_ID='"$(NP_TITLE_ID)"' $(if $(filter 1,$(LAB)),-DMSC_LAB=1 '-DMSC_LAB_BUILD_ID="np-$(shell git -C $(NPV_DIR) rev-parse --short HEAD 2>/dev/null)-$(shell date +%m%d.%H%M%S)"')
NP_VDEFS += -DNP_LOGGING=$(NP_LOGGING)

# Optimisation flag sets for the game's objects (the game's Makefile picks one):
#   base   -O2, no unsafe math
#   lto    base + link-time optimisation across the game (GCC: -flto=auto, thin partitions)
#   pgo_gen / pgo_use  profile-guided: instrumented build, then the build that uses the profile
#          (the recipe and its limits are in np/docs/vita.md "LTO and PGO")
NP_VOPT_BASE := -O2 -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections
NP_VOPT_LTO  := $(NP_VOPT_BASE) -flto=2 -fno-fat-lto-objects -fuse-linker-plugin
NP_VPGO_GEN  := $(NP_VOPT_LTO) -fprofile-generate -fprofile-update=single
NP_VPGO_USE  := $(NP_VOPT_LTO) -fprofile-use -fprofile-correction -Wno-missing-profile

# Libraries: the Sony import stubs the platform layer uses (an import table each; vita-elf-create
# fails if a referenced function is missing), -pthread (NOT -lpthread: VitaSDK's spec turns it into
# --whole-archive, which libstdc++'s weak pthread references need: std::thread, std::mutex and
# condition variables silently do nothing without it), kubridge's WEAK stubs.
NP_VBUILD ?= /home/claude/msc/build/np/NPV/vita
NP_VLIBS := -L$(NP_VBUILD)/stubs -lkubridge_stub_weak \
            -lSceDisplay_stub -lSceCtrl_stub -lSceAudio_stub -lScePower_stub -lSceAppMgr_stub -lSceVshBridge_stub \
            -lSceTouch_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub -lSceIofilemgr_stub -lSceProcessmgr_stub \
            -lSceRtc_stub -pthread -lm
# NPM's network layer (platform/sdk/net): the Wi-Fi/ad hoc radio of the Vita backend. The ad hoc import is WEAK: a
# console that cannot load the PSP ad hoc module still starts the application (the recompiled build linked it strong).
NP_VLIBS += -lSceNet_stub -lSceNetCtl_stub -lScePspnetAdhoc_stub_weak -lSceSysmodule_stub
ifeq ($(LAB),1)
# The lab server's sockets; every exit of the process passes through vita_lab.cpp.
NP_VLIBS += -lSceNet_stub -lSceNetCtl_stub -lSceSysmodule_stub -lScePromoterUtil_stub_weak
endif
# --wrap: pthread_create gives attribute-less threads a real stack and the helper core
# (np_vita_process.cpp); the mutex/cond wraps make an all-zero std::mutex usable before its
# constructor ran; remove() removes directories (std::filesystem); sceClibMspace*: the allocator
# vitaGL's pools are served from, every allocation counted against its pool for the memory report
# (vita_memreport.cpp; MSC_VITA_MEMSTATS=0 in env.txt makes the wraps forward only). The game's own wraps (the
# harness's frame hook: -Wl,--wrap=_Z10glxSwapPreb; vitaGL's sceClibMspace* counters) are added
# by the game's Makefile.
NP_VWRAPS := -Wl,--wrap=pthread_create \
             -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_mutex_trylock \
             -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_cond_timedwait \
             -Wl,--wrap=pthread_cond_signal -Wl,--wrap=pthread_cond_broadcast -Wl,--wrap=remove \
             -Wl,--wrap=sceClibMspaceCreate -Wl,--wrap=sceClibMspaceDestroy -Wl,--wrap=sceClibMspaceMalloc \
             -Wl,--wrap=sceClibMspaceCalloc -Wl,--wrap=sceClibMspaceRealloc -Wl,--wrap=sceClibMspaceMemalign \
             -Wl,--wrap=sceClibMspaceFree -Wl,--wrap=pte_osSemaphoreCancellablePend
ifeq ($(LAB),1)
NP_VWRAPS += -Wl,--wrap=sceKernelExitProcess -Wl,--wrap=_exit
endif
ifeq ($(NP_DIAG_GPU),1)
# opt-in GPU-call timing for the MSC_TOUCH_DIAG recorder (platform/compat/np_diag_gpu.cpp)
NP_VWRAPS += -Wl,--wrap=sceGxmFinish -Wl,--wrap=sceGxmNotificationWait -Wl,--wrap=sceGxmDisplayQueueAddEntry -Wl,--wrap=sceGxmBeginScene -Wl,--wrap=sceGxmEndScene
endif
ifeq ($(NP_LOGGING),0)
NP_VCOMMA := ,
NP_VWRAPS += $(addprefix -Wl$(NP_VCOMMA)--wrap=,printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite np_log OSReport OSVReport)
endif
endif # NP_VITA_MK
