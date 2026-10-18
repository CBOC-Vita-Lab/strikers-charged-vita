#!/bin/bash
# usage: tools/census.sh <target> [N]   top error messages over the logs of the last build
T=${1:-host32}; N=${2:-40}
L=/home/claude/msc/build/np/$T/log
find $L -name '*.err' -print0 | xargs -0 grep -h -E 'error:' 2>/dev/null \
 | sed -E 's/^[^ ]+ error: //; s/'"'"'[^'"'"']*'"'"'/Q/g' | sort | uniq -c | sort -rn | head -$N
