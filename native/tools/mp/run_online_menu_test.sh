#!/bin/bash
# The game's own multiplayer menus on the host (tools/mp/online_menu_test.py): mode/back/local checks, a pair that goes MULTIPLAYER -> ONLINE -> HOST / JOIN WITH A CODE -> lobby -> a
# short match over the online backend with the local fake of the service, a cancel with Circle while waiting for the partner, and a wrong code.
#   BIN=<np-host32 executable> [NODE=<node>] [CASES=modes,pair,cancel,wrong,lost] [DUMP_EVERY=25] [PROXY=<natsim_proxy>] [TIMEOUT=<s per case>] [NP_DISC_DIR=<game data>]
#   tools/mp/run_online_menu_test.sh [outdir]
# The pair and lost cases run two game processes in REAL time (a build lock around them only makes them miss their timing): run them without heavy.sh. Skips (exit 0, says why) when there is no executable, no
# game data or no node. DUMP_EVERY needs the GL build (np-host32-full-gl): every n-th frame is written as a PNG under <outdir>/<case>/.../pics.
cd "$(dirname "$0")/../.."
OUT=${1:-${TMPDIR:-/tmp}/np-online-menu-test}
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then echo "online menu test: skipped (set BIN to a host32 game executable)"; exit 0; fi
NODE=${NODE:-$(command -v node)}
if [ -z "$NODE" ] || [ ! -x "$NODE" ]; then echo "online menu test: skipped (no node for the fake service)"; exit 0; fi
FAKE=../server/fake/server.mjs
if [ ! -f "$FAKE" ]; then echo "online menu test: skipped (no $FAKE)"; exit 0; fi
if ! python3 -c "import sys, os; sys.path.insert(0, 'tools/mp'); import native_pair as n; sys.exit(0 if n.GAME.is_dir() else 1)"; then
  echo "online menu test: skipped (no game data: set NP_DISC_DIR)"; exit 0
fi
exec python3 tools/mp/online_menu_test.py --binary "$BIN" --fake-server "$FAKE" --node "$NODE" --out "$OUT" --cases "${CASES:-modes,pair,cancel,wrong,badcode,conntest,lost}" \
  ${DUMP_EVERY:+--dump-every $DUMP_EVERY} ${TIMEOUT:+--timeout $TIMEOUT} ${PROXY:+--proxy $PROXY}
