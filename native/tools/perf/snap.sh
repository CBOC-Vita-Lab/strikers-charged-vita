#!/bin/bash
# NPP2: a snapshot of the worktree (without .git and the plugin binaries) for a queued build job: build.sh builds whatever is in its source
# directory WHEN THE JOB STARTS, so a job that waits in the heavy queue must not look at the live tree.   snap.sh <name>  -> $SCR/<name>-src
# (refreshing a name whose job has not started is allowed; once the job runs, take a new name)
N=${1:?name}
SCR=/tmp/claude-0/-home-claude/2e068ce2-50b7-5168-bd6a-083a34fe1600/scratchpad/npp2
mkdir -p $SCR/$N-src.new && tar -C /home/claude/msc/npw/NPP2 --exclude=.git --exclude='*.so' -cf - . | tar -C $SCR/$N-src.new -xf - || exit 1
[ -d $SCR/$N-src ] && mv $SCR/$N-src $SCR/$N-src.old
mv $SCR/$N-src.new $SCR/$N-src
[ -d $SCR/$N-src.old ] && rm -rf "${SCR:?}/$N-src.old"
echo "snapshot $SCR/$N-src ($(git -C /home/claude/msc/npw/NPP2 log --oneline | head -1))"
