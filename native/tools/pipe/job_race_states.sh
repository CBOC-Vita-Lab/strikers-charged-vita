#!/bin/bash
# NPP2 job (one heavy slot): the race-check plugin on frames of states other than open play, with every state forked (MSC_FS_STATES=0x3FFFFF):
# the walk-out cut-scene (the all-states proof run differs from nofork in 18 frame hashes there), the stadium-select preview movie, the title.
#   tools/heavy.sh -t 700 env V=a6 tools/pipe/job_race_states.sh
cd /home/claude/msc/npw/W5PIPE || exit 1
V=${V:-a6}
for spec in "wlkout 2301 2266 2300" "stadsl 1800 1752 1790" "titles 660 596 650"; do
  set -- $spec
  echo "=== $V $1: frames $3-$4 of $2"
  TMO=${TMO:-200} TOP=40 tools/pipe/run_race.sh $V $1 $2 $3 $4 MSC_FS_STATES=0x3FFFFF 2>&1 | cut -c1-300 | grep -v "\[sync\]" | head -30
done
