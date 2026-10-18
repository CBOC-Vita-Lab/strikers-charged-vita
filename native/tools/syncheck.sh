#!/bin/bash
# usage: tools/syncheck.sh <target> [subset-prefix] [top-N]
# Clears the logs/ok-markers of the subset, re-runs the syntax check, prints failing TU count and top errors.
# (for more than ~10 files run it through /home/claude/msc/tools/heavy.sh -t <s>)
# If /home/claude/msc/build/np/.all_targets exists, the other targets (armhf, vita) are checked too
# (lets one queued heavy.sh slot cover everything).
T=${1:-host32}; S=${2:-src}; N=${3:-30}
cd /home/claude/msc/np
run_target() {
  local T=$1 O=/home/claude/msc/build/np/$1
  mkdir -p $O; rm -rf "$O/log/$S"* "$O/syn/$S"*
  make -k -j2 TARGET=$T SUBSET=$S syntax > $O/syn.log 2>&1
  echo "== $T: failing TUs: $(grep -c '^FAIL' $O/syn.log)"
  tools/census.sh $T $N
  grep '^FAIL' $O/syn.log | head -40
}
run_target $T
if [ -e /home/claude/msc/build/np/.all_targets ]; then
  for t in armhf vita; do [ $t = $T ] || run_target $t; done
fi
