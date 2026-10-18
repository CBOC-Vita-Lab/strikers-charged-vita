#!/bin/bash
# A short Vita3K check of the PLAYER package (MSCN00003, no lab, reads np-player.txt): one heavy turn.
#   platform/vita/tools/job_v3k_player.sh <tag> <seconds>
# np-player.txt (= env-v3k-player.txt) steers the run: boot to the title with the nav script, then ends; the same file proves env.txt is NOT read
# (an env.txt that would end the run at frame 50 is put next to it).
cd "$(dirname "$0")/../../.."
TAG=${1:-p1}; SECS=${2:-200}
R=/home/claude/msc/build/np/NPV3
export VITASDK=/opt/vitasdk PATH=/opt/vitasdk/bin:$PATH
cat > $R/poison-env.txt <<'EOT'
# if the player build read this file the run would end at frame 50
MSC_MAX_FRAMES=50
EOT
PROFILE_DIR=$R/v3k-player KEEPDATA=0 FILES="/home/claude/msc/npw/NPV3/tools/nav/boot_title.txt" ENVNAME=np-player.txt POISON=$R/poison-env.txt \
  ENVTXT=platform/vita/tools/v3k-env/env-v3k-player.txt platform/vita/tools/job_v3k.sh $TAG $SECS platform/vita/tools/v3k-env/env-v3k-player.txt "STATE title screen" $R/lto/vita/pkg-player/np/mscharged-np.vpk MSCN00003
