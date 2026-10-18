#!/bin/bash
# W5-ONE: the native game reads a POPULATED save (cups, unlocks) imported from the recompiled build's folder, without corrupting it.
#   heavy.sh -t 400 tools/w5one_savetest.sh [exe]       default exe: build/np/W5ONE/host32/np-host32-full-null
# 1. a populated Wii-format Strikers2 is made from the recompiled build's real (fresh) save of this machine (test_save_import populate: unlocks 10 Striker
#    challenges, sets the cup unlock flags, volume 7, skill 3; header and CRC valid) and put where the recompiled build keeps it (a private "ux0:data/mscharged")
# 2. NpSaveImportRecompiled (the code the player build runs at its first start) copies it into an empty native data directory
# 3. the game plays the whole match script (menu, captains, stadium, kick-off: it writes Strikers2 on the way, SaveLoad::StartSave) with that directory
# 4. the file the game wrote is compared with the one it read: sound header and CRC, and every byte of the populated parts equal
B=${BUILD_ROOT:-/home/claude/msc/build/np/W5ONE}
EXE=${1:-$B/host32/np-host32-full-null}
NP=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
REAL=/home/claude/msc/appdata/MSCRecomp/NAND/title/00010000/52345145/data
T=$B/set/test_save_import
W=$B/savetest
rm -rf $W; mkdir -p $W/recomp/NAND/title/00010000/52345145/data $W/native $W/set
[ -x $T ] || g++ -std=gnu++17 -O1 -w $NP/platform/sdk/tests/test_save_import.cpp -o $T || exit 1
$T populate $REAL/Strikers2 $W/recomp/NAND/title/00010000/52345145/data/Strikers2 || exit 1
cp $REAL/banner.bin $W/recomp/NAND/title/00010000/52345145/data/banner.bin
sha_before=$(sha256sum $W/recomp/NAND/title/00010000/52345145/data/Strikers2 | cut -c1-16)
$T import $W/recomp $W/native | tee $W/import.log
cmp $W/recomp/NAND/title/00010000/52345145/data/Strikers2 $W/native/NAND/title/00010000/52345145/data/Strikers2 && echo "import: identical copy"
cp -a $W/native/NAND/title/00010000/52345145/data/Strikers2 $W/in.Strikers2
run() {   # run <n> <script> <frames>
  ( cd $W/native; export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$W/native MSC_INPUT_SCRIPT=$2 MSC_MAX_FRAMES=$3 MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000
    timeout -k 3 300 $EXE > $W/run$1.log 2>&1; echo "rc=$?" >> $W/run$1.log )
  echo "run $1: $(tail -1 $W/run$1.log), $(grep -ac 'accepting' $W/run$1.log) 'create save data' accepts, first-run lines: $(grep -ac 'first run' $W/run$1.log); $(grep -a 'STATE' $W/run$1.log | tail -2 | tr '\n' ';' | cut -c1-200)"
  ls -l --time-style=full-iso $W/native/NAND/title/00010000/52345145/data/ | tail -2
}
run 1 /home/claude/msc/tools/scripts/nav_match60.txt 4600
cp -a $W/native/NAND/title/00010000/52345145/data/Strikers2 $W/after1.Strikers2
run 2 /home/claude/msc/tools/scripts/nav_match60.txt 4600
cp -a $W/native/NAND/title/00010000/52345145/data/Strikers2 $W/after2.Strikers2
echo "source file untouched: $( [ "$sha_before" = "$(sha256sum $W/recomp/NAND/title/00010000/52345145/data/Strikers2 | cut -c1-16)" ] && echo yes || echo NO)"
python3 -I - "$W" <<'PY'
import sys, struct, zlib
W = sys.argv[1]
end = 8 + 35578
unlock, progress = end - 52, end - 52 - 928
def sound(b):
    return len(b) == 35616 and struct.unpack('>I', b[:4])[0] == 35586 and struct.unpack('>I', b[4:8])[0] == zlib.crc32(b[8:]) & 0xffffffff
inp = open(W + '/in.Strikers2', 'rb').read()
for name in ('after1', 'after2'):
    out = open(W + '/' + name + '.Strikers2', 'rb').read()
    diff = [i for i in range(min(len(inp), len(out))) if inp[i] != out[i]]
    print(f'{name}: {len(out)} bytes, header+CRC sound: {sound(out)}, bytes differing from the imported file: {len(diff)}')
    if diff:
        print('   first differing offsets (file offset, in/out):', [(hex(i), inp[i], out[i]) for i in diff[:12]])
    print(f'   cup unlock flags (+44 of the progress record) in {inp[progress+44:progress+46].hex()} out {out[progress+44:progress+46].hex()}: {"EQUAL" if inp[progress+44:progress+46] == out[progress+44:progress+46] else "CHANGED"}')
    print(f'   challenge unlock record (52 bytes) {"EQUAL" if inp[unlock:end] == out[unlock:end] else "CHANGED"}: in {inp[unlock+48:end].hex()} out {out[unlock+48:end].hex()}')
    print(f'   whole cup/progress/unlock region [{progress}:{end}) {"EQUAL" if inp[progress:end] == out[progress:end] else "CHANGED"}; UserInfo volume0/skill in ({struct.unpack(">I", inp[12:16])[0]},{struct.unpack(">I", inp[44:48])[0]}) out ({struct.unpack(">I", out[12:16])[0]},{struct.unpack(">I", out[44:48])[0]})')
PY
