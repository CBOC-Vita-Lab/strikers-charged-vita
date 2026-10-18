#!/bin/bash
# Final verification of the branch in one heavy.sh turn: build (incremental) np-host32-full-null, then four pairs at once.
#   heavy.sh -t 1500 tools/mp/final_turn.sh
cd "$(dirname "$0")/../.."
TAILN=3 tools/mp/build_host32.sh 2>&1 | cut -c1-200 | tail -3 > /home/claude/msc/build/np/NPM2/logs/build_final.log
cat /home/claude/msc/build/np/NPM2/logs/build_final.log | tail -1
L=tools/mp/lan
PAIR_TIMEOUT=840 tools/mp/turn.sh "fin_base|--env MSC_LAN_PARTS=1" \
  "fin_pause|--host-script $L/lan_host_pause.txt --join-script $L/lan_join_pause.txt" \
  "fin_dch|--host-script $L/lan_host_dch.txt --join-script $L/lan_join_dch.txt" \
  "fin_series|--host-script $L/lan_host_series.txt --join-script $L/lan_join_series.txt --env MSC_LAN_TEST_SCORE=3000:0 --max-frames 40000"
