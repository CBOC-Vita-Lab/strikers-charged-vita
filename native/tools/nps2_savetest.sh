#!/bin/bash
# NPS2: end-to-end save test with the host32 -full-null exe of tools/nps2_smoke.sh (no pictures).
#   run 1: no NAND directory -> the game offers 'create save data' -> Strikers2 + banner.bin are written through the codec
#   run 2: same directory -> the game must LOAD that save (no pop-up)
#   then: the payload of run 1 against a fresh save written by the recompiled build (same defaults: only mSaveID may differ)
#   heavy.sh -t 600 tools/nps2_savetest.sh
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
EXE=$ROOT/host32/np-host32-full-null
D=$ROOT/savetest-data
rm -rf $D; mkdir -p $D
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$PWD/tools/nav/nps2_savetest.txt MSC_WATCH=1 MSC_WATCH_EVERY=0 MSC_FRAME_LOG=0 MSC_MAX_FRAMES=4000 MSC_VIRTUAL_TIME=${VT:-1}
for n in 1 2; do
  ( cd $D && timeout -k 5 300 $EXE > $ROOT/savetest-$n.log 2>&1; echo "rc=$?" >> $ROOT/savetest-$n.log )
  echo "== run $n: $(grep -c 'accepting' $ROOT/savetest-$n.log) 'create save data' accepts; $(grep -E 'STATE|script' $ROOT/savetest-$n.log | tr '\n' ';' | cut -c1-300); $(tail -1 $ROOT/savetest-$n.log)"
  ls -la $D/NAND/title/00010000/52345145/data/ 2>&1 | tail -4
done
python3 - <<'PY'
import struct, zlib, glob
root='/home/claude/msc/build/np/NPS2x/savetest-data/NAND/title/00010000/52345145/data/'
ref='/home/claude/msc/appdata/MSCRecomp/NAND/title/00010000/52345145/data/'
try:
    n=open(root+'Strikers2','rb').read(); r=open(ref+'Strikers2','rb').read()
except Exception as e:
    print('no save:', e); raise SystemExit(1)
print('native', len(n), struct.unpack('>II', n[:8]), 'recomp', len(r), struct.unpack('>II', r[:8]))
print('native CRC ok:', zlib.crc32(n[8:]) & 0xffffffff == struct.unpack('>I', n[4:8])[0])
diff=[i for i in range(min(len(n),len(r))) if n[i]!=r[i]]
print('bytes differing from the recompiled build\'s fresh save:', len(diff), 'first', diff[:24])
nb=open(root+'banner.bin','rb').read(); rb=open(ref+'banner.bin','rb').read()
print('banner native', len(nb), 'recomp', len(rb), 'differing bytes', sum(1 for a,b in zip(nb,rb) if a!=b))
PY
