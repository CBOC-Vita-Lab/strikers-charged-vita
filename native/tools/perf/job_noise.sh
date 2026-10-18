#!/bin/bash
# NPP2 job (one heavy slot): is the renderer's per-frame hash sensitive to the process layout / the garbage in memory, in a build that is otherwise unchanged?
# Two more runs of one build (nofork) against its first run (REF): the environment longer by one variable (stack addresses move), and every game-heap
# allocation filled with 0xA5 (NP_HEAP_POISON). Equal simulation digests and different frame hashes in the walk-out say the render path reads
# something that depends on the layout; the same set of frames as in LTO versus non-LTO says LTO is one more layout change.
#   tools/heavy.sh -t 600 env V=a3 REF=/home/claude/msc/build/np/NPP2/runs/a3-nofork tools/perf/job_noise.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a3}
B=/home/claude/msc/build/np/NPP2
echo "=== $V padded"; V=$V TAG=padded FS=nofork XVARS="NPPPAD=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" REF=$REF tools/perf/job_one.sh
echo "=== $V heapA5"; V=$V TAG=heapA5 FS=nofork XVARS="NP_HEAP_POISON=0xA5" REF=$REF tools/perf/job_one.sh
python3 - <<PY
import csv
def load(p):
    return [l.strip().split(',') for l in open(p)]
ref=load("$REF/vgx_frames.csv")
for t in ('padded','heapA5'):
    o=load("$B/runs/$V-%s/vgx_frames.csv" % t)
    d=[int(a[0]) for a,b in zip(ref,o) if a[-1]!=b[-1] and a[0].isdigit()]
    print(t, 'differing frame hashes:', len(d), d[:12], '...', d[-4:])
PY
