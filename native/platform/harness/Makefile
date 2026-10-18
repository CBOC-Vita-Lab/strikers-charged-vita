# Harness library and its unit tests, for the three host-side targets. Vita: platform/vita/vita.mk.
#   make -C platform/harness test                      # x86-64: library + mock + tests/run_tests.sh
#   make -C platform/harness TARGET=armhf test         # armhf under qemu-arm (through tools/heavy.sh)
#   make -C platform/harness TARGET=host32 test        # clang -m32, as NPB's host32 build
# Output: $(OUT)/<target>/ (default /home/claude/msc/build/np/NPV/harness).
TARGET ?= x86_64
OUT    ?= /home/claude/msc/build/np/NPV/harness
HERE   := $(dir $(lastword $(MAKEFILE_LIST)))
B      := $(OUT)/$(TARGET)
COMMON := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -I$(HERE)include
ifeq ($(TARGET),x86_64)
CXX := g++
FLAGS := $(COMMON)
RUN :=
else ifeq ($(TARGET),host32)
CXX := clang++
FLAGS := $(COMMON) -m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu
RUN :=
else ifeq ($(TARGET),armhf)
CXX := arm-linux-gnueabihf-g++
FLAGS := $(COMMON) -Wno-psabi -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
RUN := /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf
else
$(error TARGET must be x86_64, host32 or armhf)
endif
LIBSRC := np_harness.cpp np_vclock.cpp np_harness_game.cpp
LIBOBJ := $(patsubst %.cpp,$(B)/%.o,$(LIBSRC)) $(B)/np_wrap_swap.o
TESTBIN := $(B)/test_harness
GAMEFLAGS := -DNP_HARNESS_ALLOW_64

all lib: $(B)/libnpharness.a
$(B)/%.o: $(HERE)src/%.cpp $(HERE)src/np_harness_state.h $(HERE)include/np_harness.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) $(if $(filter x86_64,$(TARGET)),$(GAMEFLAGS)) -c $< -o $@
$(B)/libnpharness.a: $(LIBOBJ)
	ar rcs $@ $^
$(B)/mock_state.o: $(HERE)tests/mock_state.cpp $(HERE)src/np_harness_state.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
$(B)/test_harness.o: $(HERE)tests/test_harness.cpp
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
# The test binary links the harness core + clock + the MOCK state (not the game reader).
$(TESTBIN): $(B)/test_harness.o $(B)/mock_state.o $(B)/np_harness.o $(B)/np_vclock.o
	$(CXX) $(FLAGS) $^ -o $@ -pthread -ldl
test: $(TESTBIN) $(B)/libnpharness.a
	RUN="$(RUN)" BIN=$(TESTBIN) TOOLS=$(HERE)tests $(HERE)tests/run_tests.sh
clean:
	rm -rf $(B)
.PHONY: all lib test clean
