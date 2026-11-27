#!/bin/bash
# netns_run.sh <command> [args...] : run the command in an EMPTY network namespace (tools15; Galaxy's seedwalk run_all.sh does the same for every leg).
# Only the loopback exists (brought up, so local sockets work); there is no route and no other interface, so an unattended emulator run can never reach a live service
# (the game's online code resolves and connects to nothing: "Network is unreachable"). Needs root or CAP_SYS_ADMIN (`unshare -n`).
# Fail closed: when the namespace cannot be made, or the namespace has any interface besides lo, the command is NOT run (exit 125).
# Opt out: HV_NETNS=0 runs the command directly (a line on stderr says so). Used by tools/harvest/harvest_one.sh and /home/claude/s13/tools/final-gate.sh.
[ $# -gt 0 ] || { echo "usage: netns_run.sh <command> [args...]" >&2; exit 2; }
if [ "${HV_NETNS:-1}" = 0 ]; then echo "netns_run: HV_NETNS=0: NOT isolated from the network: $1" >&2; exec "$@"; fi
unshare -n true 2>/dev/null || { echo "netns_run: cannot make a network namespace (unshare -n needs root / CAP_SYS_ADMIN): not running the command; HV_NETNS=0 runs it without isolation" >&2; exit 125; }
exec unshare -n -- bash -c '
  python3 -I - <<"PY" || { echo "netns_run: cannot bring lo up in the new namespace: not running the command" >&2; exit 125; }
import fcntl, socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
fcntl.ioctl(s, 0x8914, struct.pack("16sH22x", b"lo", 0x1 | 0x40))   # SIOCSIFFLAGS: IFF_UP | IFF_RUNNING
PY
  # nothing but lo may be here (a namespace that kept a real interface is no isolation)
  if [ "$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d " " | grep -vxc lo)" != 0 ]; then echo "netns_run: the namespace has an interface besides lo: not running the command" >&2; exit 125; fi
  exec "$@"' netns_run "$@"
