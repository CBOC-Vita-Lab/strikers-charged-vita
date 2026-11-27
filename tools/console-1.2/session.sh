#!/bin/bash
# Console session driver for the test kit (1.3; the folder keeps its 1.2 name): runs the matrix on a Vita through vitacompanion (FTP 1337, commands 1338), pulls the logs.
# Plain bash 3.2 + curl + nc + python3, so it runs on a Mac.
#
#   export VITA_IP=<the Vita's address>              # (VITA_HOST works too) and VITA_IP2=<second Vita> for the LAN legs
#   bash session.sh check                            # is vitacompanion there, is the test app installed, are curl/nc/python3 here (read-only)
#   bash session.sh core                             # the core session (matrix.py tier 0, about two hours): 1.1 vs 1.3, current vs proposed, looks, stability
#   bash session.sh full                             # everything, unattended: core, tier 1, tier 2, the automatic add-ons, the LAN legs if VITA_IP2 is set, report
#   bash session.sh night                            # the overnight session (night.py, about 8 hours, in order of value, phases with deadlines, checkpoint reports, MORNING.md)
#   bash session.sh addon                            # the add-on runs, including the one that needs a plug-in set by hand (its 500 MHz profile is set by hand)
#   bash session.sh stage                            # push the routes and the two LAN scripts to the test app's private folder
#   bash session.sh run d1                           # one run of the matrix: env file, launch, wait, pull log (and shots), time
#   bash session.sh swap 1.1                         # put the 1.1 executable and shader pack into the test app (versions/1.1/)
#   bash session.sh all [--tier 2]                   # the whole session in the matrix order (resumable: finished runs are skipped)
#   bash session.sh lan lan-base                     # one two-Vita leg (needs VITA_HOST2)
#   bash session.sh list my.list                     # run/swap lines of your own (the format of gen_kit.py --list), with the same never-stop rules
#                                                    # (more line kinds: `fastboot 1` = the fast-boot route and env-fb/ file of every run that has one, FASTBOOT=1 does the same for one command;
#                                                    #  `eswap NAME` = eboot.bin alone from versions/NAME/; `restore` = the owner's np-player.txt back)
#   bash session.sh eswap r16pgo                     # put versions/r16pgo/eboot.bin (and nothing else) on the test app, read it back (prepare-versions.py --eboot r16pgo=<vpk>)
#   bash session.sh restore                          # put the np-player.txt the console had before the first run of this session back (or delete the one the session made)
#   DRY_RUN=1 bash session.sh list r16-sitting.txt   # walk a list against a stub console in $OUT/dry-console: nothing leaves this machine (curl and nc are replaced by a refusal)
#   bash session.sh report                           # the one table, the core summary, the contact sheets and ISSUES.md
#
# core, full and all never stop at one bad run: a run that crashes, hangs or fails to upload is logged and the next one starts; a swap that fails
# skips the runs of that version until the next swap. They stop only when the console stops answering for MAX_FAILS runs in a row (default 3); run
# the same command again to resume (finished runs are skipped). Each run also lists the names of new crash dumps (ux0:data/psp2core-*.psp2dmp, names only,
# nothing read or deleted; DUMPS=0 turns that listing off).
#
# What it can touch on the console: ux0:data/mscharged-perf-test/ (the test app's private folder) and, for `swap`, eboot.bin and
# shadercache/ of ux0:app/CDXMP0001. Every FTP write, read and delete goes through remote_ok() which refuses anything else; the only
# commands sent to port 1338 are version, help, "quit CDXMP0001" and "launch CDXMP0001". Other running or suspended apps are left alone.
#
# Variables: OUT (default ./out-r17, a NEW folder for each sitting: logs, shots, session-times.csv), VERSIONS (default $OUT/versions, from prepare-versions.py),
# FTP_PORT 1337, CMD_PORT 1338, POLL_S 10 (seconds between log polls once the route should be over), STALL_S 90 (no new log bytes for this long = stalled), FORCE=1 (redo a finished run), (the clock line of each run is only checked and warned about, never a stop: clock_check).
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
H=${VITA_IP:-${VITA_HOST:-}}
H2=${VITA_IP2:-${VITA_HOST2:-}}
FTP_PORT=${FTP_PORT:-1337}
CMD_PORT=${CMD_PORT:-1338}
OUT=${OUT:-$PWD/out-r17}      # one folder per sitting (fable-da21 F4): finished runs and the kept owner np-player.txt are read from here, so never reuse a folder of an earlier sitting
VERSIONS=${VERSIONS:-$OUT/versions}
TID=CDXMP0001
PRIV=ux0:/data/mscharged-perf-test
APP=ux0:/app/$TID
STATE=$OUT/state
TIMES=$OUT/session-times.csv
ROUTES="worstgpu tour look mega pipe ctrl replay ctour cpipe clava crep ntour npipe nlava cstab cintro cend on-hub on-conn on-quick on-ranked on-lists on-hostwait lan-wifi-wait lan-adhoc-wait"

say() { echo "[$(date +%H:%M:%S)] $*"; }
die() { say "STOP: $*"; exit 1; }

sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@" | cut -c1-64; else shasum -a 256 "$@" | cut -c1-64; fi; }
shastdin() { if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -c1-64; else shasum -a 256 | cut -c1-64; fi; }

norm() { echo "$1" | sed 's#^ux0:/*#ux0:/#; s#//*#/#g'; }

# The only places this script may write, read or delete on a console.
remote_ok() {
  local p; p=$(norm "$1")
  case "$p" in *..*) return 1;; esac
  case "$p" in
    "$PRIV"|"$PRIV"/*) return 0;;
    "$APP/eboot.bin"|"$APP/shadercache"|"$APP/shadercache"/*) return 0;;
  esac
  return 1
}

furl() { echo "ftp://${HOST:-$H}:$FTP_PORT/$(norm "$1")"; }

# HOST=<address> before a call picks the console (default VITA_HOST).
# A Wi-Fi hiccup in the middle of the night must not cost a run: every transfer is tried FTP_TRIES times (default 3, FTP_RETRY_S apart).
put() {   # put LOCAL REMOTE
  remote_ok "$2" || die "refusing to write $2"
  local n=0
  while :; do
    curl -sS --max-time 120 --ftp-create-dirs -T "$1" "$(furl "$2")" && return 0
    n=$((n + 1)); [ "$n" -ge "${FTP_TRIES:-3}" ] && return 1
    sleep "${FTP_RETRY_S:-5}"
  done
}
get1() {   # get1 REMOTE LOCAL: one try (polling; a missing file is an answer, not a fault)
  remote_ok "$1" || die "refusing to read $1"
  curl -sSf --max-time 60 -o "$2" "$(furl "$1")"
}
get() {   # get REMOTE LOCAL: tried FTP_TRIES times
  local n=0
  while :; do
    get1 "$1" "$2" 2>/dev/null && return 0
    n=$((n + 1)); [ "$n" -ge "${FTP_TRIES:-3}" ] && return 1
    sleep "${FTP_RETRY_S:-5}"
  done
}
rsize() {   # rsize REMOTE: the file's size in bytes from an FTP SIZE (cheap: no transfer), empty when the console does not say
  remote_ok "$1" || die "refusing to read $1"
  curl -sSI --max-time 20 "$(furl "$1")" 2>/dev/null | tr -d '\r' | awk -F': *' 'tolower($1)=="content-length" {print $2; exit}'
}
del() {   # del REMOTE (best effort; a log or screenshot of an earlier run)
  remote_ok "$1" || die "refusing to delete $1"
  curl -sS --max-time 20 -Q "-DELE $(norm "$1")" -o /dev/null "ftp://${HOST:-$H}:$FTP_PORT/" >/dev/null 2>&1
  return 0
}
names() {   # names REMOTE_DIR: file names in a folder, one per line
  remote_ok "$1" || die "refusing to list $1"
  curl -sS --max-time 20 -l "$(furl "${1%/}/")" 2>/dev/null | tr -d '\r'
}
ls_read() { curl -sS --max-time 20 -l "ftp://${HOST:-$H}:$FTP_PORT/$(norm "$1")" 2>/dev/null | tr -d '\r'; }   # read-only look, check only

vcmd() {  # one of the four allowed lines to the command port
  case "$1" in
    "version"|"help"|"quit $TID"|"launch $TID") ;;
    *) die "refusing command: $1";;
  esac
  printf '%s\n' "$1" | nc -w "${2:-8}" "${HOST:-$H}" "$CMD_PORT" 2>&1 | tr -d '\r'
}

need_host() { [ -n "${HOST:-$H}" ] || die "export VITA_IP=<the Vita's address>"; }

# ---- dry run (DRY_RUN=1): a stub console in a folder; put/get/del/names/size/vcmd work on it, curl and nc are refused, sleeps are skipped -----------------------------------
# The guards (remote_ok, the four allowed commands) stay in force: the stubs sit BELOW them. A launch writes a short fake log from the run's env file so that the loop can finish.
if [ "${DRY_RUN:-}" = 1 ]; then
  DRY=${DRY_ROOT:-$OUT/dry-console}; H=dry; H2=""; NAP=0; POLL_S=0; FTP_RETRY_S=0; LAUNCH_POLL_S=0; export NAP POLL_S
  mkdir -p "$DRY"
  curl() { echo "DRY_RUN: a curl call would have contacted a console: $*" >&2; return 99; }
  nc() { echo "DRY_RUN: an nc call would have contacted a console" >&2; return 99; }
  sleep() { :; }
  dry_path() { norm "$1" | sed 's#^ux0:/##'; }
  put() { remote_ok "$2" || die "refusing to write $2"; local d="$DRY/$(dry_path "$2")"; mkdir -p "$(dirname "$d")"; cp "$1" "$d"; }
  get1() { remote_ok "$1" || die "refusing to read $1"; [ -f "$DRY/$(dry_path "$1")" ] && cp "$DRY/$(dry_path "$1")" "$2"; }
  get() { get1 "$1" "$2"; }
  rsize() { remote_ok "$1" || die "refusing to read $1"; [ -f "$DRY/$(dry_path "$1")" ] && wc -c < "$DRY/$(dry_path "$1")" | tr -d ' '; return 0; }
  del() { remote_ok "$1" || die "refusing to delete $1"; rm -f "$DRY/$(dry_path "$1")"; return 0; }
  names() { remote_ok "$1" || die "refusing to list $1"; ls -1 "$DRY/$(dry_path "$1")" 2>/dev/null; return 0; }
  ls_read() { ls -1 "$DRY/$(dry_path "$1")" 2>/dev/null; return 0; }
  vcmd() {
    case "$1" in "version"|"help"|"quit $TID"|"launch $TID") ;; *) die "refusing command: $1";; esac
    case "$1" in
      version) echo "dry-run console";;
      "launch $TID")
        local envf="$DRY/$(dry_path "$PRIV/np-player.txt")" route
        route=$(sed -n 's/^MSC_INPUT_SCRIPT=//p' "$envf" | head -1)
        [ -f "$DRY/$(dry_path "$route")" ] || { echo "DRY: the route $route is not on the stub console: the app would not have started" >&2; return 0; }
        { echo "===== dry-run build"; grep -v '^#' "$envf" | sed 's/^/[env] /'; if grep -q "^MSC_VITA_CPU_MHZ=444" "$envf"; then echo "[vita] clocks: ARM 444 MHz"; else echo "clocks: policy max; requested ARM none (the core already measures 498 MHz); measured 498 before, 498 after (MHz)"; fi; echo "[frame-timing] dry"; echo "[script] script exit"; } > "$DRY/$(dry_path "$PRIV/log.txt")";;
    esac
  }
fi

FASTBOOT=${FASTBOOT:-0}

# Crash dumps: the system writes ux0:data/psp2core-<time>-<process>.psp2dmp when an application crashes. Only the names of such files are
# looked at (a read-only listing filtered at once), never their content or anything else in ux0:data.
dump_names() { [ "${DUMPS:-1}" != 0 ] || return 0; ls_read ux0:/data/ | grep -E '^psp2core-[A-Za-z0-9_.-]*\.psp2dmp$' | sort; }
# DUMPS=1 (default): a dump that appeared during a run is COPIED into <run>/dumps/ (read only, never deleted, at most DUMP_MAX per run, none over DUMP_MAX_MB MiB);
# DUMPS=names: only the names are listed; DUMPS=0: nothing. The file name must be a plain psp2core-*.psp2dmp in ux0:data.
dump_copy() {   # dump_copy NAMEFILE DIR
  [ "${DUMPS:-1}" = 1 ] || return 0
  local f n=0 size lim=$(( ${DUMP_MAX_MB:-96} * 1048576 ))
  mkdir -p "$2/dumps"
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    case "$f" in */*|*..*) continue;; esac
    [ "$n" -ge "${DUMP_MAX:-3}" ] && { say "more dumps than DUMP_MAX in one run: the rest are only listed"; break; }
    size=$(curl -sSI --max-time 20 "ftp://${HOST:-$H}:$FTP_PORT/ux0:/data/$f" 2>/dev/null | tr -d '\r' | awk -F': *' 'tolower($1)=="content-length" {print $2; exit}')
    if [ -n "$size" ] && [ "$size" -gt "$lim" ]; then say "dump $f is $size bytes (over DUMP_MAX_MB): not copied"; continue; fi
    if curl -sSf --max-time "${DUMP_S:-300}" -o "$2/dumps/$f" "ftp://${HOST:-$H}:$FTP_PORT/ux0:/data/$f" 2>/dev/null; then n=$((n + 1)); say "crash dump copied: $2/dumps/$f"; else rm -f "$2/dumps/$f"; fi
  done < "$1"
  rmdir "$2/dumps" 2>/dev/null; return 0
}

# ---- check -------------------------------------------------------------------------------------------------------------------------------
cmd_check() {
  need_host
  local tool
  for tool in curl nc python3; do command -v "$tool" >/dev/null 2>&1 || die "$tool is missing on this machine (README: the Linux VM)"; done
  say "vitacompanion at ${HOST:-$H}: command port $CMD_PORT"
  local out; out=$(vcmd version) || die "no answer on the command port"
  [ -n "$out" ] || die "the command port accepted the connection but sent nothing (console asleep? wake it and close the running application)"
  say "version: $out"
  local apps; apps=$(ls_read ux0:/app/)
  echo "$apps" | grep -qx "$TID" && say "test app $TID is installed" || die "the test app $TID is not installed: install the isolated VPK in VitaShell first (README: owner steps)"
  say "installed port (MSCN00003) is left alone: $(echo "$apps" | grep -qx MSCN00003 && echo present || echo not installed)"
  ls_read ux0:/data/mscharged/ | grep -qx game && say "disc folder ux0:data/mscharged/game present (read-only for the test app)" || say "WARNING: ux0:data/mscharged/game not found: the test app needs the extracted disc there"
  local pn; pn=$(names "$PRIV/" | tr '\n' ' '); say "private folder: ${pn:-(empty or not created yet)}"
}

# ---- stage -------------------------------------------------------------------------------------------------------------------------------
lan_src() { local d; for d in "$KIT/../../native/tools/mp/lan" "$KIT/lan-scripts"; do [ -f "$d/lan_host.txt" ] && { echo "$d"; return; }; done; }
cmd_stage() {
  need_host
  local f
  for f in $ROUTES; do put "$KIT/routes/$f.txt" "$PRIV/$f.txt" || die "upload of routes/$f.txt failed"; done
  for f in "$KIT"/fastboot/routes/fb-*.txt; do [ -f "$f" ] && { put "$f" "$PRIV/$(basename "$f")" || die "upload of fastboot/routes/$(basename "$f") failed"; }; done
  local ls; ls=$(lan_src)
  if [ -n "$ls" ]; then
    put "$ls/lan_host.txt" "$PRIV/lan_host.txt" && put "$ls/lan_join.txt" "$PRIV/lan_join.txt" || die "upload of the LAN scripts failed"
  else
    say "no LAN scripts found (native/tools/mp/lan/lan_host.txt): skipped"
  fi
  local bad=0 t; t=$(mktemp)
  for f in $ROUTES; do
    get "$PRIV/$f.txt" "$t" && [ "$(sha "$t")" = "$(sha "$KIT/routes/$f.txt")" ] || { say "MISMATCH after upload: $f.txt"; bad=1; }
  done
  rm -f "$t"
  [ $bad = 0 ] || die "uploaded routes differ from the kit's"
  say "routes staged in ${PRIV#ux0:/}"
}

# ---- swap --------------------------------------------------------------------------------------------------------------------------------
cmd_swap() {
  need_host
  local v=${1:?usage: session.sh swap 1.0|1.1|1.2|1.3}
  local d="$VERSIONS/$v"
  [ -f "$d/eboot.bin" ] || die "no $d/eboot.bin (run prepare-versions.py first)"
  mkdir -p "$STATE"
  if [ -f "$STATE/version" ] && [ "$(cat "$STATE/version")" = "$v" ] && [ "${FORCE:-}" != 1 ]; then say "console already has $v (state file); FORCE=1 to push again"; return 0; fi
  local t0; t0=$(date +%s)
  # From here the console holds a mix of two versions until every file is in and read back: a failure (Wi-Fi, a die below) must not leave the old state file claiming the old version.
  echo unknown > "$STATE/version"
  vcmd "quit $TID" >/dev/null; sleep "${NAP:-2}"
  local f rel
  while IFS= read -r f; do
    rel=${f#$d/}
    put "$f" "$APP/$rel" || die "upload of $rel failed"
  done < <(find "$d/eboot.bin" "$d/shadercache" -type f 2>/dev/null | sort)
  local t; t=$(mktemp)
  get "$APP/eboot.bin" "$t" || die "could not read eboot.bin back"
  [ "$(sha "$t")" = "$(sha "$d/eboot.bin")" ] || die "eboot.bin on the console differs from versions/$v after the upload"
  rm -f "$t"
  echo "$v" > "$STATE/version"
  say "console now runs version $v ($(( $(date +%s) - t0 )) s)"
  echo "swap-$v,$(awk "BEGIN{printf \"%.2f\", ($(date +%s)-$t0)/60}"),," >> "$TIMES"
}

# ---- eswap: the executable alone ------------------------------------------------------------------------------------------------------------
# For two builds of the same version (r16 against r16 PGO): versions/<name>/eboot.bin goes to ux0:app/CDXMP0001/eboot.bin and nothing else (the shader pack, the
# sce_sys of the first install and the private folder are left alone), read back and compared. The state file keeps the version (1.3) and notes the executable in state/eboot.
cmd_eswap() {
  need_host
  local v=${1:?usage: session.sh eswap NAME   (versions/NAME/eboot.bin; prepare-versions.py --eboot NAME=<vpk>)}
  case "$v" in */*|*..*|"") die "bad executable name $v";; esac
  local f="$VERSIONS/$v/eboot.bin"
  [ -f "$f" ] || die "no $f (run prepare-versions.py --eboot $v=<package.vpk> first)"
  mkdir -p "$STATE"
  if [ -f "$STATE/eboot" ] && [ "$(cat "$STATE/eboot")" = "$v" ] && [ "${FORCE:-}" != 1 ]; then say "console already has the executable $v (state file); FORCE=1 to push again"; return 0; fi
  local t0; t0=$(date +%s)
  echo unknown > "$STATE/eboot"
  vcmd "quit $TID" >/dev/null; sleep "${NAP:-2}"
  put "$f" "$APP/eboot.bin" || die "upload of $v/eboot.bin failed"
  local t; t=$(mktemp)
  get "$APP/eboot.bin" "$t" || die "could not read eboot.bin back"
  [ "$(sha "$t")" = "$(sha "$f")" ] || die "eboot.bin on the console differs from versions/$v after the upload"
  rm -f "$t"
  echo "$v" > "$STATE/eboot"
  [ -f "$STATE/version" ] || echo 1.3 > "$STATE/version"
  say "console now runs the executable $v ($(( $(date +%s) - t0 )) s, eboot.bin alone)"
  [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"
  echo "eswap-$v,$(awk "BEGIN{printf \"%.2f\", ($(date +%s)-$t0)/60}"),,," >> "$TIMES"
}

# The test app's np-player.txt is the owner's own file until the first run of a session overwrites it: it is read once (a missing file is noted as such) and kept in
# state/owner-np-player.txt; `restore` puts it back, or deletes the one the session wrote when there was none.
owner_backup() {
  mkdir -p "$STATE"
  [ -f "$STATE/owner-np-player.state" ] && return 0
  local t; t=$(mktemp)
  if get1 "$PRIV/np-player.txt" "$t" 2>/dev/null; then cp "$t" "$STATE/owner-np-player.txt"; echo present > "$STATE/owner-np-player.state"; say "the owner's np-player.txt is kept in $STATE/owner-np-player.txt"
  else echo none > "$STATE/owner-np-player.state"; say "no np-player.txt on the console before the session (restore deletes the session's)"; fi
  rm -f "$t"
}
cmd_restore() {
  need_host
  [ -f "$STATE/owner-np-player.state" ] || { say "restore: nothing was kept (no run has been made in this session)"; return 0; }
  if [ "$(cat "$STATE/owner-np-player.state")" = present ]; then
    put "$STATE/owner-np-player.txt" "$PRIV/np-player.txt" || die "could not put the owner's np-player.txt back"
    local t; t=$(mktemp); get "$PRIV/np-player.txt" "$t" && [ "$(sha "$t")" = "$(sha "$STATE/owner-np-player.txt")" ] || die "np-player.txt on the console differs from the kept one"; rm -f "$t"
    say "the owner's np-player.txt is back"
  else
    del "$PRIV/np-player.txt"; say "the np-player.txt of the session is removed (there was none before)"
  fi
}

# ---- run ---------------------------------------------------------------------------------------------------------------------------------
plan_field() {  # plan_field ID FIELD (route|version|file|settings|role) from plan.json
  python3 - "$KIT/plan.json" "$1" "$2" <<'EOF'
import json, sys
plan = json.load(open(sys.argv[1]))
for r in plan["runs"]:
    if r["id"] == sys.argv[2]:
        print(r[sys.argv[3]]); break
EOF
}

plan_fb() {  # plan_fb ID FIELD (route|file|env|minutes) of the fast-boot variant from plan.json
  python3 - "$KIT/plan.json" "$1" "$2" <<'EOF'
import json, sys
plan = json.load(open(sys.argv[1]))
for r in plan["runs"]:
    if r["id"] == sys.argv[2] and r.get("fb"):
        print(r["fb"][sys.argv[3]]); break
EOF
}

est_seconds() {  # est_seconds ROUTE: seconds of the route from earlier runs of this session, else 1.15 x the estimate
  local route=$1 m="" id r
  if [ -f "$TIMES" ]; then
    m=$(awk -F, -v r="$route" '$4==r && $5=="done" && $3+0>0 {if (min==""||$3<min) min=$3} END{print min}' "$TIMES")
  fi
  if [ -n "$m" ]; then echo "$(( m - 15 ))"; return; fi
  python3 - "$KIT" "$route" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import matrix
print(int((matrix.ROUTE_MIN.get(sys.argv[2]) or matrix.FB_ROUTE_MIN[sys.argv[2]]) * 60 * 1.15))
EOF
}

DONE_RE='script exit|settings screen done|\[settings\] test script finished'
done_ok() { [ -f "$1" ] && grep -aqE "$DONE_RE" "$1"; }

slice_log() {   # slice_log RAW OUT: from the last "=====...build" header on
  local n; n=$(grep -an "=====.*build" "$1" | tail -1 | cut -d: -f1)
  if [ -n "$n" ]; then tail -n +"$n" "$1" > "$2"; else cp "$1" "$2"; fi
}

wait_for_exit() {   # wait_for_exit LOCAL_LOG_PATH FIRST_POLL_S MAX_S -> sets WAIT_STATUS (done|timeout|stalled), WAIT_S
  local out=$1 first=$2 max=$3 start now last_size=-1 same=0 size pol=${POLL_S:-10} stall=${STALL_EFF:-${STALL_S:-90}}
  start=$(date +%s); WAIT_STATUS=timeout
  if [ -n "${EARLY_POLL:-}" ]; then
    # A stability route measures nothing, so the console may be asked early and often, with an FTP SIZE (no transfer): a crash is seen within the stall time,
    # not at the end of a 24-minute route. A measuring route is left alone until its end: an FTP server on the console must not disturb its frame times.
    local esize elast=-1 esame=0
    while [ $(( $(date +%s) - start )) -lt "$first" ]; do
      sleep "$pol"
      esize=$(rsize "$PRIV/log.txt")
      if [ -n "$esize" ]; then
        if [ "$esize" = "$elast" ]; then esame=$((esame + 1)); else esame=0; fi
        elast=$esize
        [ $((esame * pol)) -ge "$stall" ] && break
      fi
    done
  else
    sleep "$first"
  fi
  while :; do
    now=$(date +%s)
    if get1 "$PRIV/log.txt" "$out.poll" 2>/dev/null; then
      slice_log "$out.poll" "$out.poll2"      # the log may still hold the end of an earlier run
      if grep -aqE "$DONE_RE" "$out.poll2"; then WAIT_STATUS=done; break; fi
      if grep -aq "EXIT STUCK" "$out.poll2"; then WAIT_STATUS=stalled; say "WARNING: the end of this run hung and the harness left by its watchdog (exit17): the last [exit] step line and the EXIT STUCK line of the log name the step"; break; fi
      size=$(wc -c < "$out.poll" | tr -d ' ')
      if [ "$size" = "$last_size" ]; then same=$((same + 1)); else same=0; fi
      last_size=$size
      if [ $((same * pol)) -ge "$stall" ]; then WAIT_STATUS=stalled; break; fi    # STALL_S (90) seconds without a new byte: the app has stopped or crashed
    fi
    [ $((now - start)) -ge "$max" ] && break
    sleep "$pol"
  done
  WAIT_S=$(( $(date +%s) - start ))
  rm -f "$out.poll" "$out.poll2"
}

wait_log_start() {   # the log file was deleted before the launch: it exists again once the app runs (LAUNCH_WAIT_S, default 75)
  local t=0 f; f=$(mktemp)
  while [ "$t" -lt "${LAUNCH_WAIT_S:-75}" ]; do
    if get1 "$PRIV/log.txt" "$f" 2>/dev/null; then rm -f "$f"; return 0; fi
    sleep "${LAUNCH_POLL_S:-5}"; t=$((t + ${LAUNCH_POLL_S:-5}))
  done
  rm -f "$f"; return 1
}

# clock_check ID ENVFILE LOG (clockfix17): the player build runs clock policy max (500 asked 1 s after the start, 444 if refused, nothing asked when the core already measures
# 470 or more), so the log of a normal run says ~500, ~494 or 444; only a run whose env sets MSC_VITA_CPU_MHZ=444 (clock444, clock444d) must say 444. It only warns, never stops.
clock_check() {
  local cid=$1 cenv=$2 clog=$3 line meas now
  if grep -aqE '^MSC_VITA_CPU_MHZ=444' "$cenv" 2>/dev/null; then
    grep -aqE "(ARM|CPU) 444 MHz" "$clog" || say "WARNING: $cid sets MSC_VITA_CPU_MHZ=444 but the log does not say ARM 444 MHz (a PSVshellPlus profile locking 500 for this title? switch it off for this run)"
    return 0
  fi
  line=$(grep -a "clocks: policy" "$clog" | head -1)
  [ -n "$line" ] || { say "WARNING: no 'clocks: policy' line in the log of $cid: the build has no clock record"; return 0; }
  meas=$(printf '%s\n' "$line" | sed -n 's/.*measured [0-9]* before, \([0-9]*\) after.*/\1/p')
  now=$(grep -a "the ARM core measures" "$clog" | tail -1 | sed -n 's/.*the ARM core measures \([0-9]*\) MHz.*/\1/p')
  say "$cid clock: $(printf '%s\n' "$line" | sed -n 's/.*\(requested ARM [^;]*\);.*/\1/p'), measured ${meas:-?} MHz after the request, ${now:-?} MHz at the last 30 s look"
  [ -n "$now" ] && meas=$now
  case "$meas" in ''|*[!0-9]*) ;; *) [ "$meas" -ge 470 ] || say "WARNING: $cid ran at about $meas MHz, under 470: the 500 request was refused or a profile is not on (not an error; read it against clock444)";; esac
  return 0
}

cmd_run() {
  need_host
  local id=${1:?usage: session.sh run <run id>}
  local env="$KIT/env/$id.txt" fbroute=""
  [ -f "$env" ] || die "no env file $env"
  if [ "$FASTBOOT" = 1 ] && [ -f "$KIT/env-fb/$id.txt" ]; then env="$KIT/env-fb/$id.txt"; fbroute=$(plan_fb "$id" route); fi
  local dir="$OUT/$id"
  mkdir -p "$dir" "$STATE"
  if done_ok "$dir/log.txt" && [ "${FORCE:-}" != 1 ]; then say "$id: finished earlier ($dir/log.txt), skipped (FORCE=1 to redo)"; return 0; fi
  local route version want t0 settings shots=0
  route=$(plan_field "$id" route); version=$(plan_field "$id" version); settings=$(plan_field "$id" settings)
  [ -n "$fbroute" ] && route=$fbroute      # (the fast-boot route: est_seconds and the times file know it by its name)
  { [ "$route" = look ] || [ -n "$settings" ] || grep -q '^MSC_VITA_SHOT=' "$env"; } && shots=1
  if [ -f "$STATE/version" ] && [ "$(cat "$STATE/version")" != "$version" ]; then die "$id needs version $version but the console has $(cat "$STATE/version"): run 'session.sh swap $version'"; fi
  [ -f "$STATE/version" ] || say "note: no swap done in this session; assuming the installed test app is version 1.3"
  t0=$(date +%s)
  owner_backup
  # the route the env file names must be on the console: the kit's own (routes/ or fastboot/routes/), uploaded and read back by its hash
  local rname rsrc; rname=$(sed -n 's#^MSC_INPUT_SCRIPT=.*/##p' "$env" | head -1)
  if [ -n "$rname" ]; then
    for rsrc in "$KIT/routes/$rname" "$KIT/fastboot/routes/$rname"; do [ -f "$rsrc" ] && break; rsrc=""; done
    [ -n "$rsrc" ] || die "$id: the route $rname named by $env is not in the kit"
    put "$rsrc" "$PRIV/$rname" || die "upload of the route $rname failed"
    local rt; rt=$(mktemp); get "$PRIV/$rname" "$rt" && [ "$(sha "$rt")" = "$(sha "$rsrc")" ] || die "the route $rname on the console differs from the kit's"; rm -f "$rt"
  fi
  put "$env" "$PRIV/np-player.txt" || die "upload of the env file failed"
  local t; t=$(mktemp); get "$PRIV/np-player.txt" "$t" && [ "$(sha "$t")" = "$(sha "$env")" ] || die "np-player.txt on the console differs from $env"; rm -f "$t"
  del "$PRIV/log.txt"
  # del is best effort; an old log.txt left on the console would let wait_log_start pass at once and its "script exit" end the wait of a run that never started
  if names "$PRIV/" | grep -qx log.txt; then del "$PRIV/log.txt"; names "$PRIV/" | grep -qx log.txt && die "the old log.txt could not be deleted from the console: $id not started"; fi
  [ $shots = 1 ] && for f in $(names "$PRIV/shots/"); do del "$PRIV/shots/$f"; done
  if [ -n "$settings" ]; then   # the Settings screen's test hook: opened at the next start, its words are the keys (np_vita_settings.cpp)
    local m; m=$(mktemp); printf '%s\n' "$settings" > "$m"; put "$m" "$PRIV/np-settings-open" || die "upload of np-settings-open failed"; rm -f "$m"
  fi
  local dumps0; dumps0=$(dump_names)
  vcmd "quit $TID" >/dev/null; sleep "${NAP:-3}"
  say "$id: launching ($route route$([ -n "$fbroute" ] && echo ", FAST BOOT"), version $version, executable $(cat "$STATE/eboot" 2>/dev/null || echo installed)): $(grep -v '^#' "$env" | grep -v MSC_INPUT_SCRIPT | grep -v -e MSC_FRAME_TIMING -e MSC_LAT= -e MSC_WATCH | tr '\n' ' ')"
  local reply; reply=$(vcmd "launch $TID"); echo "    $reply"
  local est first max stab=0
  case "$route" in cstab|cintro|cend) stab=1;; esac
  est=$(est_seconds "$route"); first=${FIRST_POLL_S:-$est}; max=$(( est * 2 + 120 ))
  local launched; launched=$(date +%s)
  # The app must start: its log appears. If it does not (a crash dialog on the screen, a launch that was refused), quit, wait, and launch once more.
  if ! wait_log_start; then
    say "$id: no log after the launch: quit, wait, launch again"
    vcmd "quit $TID" >/dev/null; sleep "${RELAUNCH_NAP:-10}"; reply=$(vcmd "launch $TID"); echo "    $reply"
    if ! wait_log_start; then
      WAIT_STATUS=nolaunch; WAIT_S=0
      echo "$WAIT_STATUS" > "$dir/status.txt"
      [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"
      echo "$id,$(awk "BEGIN{printf \"%.2f\", ($(date +%s)-$t0)/60}"),0,$route,nolaunch" >> "$TIMES"
      say "$id: the app did not start (see the screen: a dialog?)"
      return 1
    fi
  fi
  if [ $stab = 1 ]; then EARLY_POLL=1; [ -n "${STALL_S:-}" ] || STALL_EFF=150; POLL_S=${POLL_S:-30}; fi
  wait_for_exit "$dir/log.raw" "$first" "$max"
  unset EARLY_POLL STALL_EFF
  get "$PRIV/log.txt" "$dir/log.raw" || die "could not pull log.txt"
  slice_log "$dir/log.raw" "$dir/log.txt"
  local nshots=0
  if [ $shots = 1 ]; then
    mkdir -p "$dir/shots"
    for f in $(names "$PRIV/shots/"); do get "$PRIV/shots/$f" "$dir/shots/$f" && nshots=$((nshots + 1)); done
  fi
  [ -n "$settings" ] && del "$PRIV/np-settings-open"     # (the screen deletes it when it opens; a run that never opened it must not leave it for the next)
  echo "$WAIT_STATUS" > "$dir/status.txt"
  comm -13 <(printf '%s\n' "$dumps0") <(dump_names) | grep . > "$dir/dumps.txt" || rm -f "$dir/dumps.txt"
  [ -f "$dir/dumps.txt" ] && { say "CRASH DUMP written during $id: $(tr '\n' ' ' < "$dir/dumps.txt")"; dump_copy "$dir/dumps.txt" "$dir"; }
  # vitaGL's own GPU crash dump (gpucrash.sgx, in the private folder): copied and removed so the next run starts clean
  if names "$PRIV/" | grep -qx gpucrash.sgx; then get "$PRIV/gpucrash.sgx" "$dir/gpucrash.sgx" && say "GPU crash dump copied: $dir/gpucrash.sgx" && del "$PRIV/gpucrash.sgx"; fi
  # a run that did not end cleanly leaves the app crashed or hung: make sure it is gone before anything else starts
  [ "$WAIT_STATUS" = done ] || { vcmd "quit $TID" >/dev/null; sleep "${NAP:-3}"; }
  local route_s=$(( $(date +%s) - launched ))
  local minutes; minutes=$(awk "BEGIN{printf \"%.2f\", ($(date +%s)-$t0)/60}")
  [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"
  echo "$id,$minutes,$WAIT_S,$route,$WAIT_STATUS" >> "$TIMES"
  # the route time is column 3: the wait as seen from the launch (first poll at ~the route's length)
  say "$id: $WAIT_STATUS after $route_s s, $minutes console minutes, log $(wc -l < "$dir/log.txt" | tr -d ' ') lines, $nshots screenshots"
  grep -a -e "\[run\] first frame" -e "\[vita\] clocks:" "$dir/log.txt" | head -2 | cut -c1-170
  [ -n "$settings" ] || clock_check "$id" "$env" "$dir/log.txt"
  [ -n "$settings" ] || grep -aq "\[frame-timing\]" "$dir/log.txt" || say "WARNING: no [frame-timing] lines in the log: is this the logging build? (README: build)"
  [ "$WAIT_STATUS" = done ] || say "WARNING: $id did not finish ($WAIT_STATUS); the log is kept, the run counts as incomplete"
  [ "$WAIT_STATUS" = done ]
}

# ---- lan ---------------------------------------------------------------------------------------------------------------------------------
cmd_lan() {
  local leg=${1:?usage: session.sh lan lan-base|lan-auto}
  [ -n "$H" ] && [ -n "$H2" ] || die "export VITA_HOST (host console) and VITA_HOST2 (joining console)"
  [ -f "$KIT/lan/$leg-host.txt" ] || die "no lan/$leg-host.txt"
  local dir="$OUT/$leg"; mkdir -p "$dir"
  local t0; t0=$(date +%s)
  local role HOSTX
  for role in host join; do
    HOSTX=$H; [ $role = join ] && HOSTX=$H2
    HOST=$HOSTX put "$KIT/lan/$leg-$role.txt" "$PRIV/np-player.txt" || die "upload of the $role env file failed"
    HOST=$HOSTX del "$PRIV/log.txt"
    HOST=$HOSTX vcmd "quit $TID" >/dev/null
  done
  sleep "${NAP:-3}"
  say "$leg: launching the host console, then the joining one"
  HOST=$H vcmd "launch $TID"; sleep "${NAP:-12}"; HOST=$H2 vcmd "launch $TID"
  local launched; launched=$(date +%s)
  local est; est=$(python3 -c "import sys; sys.path.insert(0,'$KIT'); import matrix; print(int(matrix.LAN_LEG_MIN*60))")
  sleep "${FIRST_POLL_S:-$(( est - 30 ))}"
  local max=$(( est * 2 )) status=timeout
  while [ $(( $(date +%s) - launched )) -lt $max ]; do
    local a=0 b=0
    HOST=$H get "$PRIV/log.txt" "$dir/host.raw" 2>/dev/null && { slice_log "$dir/host.raw" "$dir/host.cut"; grep -aq "script exit" "$dir/host.cut" && a=1; }
    HOST=$H2 get "$PRIV/log.txt" "$dir/join.raw" 2>/dev/null && { slice_log "$dir/join.raw" "$dir/join.cut"; grep -aq "script exit" "$dir/join.cut" && b=1; }
    [ $a = 1 ] && [ $b = 1 ] && { status=done; break; }
    sleep "${POLL_S:-10}"
  done
  HOST=$H get "$PRIV/log.txt" "$dir/host.raw"; HOST=$H2 get "$PRIV/log.txt" "$dir/join.raw"
  slice_log "$dir/host.raw" "$dir/host.log.txt"; slice_log "$dir/join.raw" "$dir/join.log.txt"; rm -f "$dir/host.cut" "$dir/join.cut"
  [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"
  echo "$leg,$(awk "BEGIN{printf \"%.2f\", ($(date +%s)-$t0)/60}"),$(( $(date +%s) - launched )),lan,$status" >> "$TIMES"
  say "$leg: $status"
  python3 "$KIT/analyse.py" --lan "$dir/host.log.txt" "$dir/join.log.txt"
}

# ---- all / report ------------------------------------------------------------------------------------------------------------------------
# One loop for all, core, full and night: each run in a subshell (a STOP inside it ends that run only), a failed swap skips its version's runs,
# MAX_FAILS runs in a row that fail without the console answering `version` end the session (resume: run the same command again).
#
# A night list (gen_kit.py --night --list) also has `night`, `phase` and `end` lines and a phase on every run: the loop then keeps the clock of the night
# (NIGHT_MIN overrides its length, default 480 minutes; the clock starts at the first run and survives a restart of this script within 14 hours), leaves a phase
# when its deadline has passed (the rest of its runs are written to skipped.txt, not run), starts no run that would end after the night does, swaps the console's
# version only when a run needs it (a failed swap skips that version's runs for good), writes a checkpoint report whenever a phase ends, and is more patient with
# a console that does not answer (NIGHT_MAX_FAILS 8, DEAD_WAIT_S 120: a Wi-Fi drop or a restart of the router is not the end of the night).
elapsed_min() { echo $(( ($(date +%s) - ${NIGHT_T0:-$(date +%s)}) / 60 )); }

night_clock() {   # night_clock: sets NIGHT_T0 from the state file (a night that is still running or was interrupted), or starts it now
  mkdir -p "$STATE"
  local f="$STATE/night-start" t now; now=$(date +%s)
  if [ -f "$f" ] && [ "${NIGHT_RESTART:-}" != 1 ]; then t=$(cat "$f"); [ $((now - t)) -lt $(( ${NIGHT_RESUME_H:-14} * 3600 )) ] && { NIGHT_T0=$t; return; }; fi
  NIGHT_T0=$now; echo "$now" > "$f"
}

progress() {   # progress ID PHASE STATUS: one line per run in night-progress.txt (elapsed minutes, phase, run, status), also on screen
  [ -n "${NIGHT:-}" ] || return 0
  [ -f "$OUT/night-progress.txt" ] || echo "elapsed_min,phase,id,status" > "$OUT/night-progress.txt"
  echo "$(elapsed_min),$2,$1,$3" >> "$OUT/night-progress.txt"
}

checkpoint() {   # checkpoint: the reports from what is on disk so far (never contacts the console); a failure here never stops the night
  [ -n "${NIGHT:-}" ] || return 0
  say "checkpoint report (elapsed $(elapsed_min) min)"
  ( cmd_report > "$OUT/report.txt" 2>&1 ) || say "the checkpoint report failed (see $OUT/report.txt); the night goes on"
  return 0
}

run_list() {   # run_list LISTFILE
  local kind a b c ver ph skip="" fails=0 cur_ph="" deadline=0 end_min=${NIGHT_MIN:-} failed_ver=" " nolaunch=0 done_ph=""
  local max_fails=${MAX_FAILS:-3} dead_wait=${DEAD_WAIT_S:-60}
  unset NIGHT
  local estate=ok
  while IFS=$'\t' read -r kind a b c ver ph; do
    case "$kind" in
      fastboot) FASTBOOT=$a; say "fast boot: $([ "$a" = 1 ] && echo ON || echo off) for the runs that have a fast-boot route";;
      eswap)
        if ( cmd_eswap "$a" ); then estate=ok; else
          estate=failed; say "eswap to $a failed: the runs up to the next eswap are skipped (later: session.sh eswap $a, then the same command again)"
          [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"; echo "eswap-$a,0,0,eswap,failed" >> "$TIMES"
        fi;;
      restore) ( cmd_restore ) || say "restore failed";;
      night)
        NIGHT=1; night_clock
        [ -n "$end_min" ] || end_min=$b
        max_fails=${MAX_FAILS:-${NIGHT_MAX_FAILS:-8}}; dead_wait=${DEAD_WAIT_S:-120}
        say "the night: ends at minute $end_min (clock started $(date -d @$NIGHT_T0 +%H:%M 2>/dev/null || date -r $NIGHT_T0 +%H:%M 2>/dev/null)); elapsed $(elapsed_min) min"
        ;;
      phase)
        [ -n "$cur_ph" ] && [ "$cur_ph" != "$a" ] && checkpoint
        cur_ph=$a; deadline=$b
        [ "$a" != end ] && say "phase $a (deadline minute $b, elapsed $(elapsed_min))"
        ;;
      swap)
        [ -n "${NIGHT:-}" ] && continue          # lazy: the swap is made when a run needs it
        if ( cmd_swap "$a" ); then skip=""; else
          skip=$a; say "swap to $a failed: its runs are skipped (later: session.sh swap $a, then the same command again)"
          [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"; echo "swap-$a,0,0,swap,failed" >> "$TIMES"
        fi;;
      run)
        if [ -n "${NIGHT:-}" ]; then
          local el; el=$(elapsed_min)
          if done_ok "$OUT/$a/log.txt" && [ "${FORCE:-}" != 1 ]; then say "$a: finished earlier, skipped"; continue; fi
          if [ "$el" -ge "$deadline" ]; then
            say "$a skipped: phase $ph is past its deadline (minute $deadline, now $el)"; echo "$a,$ph,deadline" >> "$OUT/skipped.txt"; progress "$a" "$ph" skipped-deadline; continue
          fi
          if [ "$(awk "BEGIN{print ($el + $c > ${end_min:-480}) ? 1 : 0}")" = 1 ]; then
            say "$a skipped: it would end after the night ($el + $c min > ${end_min:-480})"; echo "$a,$ph,night-end" >> "$OUT/skipped.txt"; progress "$a" "$ph" skipped-night-end; continue
          fi
          case "$failed_ver" in *" $ver "*) say "$a skipped: version $ver could not be put on the console"; echo "$a,$ph,swap-failed" >> "$OUT/skipped.txt"; progress "$a" "$ph" skipped-swap; continue;; esac
          if [ ! -f "$STATE/version" ] || [ "$(cat "$STATE/version")" != "$ver" ]; then
            if ! ( cmd_swap "$ver" ); then
              failed_ver="$failed_ver$ver "
              say "swap to $ver failed: the runs of $ver are skipped"; [ -f "$TIMES" ] || echo "id,minutes,route_s,route,status" > "$TIMES"; echo "swap-$ver,0,0,swap,failed" >> "$TIMES"
              echo "$a,$ph,swap-failed" >> "$OUT/skipped.txt"; progress "$a" "$ph" skipped-swap; continue
            fi
          fi
        else
          if [ -n "$skip" ] && [ "$ver" = "$skip" ]; then say "$a skipped: version $ver is not on the console"; continue; fi
          if [ "$estate" = failed ]; then say "$a skipped: the executable it needs is not on the console"; continue; fi
        fi
        if ( cmd_run "$a" ); then fails=0; nolaunch=0; progress "$a" "${ph:-}" done; else
          say "$a did not finish: continuing (rerun it afterwards with FORCE=1 session.sh run $a)"
          local st="?"; [ -f "$OUT/$a/status.txt" ] && st=$(cat "$OUT/$a/status.txt")
          progress "$a" "${ph:-}" "$st"
          [ "$st" = nolaunch ] && nolaunch=$((nolaunch + 1)) || nolaunch=0
          if [ -n "$(vcmd version 2>/dev/null)" ]; then
            fails=0
            [ "$nolaunch" -ge 4 ] && die "the app did not start in $nolaunch runs in a row although the console answers: look at its screen (a dialog? a full card?), then run the same command again to resume"
          else
            fails=$((fails + 1)); say "the console does not answer ($fails in a row)"
            [ "$fails" -ge "$max_fails" ] && die "the console stopped answering: wake it, start vitacompanion, then run the same command again to resume"
            sleep "$dead_wait"
          fi
        fi;;
    esac
  done < "$1"
  if [ -n "$cur_ph" ]; then checkpoint; fi
  return 0
}

cmd_all() {
  local tier=1
  [ "${1:-}" = "--tier" ] && tier=${2:-1}
  mkdir -p "$OUT"
  python3 "$KIT/gen_kit.py" --list --tier "$tier" > "$OUT/plan.list" || die "could not list the session"
  cmd_check || exit 1
  cmd_stage
  run_list "$OUT/plan.list"
  say "matrix done. LAN legs next: session.sh lan lan-base ; session.sh lan lan-auto (two consoles). Then: session.sh report"
}

cmd_core() {
  mkdir -p "$OUT"
  python3 "$KIT/gen_kit.py" --list --tier 0 > "$OUT/plan-core.list" || die "could not list the core session"
  cmd_check || exit 1
  cmd_stage
  run_list "$OUT/plan-core.list"
  cmd_report > "$OUT/report.txt" 2>&1
  say "core session done. Read $OUT/ISSUES.md first, then $OUT/core.txt and $OUT/looks-core.html"
}

cmd_full() {
  mkdir -p "$OUT"
  python3 "$KIT/gen_kit.py" --list --tier 3 --staged > "$OUT/plan-full.list" || die "could not list the full session"
  cmd_check || exit 1
  cmd_stage
  run_list "$OUT/plan-full.list"
  if [ -n "$H2" ]; then
    if ( HOST=$H2 cmd_stage ); then
      for l in lan-base lan-auto; do ( cmd_lan "$l" ) || say "$l did not finish"; done
    else
      say "staging the second Vita failed: the LAN legs are skipped"
    fi
  else
    say "VITA_IP2 not set: the two-Vita LAN legs are skipped"
  fi
  cmd_report > "$OUT/report.txt" 2>&1
  say "full session done. Read $OUT/ISSUES.md first, then $OUT/core.txt, $OUT/table.txt and $OUT/looks-core.html"
}

cmd_night() {
  mkdir -p "$OUT"
  if [ -n "${NIGHT_LIST:-}" ]; then   # a hand-made or trimmed list (tests, resuming with fewer runs)
    cp "$NIGHT_LIST" "$OUT/plan-night.list" || die "cannot read NIGHT_LIST"
  else
    python3 "$KIT/gen_kit.py" --night --list > "$OUT/plan-night.list" || die "could not list the night"
  fi
  grep '^# dropped' "$OUT/plan-night.list" | sed 's/^# dropped\t//' > "$OUT/not-in-this-build.txt"
  cmd_check || exit 1
  cmd_stage
  # whatever happens from here (a STOP, Ctrl-C, the last run) the reports are written from what is on disk
  trap 'say "writing the reports"; cmd_report > "$OUT/report.txt" 2>&1; python3 "$KIT/issues.py" "$OUT" --morning --pixels >> "$OUT/report.txt" 2>&1; say "night over. Read $OUT/MORNING.md first, then $OUT/ISSUES.md"' EXIT
  run_list "$OUT/plan-night.list"
  if [ -n "$H2" ]; then
    if ( HOST=$H2 cmd_stage ); then
      for l in lan-base lan-auto; do ( cmd_lan "$l" ) || say "$l did not finish"; done
    fi
  fi
}

cmd_addon() {
  mkdir -p "$OUT"
  python3 "$KIT/gen_kit.py" --list --tier 3 --staged --manual --only-tier 3 > "$OUT/plan-addon.list" || die "could not list the add-ons"
  run_list "$OUT/plan-addon.list"
}

cmd_report() {
  local rc=0
  python3 "$KIT/analyse.py" "$OUT" --csv "$OUT/table.csv" "$@" | tee "$OUT/table.txt"
  local st=${PIPESTATUS[0]}
  [ "$st" = 0 ] || { echo "REPORT: analyse.py FAILED (exit $st): table.txt is incomplete" >&2; rc=1; }
  if python3 "$KIT/analyse.py" "$OUT" --core > "$OUT/core.txt" 2>&1; then cat "$OUT/core.txt"; else echo "REPORT: analyse.py --core FAILED: see $OUT/core.txt" >&2; rc=1; fi
  ls -d "$OUT"/look*/shots >/dev/null 2>&1 && python3 "$KIT/looks.py" "$OUT"
  ls -d "$OUT"/*/shots >/dev/null 2>&1 && python3 "$KIT/looks.py" "$OUT" --core
  python3 "$KIT/issues.py" "$OUT" && echo "ISSUES: $OUT/ISSUES.md"
  ls -d "$OUT"/b-npipe-1 "$OUT"/s-stab >/dev/null 2>&1 && python3 "$KIT/issues.py" "$OUT" --morning && echo "MORNING: $OUT/MORNING.md"
  for l in lan-base lan-auto; do
    [ -f "$OUT/$l/host.log.txt" ] && { echo; echo "== $l"; python3 "$KIT/analyse.py" --lan "$OUT/$l/host.log.txt" "$OUT/$l/join.log.txt"; }
  done
  return $rc
}

case "${1:-}" in
  check)  shift; cmd_check "$@";;
  stage)  shift; cmd_stage "$@";;
  swap)   shift; cmd_swap "$@";;
  eswap)  shift; cmd_eswap "$@";;
  restore) shift; cmd_restore "$@";;
  run)    shift; cmd_run "$@";;
  clockcheck) shift; [ $# = 3 ] || die "usage: session.sh clockcheck <run id> <env file> <log>"; clock_check "$@";;
  lan)    shift; cmd_lan "$@";;
  all)    shift; cmd_all "$@";;
  core)   shift; cmd_core "$@";;
  full)   shift; cmd_full "$@";;
  night)  shift; cmd_night "$@";;
  addon)  shift; cmd_addon "$@";;
  list)   shift; [ -f "${1:-}" ] || die "usage: session.sh list <file of run/swap lines as gen_kit.py --list writes them>"; mkdir -p "$OUT"; run_list "$1";;
  report) shift; cmd_report "$@";;
  *) sed -n 2,30p "$0"; exit 2;;
esac
