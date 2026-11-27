# Host-side checks and the host platform library of the native port (platform layer's portable parts (the job pool; later the audio queue and
# the pad mapping) for x86-64 and armhf under qemu. The Vita package is built by vita.mk / Makefile.vpk.
#   make -C platform/vita check                    # x86-64: job pool tests in 9 configurations
#   make -C platform/vita TARGET=armhf check       # the same under qemu-arm (run through tools/heavy.sh)
#   make -C platform/vita tsan                     # x86-64 with ThreadSanitizer
TARGET ?= x86_64
OUT    ?= /work/msc/build/np/NPV/vita-host
HERE   := $(dir $(lastword $(MAKEFILE_LIST)))
B      := $(OUT)/$(TARGET)
HARN   := $(HERE)../harness
COMMON := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-psabi -fno-strict-aliasing -I$(HERE)include -I$(HERE)src -I$(HARN)/include
ifeq ($(TARGET),x86_64)
CXX := g++
FLAGS := $(COMMON)
RUN :=
else ifeq ($(TARGET),host32)
# as NPB's host32 build (docs/build.md): clang -m32 with SSE float math and the 64-bit host's libc headers
CXX := clang++
FLAGS := $(COMMON) -m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu
RUN :=
else ifeq ($(TARGET),armhf)
CXX := arm-linux-gnueabihf-g++
FLAGS := $(COMMON) -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
RUN := /work/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf
else
$(error TARGET must be x86_64, host32 or armhf)
endif
JOBSBIN := $(B)/test_jobs
NEONBIN := $(B)/test_neon
$(NEONBIN): $(HERE)tests/test_neon.cpp $(HERE)neon/np_neon.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -I$(HERE)neon -fno-inline-functions $< -o $@
$(B)/np_jobs.o: $(HERE)src/np_jobs.cpp $(HERE)include/np_jobs.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
$(B)/np_vclock.o: $(HARN)/src/np_vclock.cpp
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
$(B)/test_jobs.o: $(HERE)tests/test_jobs.cpp
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
$(JOBSBIN): $(B)/test_jobs.o $(B)/np_jobs.o $(B)/np_vclock.o
	$(CXX) $(FLAGS) $^ -o $@ -pthread -ldl
check: $(JOBSBIN)
	RUN="$(RUN)" BIN=$(JOBSBIN) $(HERE)tests/run_jobs_tests.sh
# the console audio output thread (np_vita_audio.cpp) on fakes of sceAudioOutOutput: three rotating buffers, none rewritten while it plays (STR-133)
AOBIN := $(B)/test_audio_out
$(AOBIN): $(HERE)tests/test_audio_out.cpp $(HERE)src/np_vita_audio.cpp $(HERE)src/vita_audio_queue.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -Wno-address -I$(HERE)tests/fake_audio -I$(HERE)../sdk/tests/fake_sce $< -o $@ -pthread
aocheck: $(AOBIN)
	$(RUN) $(AOBIN)
# the output queue's policies on a simulated clock: soft gaps, recovery, latency per pace slack (test_audio_queue.cpp)
AQBIN := $(B)/test_audio_queue
$(AQBIN): $(HERE)tests/test_audio_queue.cpp $(HERE)src/vita_audio_queue.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) $< -o $@
aqcheck: $(AQBIN)
	$(RUN) $(AQBIN)
# streamed music that survives a long game frame or a late read (AudioStreamKeep.h)
SKBIN := $(B)/test_stream_keep
$(SKBIN): $(HERE)tests/test_stream_keep.cpp $(HERE)../../include/Game/Audio/AudioStreamKeep.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) $< -o $@
skcheck: $(SKBIN)
	$(RUN) $(SKBIN)
# the frame-send worker (np_framesend.cpp): its unit tests in the four modes (make fscheck; make TARGET=x86_64 fstsan for ThreadSanitizer)
FSBIN := $(B)/test_framesend
$(B)/np_framesend.o: $(HERE)src/np_framesend.cpp $(HERE)include/np_framesend.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
$(B)/test_framesend.o: $(HERE)tests/test_framesend.cpp
	@mkdir -p $(B)
	$(CXX) $(FLAGS) -c $< -o $@
$(FSBIN): $(B)/test_framesend.o $(B)/np_framesend.o $(B)/np_vclock.o
	$(CXX) $(FLAGS) $^ -o $@ -pthread -ldl
fscheck: $(FSBIN)
	RUN="$(RUN)" BIN=$(FSBIN) $(HERE)tests/run_fs_tests.sh
fstsan:
	@mkdir -p $(OUT)/tsan
	g++ $(COMMON) -fsanitize=thread -O1 $(HERE)tests/test_framesend.cpp $(HERE)src/np_framesend.cpp $(HARN)/src/np_vclock.cpp -o $(OUT)/tsan/test_framesend -pthread -ldl
	cd $(OUT)/tsan && for m in thread serial inline off; do MSC_FRAMESEND=$$m ./test_framesend > tsanfs_$$m.log 2>&1 && echo "tsan frame send $$m: clean" || { echo "tsan frame send $$m: REPORT"; head -30 tsanfs_$$m.log; }; done
	cd $(OUT)/tsan && MSC_FRAMESEND=thread MSC_SKIN_LANE=1 ./test_framesend > tsanfs_lane.log 2>&1 && echo "tsan helper lane: clean" || { echo "tsan helper lane: REPORT"; head -30 tsanfs_lane.log; }
	cd $(OUT)/tsan && MSC_FRAMESEND=thread MSC_SKIN_LANE=1 MSC_SEM_MAX1=1 ./test_framesend > tsanfs_lane2.log 2>&1 && echo "tsan helper lane, semaphore maximum 1: clean" || { echo "tsan helper lane max1: REPORT"; head -30 tsanfs_lane2.log; }
	cd $(OUT)/tsan && MSC_FRAMESEND=thread MSC_SKIN_LANE=1 MSC_LANE_FREEZE_MS=300 ./test_framesend > tsanfs_lane3.log 2>&1 && echo "tsan helper lane, stalled lane thread: clean" || { echo "tsan helper lane frozen: REPORT"; head -30 tsanfs_lane3.log; }
	cd $(OUT)/tsan && for m in thread serial; do MSC_FRAMESEND=$$m MSC_FS_FREEGATE=2 ./test_framesend > tsanfs2_$$m.log 2>&1 && echo "tsan frame send $$m freegate=2: clean" || { echo "tsan frame send $$m freegate=2: REPORT"; head -30 tsanfs2_$$m.log; }; done
# np_sim's kick / sleep / join hand-shake (include/np/simwake17.h, simfix20): 300000 kicks with spinUs=0 (np_sim always sleeps); no join may wait LIMIT_MS (default 50; raise it
# on a loaded box, scheduler noise) and a lost wake-up (STALL, 500 ms) fails. simwakemut: the OLD ordering with a delay between the load and the store (compile-time mutant, must STALL = fail);
# simwakemut2: the same reordering with the join's re-signal (must pass: the re-signal repairs it). simwaketsan: the fixed code under ThreadSanitizer.
SWSRC := $(HERE)tests/test_simwake.cpp $(HERE)../../include/np/simwake17.h
SWFLAGS := $(FLAGS) -I$(HERE)../../include
LIMIT_MS ?= 50
$(B)/test_simwake: $(SWSRC)
	@mkdir -p $(B)
	$(CXX) $(SWFLAGS) $< -o $@ -pthread
$(B)/test_simwake_mutold: $(SWSRC)
	@mkdir -p $(B)
	$(CXX) $(SWFLAGS) -DNP_SIMWAKE_MUTANT_REORDER=40 -DNP_SIMWAKE_MUTANT_NORESIGNAL=1 $< -o $@ -pthread
$(B)/test_simwake_mutres: $(SWSRC)
	@mkdir -p $(B)
	$(CXX) $(SWFLAGS) -DNP_SIMWAKE_MUTANT_REORDER=40 $< -o $@ -pthread
simwakecheck: $(B)/test_simwake
	$(RUN) $(B)/test_simwake 300000 $(LIMIT_MS)
simwakemut: $(B)/test_simwake_mutold $(B)/test_simwake_mutres
	@if $(RUN) $(B)/test_simwake_mutold 20000 $(LIMIT_MS); then echo "simwakemut: the old ordering was NOT caught"; exit 1; else echo "simwakemut: the old ordering is caught (STALL, as it must)"; fi
	$(RUN) $(B)/test_simwake_mutres 20000 $(LIMIT_MS)
simwaketsan:
	@mkdir -p $(OUT)/tsan
	g++ $(COMMON) -I$(HERE)../../include -fsanitize=thread -O1 $(HERE)tests/test_simwake.cpp -o $(OUT)/tsan/test_simwake -pthread && $(OUT)/tsan/test_simwake 100000 $(LIMIT_MS) && echo "tsan simwake: clean"
neon: $(NEONBIN)
	$(RUN) $(NEONBIN)
tsan:
	@mkdir -p $(OUT)/tsan
	g++ $(COMMON) -fsanitize=thread -O1 $(HERE)tests/test_jobs.cpp $(HERE)src/np_jobs.cpp $(HARN)/src/np_vclock.cpp -o $(OUT)/tsan/test_jobs -pthread -ldl
	cd $(OUT)/tsan && for l in 2 3; do MSC_JOBS=$$l MSC_JOBS_SPIN_US=50 ./test_jobs > tsan$$l.log 2>&1 && echo "tsan $$l lanes: clean" || { echo "tsan $$l lanes: REPORT"; head -30 tsan$$l.log; }; done
# libnpplat.a: what the other workstreams link into np-host32 / np-armhf (np/docs/vita.md "Host builds"):
# the np_vita.h functions for Linux (arenas, paths, audio queue with a discarding device, input stub,
# job pool, core-plan roles), the harness (platform/harness) and its strong `main` (np_host_main.cpp,
# overrides NPB's weak one). Compile flags follow NPB's Makefile (no exceptions/RTTI are required of
# these files, they are compiled with the plain host flags).
#   make -C platform/vita TARGET=host32 hostlib      -> $(OUT)/host32/libnpplat.a
HOSTSRC := np_vita_audio np_vita_input vita_threads np_jobs np_host_platform np_port_parallel np_framesend np_settings np_settings_guard
HARNSRC := np_harness np_vclock np_harness_game np_wrap_swap
HOBJS := $(patsubst %,$(B)/h_%.o,$(HOSTSRC)) $(patsubst %,$(B)/hh_%.o,$(HARNSRC))
$(B)/h_%.o: $(HERE)src/%.cpp $(HERE)include/np_vita.h
	@mkdir -p $(B)
	$(CXX) $(FLAGS) $(HOSTDEFS) -c $< -o $@
$(B)/hh_%.o: $(HARN)/src/%.cpp
	@mkdir -p $(B)
	$(CXX) $(FLAGS) $(HOSTDEFS) -c $< -o $@
HOSTDEFS := $(if $(filter x86_64,$(TARGET)),-DNP_HARNESS_ALLOW_64)
# np_host_main.o is a LOOSE object: an archive member is never pulled to replace a weak definition
# (NPB's np_main.o has a weak main), so the link line names it: `... $(B)/np_host_main.o ... -lnpplat`.
hostlib: $(B)/libnpplat.a $(B)/np_host_main.o
$(B)/np_host_main.o: $(HARN)/src/np_host_main.cpp
	@mkdir -p $(B)
	$(CXX) $(FLAGS) $(HOSTDEFS) -c $< -o $@
$(B)/libnpplat.a: $(HOBJS)
	ar rcs $@ $^
	@echo "built $@ ($(words $(HOBJS)) objects)"
clean:
	rm -rf $(B)
.PHONY: check neon tsan fscheck fstsan simwakecheck simwakemut simwaketsan hostlib clean
