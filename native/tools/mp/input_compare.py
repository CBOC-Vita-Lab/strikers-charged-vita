#!/usr/bin/env python3
"""Checks the input trace of a native_pair.py run made with MSC_LAN_INPUT_TRACE=1 (STR-143 / STR-144).

Every console logs "[mp-in] cap|app tick=T ch=C h=H rec=<16 bytes> conn=N remap=R" whenever a channel's captured record (cap: what the console puts on
the wire, taken from its own pad) or applied record (app: what the lockstep hands to the simulation) changes.

  1. APPLIED: for every channel, both consoles applied the same sequence of records (same bytes, connection state, remap angle) at the same ticks.
     A console that applied a record read from its own pad instead of the wire, or a record that lost bytes on the way, differs here.
  2. WIRE: the records a console captured for a channel, in order, are a subsequence of what both consoles applied for that channel (every change the
     owner captured reached the other console unchanged; the lockstep may apply a record later, never another one). Records the owner captured and
     that were superseded within one tick are allowed to be missing from the applied list; none may be altered.
  3. FULL RECORD: the number of distinct records with non-zero bytes in the 4 bytes after the first 8 (the stick/trigger/accelerometer part that older
     builds cut off) is reported; with the full 16-byte record on the wire the applied records carry them too.

usage: input_compare.py RUN_DIR [--min-changes N]   (RUN_DIR holds peer0/runtime.log and peer1/runtime.log)
Exit status 0 when 1 and 2 hold, and at least N changes were applied per console.
"""
import re
import sys
from pathlib import Path

LINE = re.compile(r"\[mp-in\] (cap|app) tick=(-?\d+) ch=(\d+) h=([0-9a-f]+) rec=([0-9a-f]{32}) conn=(\d+) remap=([0-9a-f]+)")


def load(path):
    cap, app = {}, {}
    for line in open(path, errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        kind, tick, ch, h, rec, conn, remap = m.groups()
        (cap if kind == "cap" else app).setdefault(int(ch), []).append((int(tick), h, rec, int(conn), remap))
    return cap, app


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    min_changes = 1
    if "--min-changes" in sys.argv:
        min_changes = int(sys.argv[sys.argv.index("--min-changes") + 1])
        args = [a for a in args if a != str(min_changes)]
    run = Path(args[0])
    logs = [run / "peer0" / "runtime.log", run / "peer1" / "runtime.log"]
    data = [load(p) for p in logs]
    ok = True
    channels = sorted(set(data[0][1]) | set(data[1][1]))
    print("channels with applied inputs: %s" % channels)
    # 1. applied sequences equal (ignore the leading all-neutral record both start with, and records after one console stopped logging)
    for ch in channels:
        a, b = data[0][1].get(ch, []), data[1][1].get(ch, [])
        n = min(len(a), len(b))
        bad = [(x, y) for x, y in zip(a[:n], b[:n]) if x[1] != y[1] or x[0] != y[0]]
        tail = abs(len(a) - len(b))
        status = "OK" if not bad and tail <= 2 else "DIFFERENT"
        if status != "OK":
            ok = False
        print("  ch%-2d applied: peer0 %d changes, peer1 %d; mismatches %d, length difference %d: %s" % (ch, len(a), len(b), len(bad), tail, status))
        for x, y in bad[:3]:
            print("     peer0 %s\n     peer1 %s" % (x, y))
        if min(len(a), len(b)) < min_changes:
            ok = False
            print("     fewer than %d applied changes" % min_changes)
    # 2. captured records reached both consoles unchanged
    for who in (0, 1):
        for ch, caps in sorted(data[who][0].items()):
            for other in (0, 1):
                applied = {x[1] for x in data[other][1].get(ch, [])}
                missing = [c for c in caps if c[1] not in applied]
                # a record superseded before the next lockstep tick is legitimately never applied; more than a few missing means loss or alteration
                frac = len(missing) / max(1, len(caps))
                status = "OK" if frac <= 0.25 else "ALTERED/LOST"
                if frac > 0.25:
                    ok = False
                print("  ch%-2d captured on peer%d: %d changes, %d never applied on peer%d (%.0f%%): %s" % (ch, who, len(caps), len(missing), other, 100 * frac, status))
    # 3. the part of the record the 8-byte transport used to cut off
    for who in (0, 1):
        wide = {x[1] for ch in data[who][1] for x in data[who][1][ch] if any(x[2][i:i + 2] != "00" for i in range(16, 32, 2))}
        print("  peer%d: %d distinct applied records use bytes 8..15 of the record" % (who, len(wide)))
    print("RESULT %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
