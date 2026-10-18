#!/bin/bash
# builds the qemu plugin (tools/perf/libframecount2.so) and the LD_PRELOAD spy (libspy.so, armhf) next to their sources
cd "$(dirname "$0")" || exit 1
gcc -O2 -shared -fPIC -o libframecount2.so framecount2.c -I/home/claude/research/qemu-full/include/qemu $(pkg-config --cflags --libs glib-2.0) || exit 1
arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libspy.so strlen_spy.c -ldl || exit 1
echo built framecount2 spy
# the race-check plugin (racecheck.c): memory words touched by the game thread and the frame-send worker in the same window
gcc -O2 -shared -fPIC -o libracecheck.so racecheck.c -I/home/claude/research/qemu-full/include/qemu $(pkg-config --cflags --libs glib-2.0) || exit 1
echo built racecheck
