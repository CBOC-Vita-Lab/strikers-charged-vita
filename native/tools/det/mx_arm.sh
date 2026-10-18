#!/bin/bash
# armhf determinism matrix under qemu on the B3a build: the plain layout/poison configs and NPP's flags with different environment sizes.
#   heavy.sh -t 590 tools/det/mx_arm.sh [tag] [frames]
exec /home/claude/msc/np/tools/det/matrix.sh -q -x /home/claude/msc/build/np/B3a/armhf/np-armhf-full -t ${1:-ma1} -f ${2:-4400} -c "${CFGS:-base pad12 pad16 nppflags nppflags+pad16 nppflags+pad12 sp00 spAA hp00 hpA5}"
