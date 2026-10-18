#!/bin/bash
# Runs a host32 GL program with the private i386 Mesa (llvmpipe, surfaceless EGL).
#   native/test/run32.sh <program> [args]
R=${I386ROOT:-/home/claude/msc/build/np/NPR/i386apt/root}
L=$R/usr/lib/i386-linux-gnu
export LD_LIBRARY_PATH=$L${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LIBGL_DRIVERS_PATH=$L/dri
export __EGL_VENDOR_LIBRARY_DIRS=$R/usr/share/glvnd/egl_vendor.d
export EGL_PLATFORM=surfaceless
export GALLIUM_DRIVER=${GALLIUM_DRIVER:-llvmpipe}
export LP_NUM_THREADS=${LP_NUM_THREADS:-1}
exec "$@"
