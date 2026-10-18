#!/bin/bash
# seg_gap.sh <elf>: succeeds when vita-elf-create will find room for its module data.
# vita-elf-create appends about 2 KiB ("SCE data") to the end of the first loadable
# segment (code + read-only data) and fails with "Cannot allocate N bytes for SCE data at
# end of segment 0; segment 1 overlaps" when the data segment, which the linker script
# starts on the next 64 KiB boundary, begins closer than that. Whether it does is an
# accident of the code size (about 3 links in 100). The makefiles call this after every
# link and relink with tools' 4 KiB pad object (src/vita_pad.c) when it fails.
set -e
RE=${READELF:-/opt/vitasdk/bin/arm-vita-eabi-readelf}
$RE -l -W "$1" | python3 -c '
import sys
loads = [l.split() for l in sys.stdin if l.split()[:1] == ["LOAD"]]
if len(loads) < 2: sys.exit(0)
gap = int(loads[1][2], 16) - (int(loads[0][2], 16) + int(loads[0][5], 16))
sys.stderr.write("segment gap %d bytes\n" % gap)
sys.exit(0 if gap >= 3072 else 1)'
