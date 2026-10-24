# vgx builds. Everything links existing runtime + translated objects with the
# renderer in place of the null graphics layer (plat_null_gfx.o, generic_stubs.o)
# and the current navigation harness (vita/src/plat_headless.cpp).
#
#   make                 x86-64, GLES2 back end   -> build/host-gl/msc_gl
#   make null            x86-64, null back end    -> build/host-gl/msc_null   (no GL: fast functional runs)
#   make arm             armhf,  null back end    -> build/host-gl-arm/msc_arm_vgx   (qemu instruction counts)
#   make glnull          x86-64, GL back end over a GL that does nothing (src/glstub) -> build/host-gl/msc_glnull
#                        (GL call census per frame without rasterising: MSC_VGX_GLSTATS=<csv>; no heavy lock needed)
#   make armgl           armhf,  the same         -> build/host-gl-arm/msc_arm_vgx_glnull   (instruction counts of the
#                        render thread including the GL back end's own work, without a driver)
#
#   SUFFIX=_x            appended to the executable name (keep a binary a queued run refers to)
#
# HLE=1 additionally compiles runtime/src/hle/gx from the worktree (wt/gx) into
# the build directory and links those objects instead of the prebuilt ones
# (needed once the submission path in the HLE layer is changed).
MSC    ?= /home/claude/msc
FROZEN ?= $(MSC)/frozen/host
PORT   ?= $(MSC)/wt/gx
DEPS   ?= $(MSC)/deps
SRC    := $(CURDIR)/src
RT     := $(PORT)/runtime
# The platform repository's navigation harness (src/plat_headless.cpp and its headers) is
# compiled into every executable here. It is another workstream's tree: take it from a
# snapshot of that repository's HEAD commit (`make plat-snap`, build/host-gl/vita-snap), never
# from files somebody is editing. Without a snapshot the live tree is used, as before round 6.
PLATSNAP := $(MSC)/build/host-gl/vita-snap
PLAT   ?= $(if $(wildcard $(PLATSNAP)/src/plat_headless.cpp),$(PLATSNAP),$(MSC)/vita)

ARCH    ?= host
BACKEND ?= gl
HLE     ?= 0

ARMTARGET := --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
ifeq ($(ARCH),arm)
  BUILD   ?= $(MSC)/build/host-gl-arm
  TARGET  := $(ARMTARGET)
  LINK    := arm-linux-gnueabihf-g++
  OUT     := $(BUILD)/msc_arm_vgx$(if $(filter glnull,$(BACKEND)),_glnull,)$(SUFFIX)
  # A private, stable copy of the armhf objects of build/armlinux (see `make arm-objs`).
  OBJROOT := $(BUILD)/obj-base
  BASE_OBJ = $(filter-out %/plat_null_gfx.o %/generic_stubs.o %/plat_headless.o $(HLE_EXCL),$(shell find $(OBJROOT)/rt $(OBJROOT)/plat -name '*.o' | sort))
  TR_OBJ   = $(shell find $(OBJROOT)/tr -name '*.o' | sort)
  LIBS    := -lpthread -ldl
else
  BUILD   ?= $(MSC)/build/host-gl
  TARGET  := -march=x86-64-v3
  LINK    := clang++
  OUT     := $(BUILD)/$(if $(filter null,$(BACKEND)),msc_null,$(if $(filter glnull,$(BACKEND)),msc_glnull,msc_gl))$(SUFFIX)
  BASE_OBJ = $(filter-out %/plat_null_gfx.o %/generic_stubs.o %/plat_headless.o $(HLE_EXCL),$(shell find $(FROZEN)/obj -name '*.o' | sort))
  TR_OBJ   = -Wl,--whole-archive $(FROZEN)/libtranslated.a -Wl,--no-whole-archive
  LIBS    := $(if $(filter gl,$(BACKEND)),-lEGL -lGLESv2,) -lpthread -ldl
endif
O := $(BUILD)/obj-$(BACKEND)

CXX := clang++
CC  := clang
INC := $(if $(filter glnull,$(BACKEND)),-I$(SRC)/glstub,) -I$(SRC) -I$(PORT)/aurora-main/include -I$(DEPS)/SDL/include -I$(DEPS)/xxHash -I$(DEPS)/stb -I$(PLAT)/src
# EXTRA: extra compiler flags for the renderer sources, with O=<object dir> and SUFFIX for a variant
# build (e.g. EXTRA=-ffp-contract=off O=$(BUILD)/obj-glnull-nofma SUFFIX=_nofma: an x86-64 build
# without fused multiply-add, whose frame hashes are comparable with the ARM build's).
EXTRA ?=
CXXFLAGS := -std=c++20 -O2 -g -pipe $(TARGET) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DTARGET_PC -DNOMINMAX $(INC) $(EXTRA)

VGX_SRC := vgx_api.cpp vgx_rec.cpp vgx_play.cpp vgx_guestmem.cpp vgx_dlcache.cpp vgx_state.cpp vgx_draw.cpp vgx_shadergen.cpp vgx_texdecode.cpp vgx_texcache.cpp vgx_stats.cpp vgx_host.cpp vgx_interp.cpp vgx_present.cpp vgx_capture.cpp vgx_skin.cpp vgx_hdtex.cpp
ifeq ($(BACKEND),null)
  VGX_SRC += vgx_backend_null.cpp
else ifeq ($(BACKEND),glnull)
  VGX_SRC += vgx_backend_gl.cpp
else
  VGX_SRC += vgx_backend_gl.cpp vgx_host_egl.cpp
endif
VGX_OBJ := $(patsubst %.cpp,$(O)/%.o,$(VGX_SRC)) $(O)/vgx_host_stubs.o

PLAT_INC := -I$(RT)/include -I$(RT)/src -I$(PORT) -I$(PORT)/aurora-main/include -I$(RT)/third_party/toml11 -I$(RT)/third_party/pugixml \
            -I$(RT)/third_party/libco -I$(DEPS)/SDL/include -I$(DEPS)/imgui -I$(DEPS)/xxHash -I$(PLAT)/gen
RTFLAGS  := -std=c++20 -O2 -w -pipe $(TARGET) -DTARGET_PC -DNOMINMAX -DSDL_MAIN_HANDLED $(PLAT_INC)
# hle/gx sources additionally get -DMSC_VGX (direct submission paths, gx_vgx_fast.h) and vgx_hle.h.
PLAT_OBJ := $(O)/plat_headless.o

# The runtime's GX HLE layer, rebuilt from the worktree.
# HLE_RT: take runtime/src/hle/gx from another checkout than the one whose headers and frozen
# objects are linked (round 5: HLE_RT=$(MSC)/wt/gx5/runtime, the branch from `integ`; the hle/gx
# directory of `integ` and of wt/gx were byte-identical when it was branched, and gx5 only
# changes files inside it). Use a separate O= and SUFFIX= for such a build.
HLE_RT ?= $(RT)
ifeq ($(HLE),1)
  HLE_SRC  := $(wildcard $(HLE_RT)/src/hle/gx/*.cpp)
  HLE_OBJ  := $(patsubst $(HLE_RT)/src/hle/gx/%.cpp,$(O)/hle/%.o,$(HLE_SRC))
  HLE_EXCL := $(foreach s,$(HLE_SRC),%/hle/gx/$(notdir $(s:.cpp=.o)))
  OUT      := $(OUT)_hle
endif

all: $(OUT)
null:
	$(MAKE) BACKEND=null
arm:
	$(MAKE) ARCH=arm BACKEND=null
glnull:
	$(MAKE) BACKEND=glnull
armgl:
	$(MAKE) ARCH=arm BACKEND=glnull

$(O)/%.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h) $(wildcard $(SRC)/glstub/GLES2/*.h)
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(O)/vgx_host_stubs.o: $(SRC)/vgx_host_stubs.c
	@mkdir -p $(dir $@)
	$(CC) $(TARGET) -O1 -w -c $< -o $@

$(PLAT_OBJ): $(PLAT)/src/plat_headless.cpp
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(RTFLAGS) -c $< -o $@

$(O)/hle/%.o: $(HLE_RT)/src/hle/gx/%.cpp $(wildcard $(HLE_RT)/src/hle/gx/*.h) $(wildcard $(SRC)/vgx_hle*.h)
	@mkdir -p $(dir $@)
	@echo CXX hle/$(notdir $<)
	@$(CXX) $(RTFLAGS) -DMSC_VGX -I$(SRC) -c $< -o $@

$(OUT): $(VGX_OBJ) $(PLAT_OBJ) $(HLE_OBJ)
	@echo link $@
	@$(LINK) -o $@ $(BASE_OBJ) $(PLAT_OBJ) $(HLE_OBJ) $(VGX_OBJ) $(TR_OBJ) $(LIBS)

# Snapshot of the armhf objects (runtime, platform stand-ins, translated code)
# so that another workstream rebuilding build/armlinux cannot disturb a link.
arm-objs:
	mkdir -p $(MSC)/build/host-gl-arm/obj-base
	cp -a $(MSC)/build/armlinux/obj/rt $(MSC)/build/armlinux/obj/plat $(MSC)/build/armlinux/obj/tr $(MSC)/build/host-gl-arm/obj-base/

# Snapshot of the platform repository's HEAD commit (see PLAT above).
plat-snap:
	rm -rf $(PLATSNAP) && mkdir -p $(PLATSNAP)
	git -C $(MSC)/vita archive HEAD src gen | tar -x -C $(PLATSNAP)
	git -C $(MSC)/vita rev-parse --short HEAD > $(PLATSNAP)/commit

clean:
	rm -rf $(O) $(OUT)

.PHONY: all null arm glnull armgl arm-objs plat-snap clean

# ---- native library (np branch): the renderer without the recompilation runtime. See native/native.mk.
native:
	$(MAKE) -f native/native.mk
native-null:
	$(MAKE) -f native/native.mk NBACKEND=null
native-clean:
	$(MAKE) -f native/native.mk clean
native-vita:
	$(MAKE) -f native/native-vita.mk
.PHONY: native native-null native-clean native-vita
