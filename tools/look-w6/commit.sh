#!/bin/bash
# copy the agent's text results into the worktree and commit them
W=/home/claude/s13/wt/look; D=$W/tools/look-w6
mkdir -p $D; cp -r /home/claude/s13/w6/look/tools/*.py /home/claude/s13/w6/look/tools/*.sh $D/ 2>/dev/null
cp /home/claude/s13/w6/look.md $D/look.md 2>/dev/null; cp /home/claude/s13/w6/look/index.md /home/claude/s13/w6/look/*.tsv $D/ 2>/dev/null
cd $W && git add -A tools/look-w6 && git -c user.name=CBOC-Vita -c user.email=cboc-vita@users.noreply.github.com commit -q -m "${1:-look: picture study notes and tools}" && git log --oneline | head -1
