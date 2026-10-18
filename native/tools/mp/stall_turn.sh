#!/bin/bash
# The keep-alive limit: build (incremental), then three pairs at once:
#   stall20: the host's game thread sleeps 10 s at match tick 1000 (a first-use shader compile on the Vita); default limit 20 s: the match goes on
#   stall6:  the same with the retail limit (MSC_LAN_KEEPALIVE_MS=6000): the healthy console and the stalled one declare the connection lost (negative control)
#   k_base:  a plain pair with the new default limit
#   heavy.sh -t 1200 tools/mp/stall_turn.sh
cd "$(dirname "$0")/../.."
TAILN=3 tools/mp/build_host32.sh 2>&1 | cut -c1-200 | tail -3 > /home/claude/msc/build/np/NPM2/logs/build_stall.log
tail -1 /home/claude/msc/build/np/NPM2/logs/build_stall.log
PAIR_TIMEOUT=840 tools/mp/turn.sh "stall20|--env0 MSC_LAN_TEST_STALL=1000:10000 --max-frames 30000" \
  "stall6|--env0 MSC_LAN_TEST_STALL=1000:10000 --env MSC_LAN_KEEPALIVE_MS=6000 --max-frames 30000" \
  "k_base|--env MSC_LAN_PARTS=1"
