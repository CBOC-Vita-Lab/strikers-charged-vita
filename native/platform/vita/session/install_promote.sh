#!/bin/bash
# Installs the native port on the console WITHOUT the owner touching it: vitacompanion (FTP 1337, commands 1338) takes the extracted
# application directory and its `promote` command makes it a LiveArea bubble. Run it on the Mac (or any machine that reaches the Vita):
#
#   export VITA_HOST=<your-console-address>
#   bash install_promote.sh [lab|player]        # lab = MSCN00001 (default), player = MSCN00003
#   bash install_promote.sh check               # only: is vitacompanion there, what version, is the lab server answering
#
# What it does (each step prints one line and stops on a failure):
#   1. `version` and `help` to the command port (the forms of the other commands in this vitacompanion are listed by `help`)
#   2. uploads the directory app-<TITLEID>/ (eboot.bin, sce_sys/...) to <STAGE> over FTP and reads every file back (sha256)
#   3. `promote <STAGE>` (expect "Promoted.")
#   4. removes the staging directory
#   5. `launch <TITLEID>`; for the lab build it waits for the lab server (vitalab.py ping, port 1339) and prints the answer
# It never quits other applications and never reboots (three chats share this console; the owner's rule: no plug-in or setting changes).
# Tools used: bash, curl, nc, shasum or sha256sum, python3 (for the ping). The Mac folder is mounted no-exec: run with `bash`.
# Variables: STAGE (default ux0:data/np-promote/<TITLEID>; ux0:data/pkg is what VitaShell uses if that is refused), FTP_PORT 1337, CMD_PORT 1338,
#            LAUNCH=0 (do not launch), WAIT=90 (seconds to wait for the lab server).
set -u
cd "$(dirname "$0")"
H=${VITA_HOST:?export VITA_HOST to your console address}
FTP_PORT=${FTP_PORT:-1337}; CMD_PORT=${CMD_PORT:-1338}
WHAT=${1:-lab}
case "$WHAT" in
  lab) TID=MSCN00001;; player) TID=MSCN00003;; check) TID=;;
  *) echo "usage: bash install_promote.sh [lab|player|check]"; exit 2;;
esac
SHA() { if command -v sha256sum >/dev/null; then sha256sum | cut -c1-64; else shasum -a 256 | cut -c1-64; fi; }
say() { echo "[$(date +%T)] $*"; }
cmd() {   # one line to the command port, prints the answer (nc -w: give up after N seconds of silence)
  printf '%s\n' "$1" | nc -w "${2:-8}" "$H" "$CMD_PORT" 2>&1 | tr -d '\r'
}
say "vitacompanion at $H: command port $CMD_PORT"
out=$(cmd version) || { say "no answer on the command port"; exit 1; }
[ -n "$out" ] || { say "FAILED: the command port accepted the connection but sent nothing (console asleep? FTP/command ports hang when it is: wake it, or the owner closes the running application)"; exit 1; }
say "version: $out"
if [ "$WHAT" = check ]; then
  say "help:"; cmd help | sed 's/^/    /'
  say "lab server (port 1339) of the installed title, if one is running:"; python3 vitalab.py ping || say "(not answering: normal when no lab build is running)"
  exit 0
fi

APP=app-$TID
[ -d "$APP" ] || { say "FAILED: no directory $APP next to this script (kit incomplete)"; exit 1; }
STAGE=${STAGE:-ux0:data/np-promote/$TID}
URLBASE="ftp://$H:$FTP_PORT/$(echo "$STAGE" | sed 's#:#:/#; s#//*#/#g')"
say "stage: $STAGE (ftp: $URLBASE)"
# a leftover staging directory of an earlier attempt must not mix with this one
files=$(cd "$APP" && find . -type f | sed 's#^\./##' | sort)
n=0; for f in $files; do n=$((n+1)); done
say "uploading $n files from $APP/"
for f in $files; do
  curl -sS --ftp-create-dirs -T "$APP/$f" "$URLBASE/$f" || { say "FAILED: upload of $f"; exit 1; }
done
bad=0
for f in $files; do
  a=$(SHA < "$APP/$f"); b=$(curl -sS "$URLBASE/$f" | SHA)
  [ "$a" = "$b" ] || { say "MISMATCH after upload: $f ($a / $b)"; bad=1; }
done
[ $bad = 0 ] || { say "FAILED: the uploaded files differ from the kit's (nothing was promoted)"; exit 1; }
say "uploaded and read back: $n files identical"

say "promote $STAGE"
out=$(cmd "promote $STAGE" 60)
say "answer: $out"
case "$out" in
  *Promoted*) ;;
  *) say "FAILED: not promoted. If the title is already installed: python3 vitalab.py uninstall $TID (from the running lab build of ANOTHER title), then again. If the path is refused: STAGE=ux0:data/pkg bash install_promote.sh $WHAT"; exit 1;;
esac
# the staging copy is no longer needed (promote does not remove it); delete what is left, files first and then the directories, best effort
FPATH="/$(echo "$STAGE" | sed 's#:#:/#; s#//*#/#g')"
for f in $files; do curl -sS -o /dev/null -Q "DELE $FPATH/$f" "ftp://$H:$FTP_PORT/" 2>/dev/null || true; done
for d in $(for f in $files; do dirname "$f"; done | sort -u | awk '{print length($0) " " $0}' | sort -rn | cut -d' ' -f2- ); do
  [ "$d" = . ] || curl -sS -o /dev/null -Q "RMD $FPATH/$d" "ftp://$H:$FTP_PORT/" 2>/dev/null || true
done
curl -sS -o /dev/null -Q "RMD $FPATH" "ftp://$H:$FTP_PORT/" 2>/dev/null || true

if [ "${LAUNCH:-1}" = 1 ]; then
  say "launch $TID"
  cmd "launch $TID" 10
  if [ "$WHAT" = lab ]; then
    say "waiting for the lab server (up to ${WAIT:-90} s)"
    t=0; while [ $t -lt "${WAIT:-90}" ]; do
      if r=$(python3 vitalab.py --timeout 5 ping 2>&1) && echo "$r" | grep -q "title=$TID"; then say "$r"; say "INSTALLED and answering"; exit 0; fi
      sleep 3; t=$((t+3))
    done
    say "the title is installed but the lab server did not answer in time (apps started remotely may start in the background behind the lock screen: try vitacompanion's unlock, or the owner starts the bubble once)"
    exit 3
  fi
  say "launched. (The player build has no lab server; it plays like a normal game.)"
fi
