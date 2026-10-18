#!/bin/bash
# Differential test of stack garbage: the same armhf match with every automatic variable zero-initialised (B3z, AUTOINIT=zero), pattern-initialised (B3p,
# AUTOINIT=pattern: 0xAA bytes) and as built (B3a); per-step digests must be identical if no uninitialised local influences the simulation.
#   heavy.sh -t 700 tools/det/autoinit.sh [frames]
B=/home/claude/msc/build/np; NP=/home/claude/msc/np; FR=${1:-8000}; NAVF=$NP/tools/nav/nav_fulltime.txt
for v in z p; do
  $NP/tools/det/matrix.sh -q -x $B/B3$v/armhf/np-armhf-full -t a$v -s $NAVF -f $FR -c base 2>&1 | head -3
done
python3 -I $NP/tools/det/stepcmp.py $B/B3/mx/pa1-base.trace.gz $B/B3/mx/az-base.trace.gz $B/B3/mx/ap-base.trace.gz
