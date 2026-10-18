# NPA asset converter tests.  make -C np/tools/assets [M32=1|ARM=1] check GAME=/home/claude/msc/game/files
# M32=1: x86 32-bit (ILP32, no zlib).  ARM=1: armhf Cortex-A9 binary (run it under qemu-arm via heavy.sh).
# Heavy runs go through /home/claude/msc/tools/heavy.sh (see docs/assets.md).
CC      ?= cc
ifdef ARM
CC      := arm-linux-gnueabihf-gcc
endif
LIBDIR  := ../../platform/assets
OUT     ?= /home/claude/msc/build/np/NPA/assets$(if $(M32),32,)$(if $(ARM),-armhf,)
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -std=gnu11 -I$(LIBDIR) -I. $(if $(M32),-m32 -I/usr/include/x86_64-linux-gnu -DNPA_NO_ZLIB,)$(if $(ARM),-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DNPA_NO_ZLIB,)
LIB_SRC := $(wildcard $(LIBDIR)/np_*.c)
GAME    ?= /home/claude/msc/game/files

all: $(OUT)/npa_check $(OUT)/npa_convert_tree

$(OUT)/npa_check: npa_check.c npa_verify.c $(LIB_SRC) $(wildcard $(LIBDIR)/*.h) npa_verify.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ npa_check.c npa_verify.c $(LIB_SRC) $(if $(M32)$(if $(ARM),1,),,-lz) -lm

$(OUT)/npa_convert_tree: npa_convert_tree.c npa_verify.c $(LIB_SRC) $(wildcard $(LIBDIR)/*.h) npa_verify.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ npa_convert_tree.c npa_verify.c $(LIB_SRC) $(if $(M32)$(if $(ARM),1,),,-lz) -lm

check: $(OUT)/npa_check
	$(OUT)/npa_check $(GAME)

.PHONY: all check
