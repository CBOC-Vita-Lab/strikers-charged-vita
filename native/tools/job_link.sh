#!/bin/bash
# One heavy.sh slot: for each target build every object, (re)generate the SDK stubs, link.
#   usage: tools/job_link.sh <target> [make vars...]       (env NOSTUBS=1: keep the committed stubs)
# If /home/claude/msc/build/np/.targets exists its content (e.g. "host32 armhf") replaces <target>, so a job
# that is already queued can be extended.   e.g.  heavy.sh -t 1700 tools/job_link.sh host32 OPT=-O1 DEBUG=-g0
T0=$1; shift
cd /home/claude/msc/np
TARGETS=$T0; [ -e /home/claude/msc/build/np/.targets ] && TARGETS=$(cat /home/claude/msc/build/np/.targets)
for T in $TARGETS; do
  echo "== objs $T $(date +%T)"
  make -k -j2 TARGET=$T "$@" objs 2>&1 | tail -60
  missing=$(make -n TARGET=$T "$@" objs 2>/dev/null | grep -c 'clang\|gcc\|g++')
  echo "== objects still to build: $missing"
  [ "$missing" = 0 ] || continue
  # stubs are generated from the first target only (the symbol set is the same on every target)
  if [ "$T" = "${TARGETS%% *}" ] && { [ -z "$NOSTUBS" ] || [ "$(wc -c < platform/stubs/sdk_stubs.c)" -lt 500 ]; }; then
    echo "== stubs $T $(date +%T)"
    make TARGET=$T "$@" stubs 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc' | tail -20
  fi
  echo "== link $T $(date +%T)"
  make TARGET=$T "$@" 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-' | tail -30
  ls -la /home/claude/msc/build/np/$T/np-$T 2>&1 | tail -1
done
echo "== done $(date +%T)"
