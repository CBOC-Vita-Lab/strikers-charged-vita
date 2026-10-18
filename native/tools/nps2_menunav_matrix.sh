#!/bin/bash
# NPS2: the menu navigation script in virtual time with the AX mix on, and in real time with it off (the first build crashed in
# SoundPlaybackEvent::AccumulateRpcModifiers at f=58 with NP_AX_MIX=0 in virtual time).   heavy.sh -t 600 tools/nps2_menunav_matrix.sh
cd "$(dirname "$0")/.."
echo "== virtual, mix on";  SUF=-vmix1 VT=1 NP_AX_MIX=1 TAILN=14 tools/nps2_menunav.sh
echo "== real, mix off";    SUF=-rmix0 VT=0 NP_AX_MIX=0 TAILN=14 tools/nps2_menunav.sh
echo "== virtual, mix off, again"; SUF=-vmix0 VT=1 NP_AX_MIX=0 TAILN=14 tools/nps2_menunav.sh
