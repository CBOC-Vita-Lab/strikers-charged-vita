#!/bin/bash
# One native console, one nav script (no peer): tools/mp/one.sh NAME SCRIPT [extra env KEY=VAL ...]; log build/np/NPM2/runs/NAME/runtime.log
#   heavy.sh -t 300 tools/mp/one.sh lobby tools/mp/lan/lan_lobby_only.txt MSC_VIRTUAL_TIME=1
cd "$(dirname "$0")/../.."
ROOT=$PWD; NAME=$1; SCRIPT=$(realpath "$2"); shift 2
EXE=${EXE:-/home/claude/msc/build/np/NPM2/host32/np-host32-full-null}
D=/home/claude/msc/build/np/NPM2/runs/$NAME; rm -rf $D; mkdir -p $D/data
env -i PATH=$PATH HOME=$D NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data MSC_INPUT_SCRIPT=$SCRIPT MSC_WATCH=1 MSC_WATCH_EVERY=600 MSC_FRAME_LOG=0 \
  MSC_LINK_PORT=41000 MSC_LINK_BCAST=127.0.0.1:41000,127.0.0.1:41001 MSC_LINK_VIP=10.0.0.1 MSC_LINK_NAME=VITA-1 "$@" \
  timeout -k 5 ${TMO:-280} $EXE > $D/runtime.log 2>&1
echo "rc=$?" >> $D/runtime.log
grep -E '^\[(script|mp-lan|mp-link|nav)\]|rc=|STUB|SIG' $D/runtime.log | tail -${TAILN:-60}
