#!/bin/bash
# The relay (TURN) tests: the client against the independent test TURN server (Allocate with the 401 round trip, Refresh, CreatePermission, ChannelBind,
# ChannelData, stale nonces, credential expiry and rotation, symmetric-vs-symmetric and relay-only pairs at 0% and 2% loss, every failure sentence) in
# virtual time, an independent re-computation of MESSAGE-INTEGRITY in Python, and the service side through its local fake (server/fake): credentials,
# the daily budget and per-player limit sentences, and whole relayed sessions steered by the service.
#   FAKE_SERVER=<server/fake/server.mjs> NODE=<node> heavy.sh tools/mp/run_turn_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
ROOT=$(git rev-parse --show-toplevel)
FAKE=${FAKE_SERVER:-$ROOT/server/fake/server.mjs}
NODE=${NODE:-node}
command -v "$NODE" >/dev/null 2>&1 || { [ -x /opt/node22/bin/node ] && NODE=/opt/node22/bin/node; }
OUT=${1:-/tmp/np-turn-test}
mkdir -p "$OUT"
SECRET=turn-test-secret
URLS="turn:198.51.100.9:3478?transport=udp,turn:198.51.100.9:53?transport=udp"
PORT=${FAKE_PORT:-$((19000 + $$ % 1500))}
PIDS=""
trap 'kill $PIDS 2>/dev/null' EXIT
reg() { curl -s -X POST "http://127.0.0.1:$1/v1/register" -H 'X-Msc-App: 1.2.0' -H 'Content-Type: application/json' -d "{\"name\":\"$2\",\"region\":2}" | sed -e 's/.*"token":"\([^"]*\)".*/\1/'; }
start() {   # start <port> <env...>: a fake service; sets STARTED to "base,token0,token1" (not run in a subshell: the trap must know the process)
  local port=$1; shift
  env "$@" TURN_PROVIDER=stub TURN_STUB_URLS="$URLS" TURN_STUB_SECRET=$SECRET "$NODE" "$FAKE" --http-port $port --https-port $((port + 1)) > "$OUT/fake$port.log" 2>&1 &
  PIDS="$PIDS $!"
  for i in $(seq 1 80); do curl -s "http://127.0.0.1:$port/v1/info" >/dev/null 2>&1 && break; sleep 0.25; done
  local a=$(reg $port Alice1) b=$(reg $port Bobby2)
  [ ${#a} = 43 ] && [ ${#b} = 43 ] || { echo "registration failed on $port"; cat "$OUT/fake$port.log"; exit 3; }
  STARTED="http://127.0.0.1:$port,$a,$b"
}
if [ -f "$FAKE" ] && command -v "$NODE" >/dev/null 2>&1; then
  start $PORT X=1; export MSC_TEST_TURN_SESSION=$STARTED
  start $((PORT + 2)) TURN_MAX_CREDS_PER_DAY=1; export MSC_TEST_TURN_BUDGET=$STARTED
  start $((PORT + 4)) TURN_MAX_CREDS_PER_PLAYER_DAY=1; export MSC_TEST_TURN_PLAYER=$STARTED
else
  echo "(the service tests are skipped: no fake server at $FAKE or no node)"
fi
bash tools/net/run_online_tests.sh "$OUT" --turn | tee "$OUT/turn.out"
# independent MESSAGE-INTEGRITY check: key = MD5(user:realm:password), HMAC-SHA1 over the message up to the attribute, header length = everything up to and including it
python3 -I - "$OUT/turn.out" <<'PY'
import hashlib, hmac, struct, sys
line = [l for l in open(sys.argv[1]) if l.startswith("TURN_AUTH_REQUEST")]
assert line, "no authenticated request was printed"
_, hexmsg, realm, user, password = line[0].split()
msg = bytes.fromhex(hexmsg)
assert msg[4:8] == bytes.fromhex("2112a442"), "magic cookie"
mtype, mlen = struct.unpack(">HH", msg[:4])
assert mlen == len(msg) - 20, "length field"
off, attrs = 20, {}
while off < len(msg):
    t, l = struct.unpack(">HH", msg[off:off + 4])
    attrs[t] = (off, msg[off + 4:off + 4 + l])
    off += 4 + ((l + 3) & ~3)
assert attrs[0x0006][1].decode() == user and attrs[0x0014][1].decode() == realm
assert 0x0015 in attrs and 0x0019 in attrs, "nonce and requested transport"
at = attrs[0x0008][0]
assert at + 24 == len(msg), "MESSAGE-INTEGRITY must be last"
key = hashlib.md5(f"{user}:{realm}:{password}".encode()).digest()
body = bytearray(msg[:at]); body[2:4] = struct.pack(">H", at - 20 + 24)
assert hmac.new(key, bytes(body), hashlib.sha1).digest() == attrs[0x0008][1], "MESSAGE-INTEGRITY differs from the independent computation"
print("MESSAGE-INTEGRITY of the Allocate request verified in Python (type 0x%04x, %d bytes, user %s)" % (mtype, len(msg), user))
PY
