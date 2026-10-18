#!/bin/bash
# W5-SEED: the final lists from the finished null runs (rec/*.shl), computed at the start of the Vita3K turn so that it includes every run that has finished by then.
#   final_lists.sh <out dir>
# out/final.shl = union of every run except the held-out sets (ho_ is folded in: it is known; hx_ never is), the old seed's list and ho_ = the catalogue
# out/core.shl  = the eager tier: front-end + base + every key used by at least 10 % of the match runs (tiers.py)
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
R=/home/claude/msc/build/np/W5SEED
OLD=/home/claude/msc/build/np/W5INT/seed/shadercache/vgx_shaderlist.bin
OUT=${1:?out dir}
mkdir -p $OUT
cd $HERE
python3 -I tools/seed/report.py --rec $R/rec --base $OLD --exclude '^(ho_|hx_)' --out $OUT/union.shl > $OUT/report.txt
python3 -I tools/seed/shl.py merge -o $OUT/final.shl $OUT/union.shl $R/rec/ho_*.shl > /dev/null
python3 -I tools/seed/tiers.py --rec $R/rec --base $OLD --share 0.10 --table --out-core $OUT/core.shl > $OUT/tiers.txt
python3 -I tools/seed/shl.py info $OUT/final.shl $OUT/core.shl | tee $OUT/lists.txt
