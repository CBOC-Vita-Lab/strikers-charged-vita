#!/bin/bash
# The online unit tests plus the signaling client against the LOCAL fake of the online service (server/fake/server.mjs: the Worker's own code over
# in-memory SQLite, plain HTTP on localhost). Starts the fake, registers two players, runs tools/net/online_test with MSC_TEST_SERVER set.
#   FAKE_SERVER=<path to server/fake/server.mjs> NODE=<node> heavy.sh tools/net/run_signal_http_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
ROOT=$(git rev-parse --show-toplevel)
FAKE=${FAKE_SERVER:-$ROOT/server/fake/server.mjs}
NODE=${NODE:-node}
OUT=${1:-/tmp/np-signal-http-test}
mkdir -p "$OUT"
PORT=${FAKE_PORT:-$((18000 + $$ % 2000))}
[ -f "$FAKE" ] || { echo "no fake server at $FAKE (set FAKE_SERVER)"; exit 2; }
"$NODE" "$FAKE" --http-port $PORT --https-port $((PORT + 1)) > "$OUT/fake.log" 2>&1 &
FAKE_PID=$!
trap 'kill $FAKE_PID 2>/dev/null' EXIT
for i in $(seq 1 60); do curl -s "http://127.0.0.1:$PORT/v1/info" >/dev/null 2>&1 && break; sleep 0.25; done
reg() { curl -s -X POST "http://127.0.0.1:$PORT/v1/register" -H 'X-Msc-App: 1.2.0' -H 'Content-Type: application/json' -d "{\"name\":\"$1\",\"region\":2}" | sed -e 's/.*"token":"\([^"]*\)".*/\1/'; }
T0=$(reg Alice1); T1=$(reg Bobby2)
[ ${#T0} = 43 ] && [ ${#T1} = 43 ] || { echo "registration failed"; cat "$OUT/fake.log"; exit 3; }
export MSC_TEST_SERVER=http://127.0.0.1:$PORT MSC_TEST_TOKEN0=$T0 MSC_TEST_TOKEN1=$T1
SAN=${SAN} bash tools/net/run_online_tests.sh "$OUT" "${@:2}"
