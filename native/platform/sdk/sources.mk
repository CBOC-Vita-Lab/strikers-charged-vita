# Source lists of platform/sdk (shared by the Makefile and the main build of NPB).
#
# SRC_OWN: files in platform/sdk/src (".c" adapted from the decomp's SDK sources, ".cpp" ours; the
#          list names them with their extension, the Makefile maps them to objects).
# REF_SRC: files of np/ref/RVL_SDK compiled unchanged (path relative to ref/RVL_SDK, with ".c").

SRC_OWN := \
  np_hw_globals.c \
  np_rt.c \
  mtx_native.c \
  fmt_endian.c \
  os_time.c \
  os_thread.c \
  os_alarm.c \
  os_core.cpp \
  os_interrupt.cpp \
  os_virtual.cpp \
  dvd_fs.c \
  dvd_native.cpp \
  vi_native.cpp \
  sc_native.cpp \
  nand_native.cpp \
  input_native.cpp \
  thp_native.cpp \
  stubs_rfl_hbm.cpp \
  audio_out.cpp \
  dsp_hle.cpp \
  ax/ax_mix.cpp \
  ax/ax_memory.cpp \
  ax/ax_dsp_coef.c

REF_SRC := \
  os/OSArena.c \
  os/OSAlloc.c \
  os/OSMessage.c \
  os/OSMutex.c \
  mem/mem_allocator.c \
  mem/mem_expHeap.c \
  mem/mem_frameHeap.c \
  mem/mem_heapCommon.c \
  mem/mem_list.c \
  mem/mem_unitHeap.c \
  ax/AX.c \
  ax/AXAlloc.c \
  ax/AXAux.c \
  ax/AXCL.c \
  ax/AXComp.c \
  ax/AXOut.c \
  ax/AXProf.c \
  ax/AXSPB.c \
  ax/AXVPB.c \
  ax/DSPCode.c \
  axfx/AXFXDelay.c \
  axfx/AXFXDelayExpDpl2.c \
  axfx/AXFXHooks.c \
  axfx/AXFXReverbHi.c \
  axfx/AXFXReverbHiDpl2.c \
  axfx/AXFXReverbHiExp.c \
  axfx/AXFXReverbHiExpDpl2.c \
  mix/mix.c \
  mix/remote.c \
  arc/arc.c \
  tpl/TPL.c \
  sp/sp.c \
  wenc/wenc.c \
  net/md5.c \
  net/hmac.c \
  net/neterrorcode.c

# Game-side C++ files of the decomp's SDK tree (they include the game's NL headers) compiled into the library with a
# sed patch (patches/<path>.sed): the movie player. Path relative to ref/RVL_SDK, with ".cpp".
REF_CXX_SRC := \
  thp/THPSimple.cpp
