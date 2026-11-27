#!/usr/bin/env python3
# hold60_extract.py (hold60, 23 Nov 2026): console load traces for native/tools/fps/hold60_unit.cpp, from Smooth 60 tier logs (the [tier], [late], [gsplit-pic], [lane] lines).
#   python3 -I native/tools/fps/hold60_extract.py LOG... > native/tools/fps/hold60_traces.txt
# One "seg" row per 300-live-picture [late] window of live play after the first cut into play: seconds, the game thread's 1-step picture work p50 / p95 and the simulation's ms a step
# (the [gsplit-pic] line after it: work net of waits), the front-end, render-thread and GPU ms a picture ([late]: the lanes' largest of three, the GPU at the size the window was drawn at,
# mostly the full-size 30), and the share of the window at 30 (the GPU size). One "entry" row per cut into play at 60 ([fps] auto: 30 -> 60, task state 0x2): the pictures and seconds
# the console then made at 60 before the fall ([tier] 60 -> 45), i.e. the cut-in transient. What the windows do not say (the per-picture shape) the test models; every replay is an ESTIMATE.
# hold60b (24 Nov): two more columns, the game thread's blocked wait on the helper lane a picture (the async snapshot blend's join: the [gsplit] line's "other" wait, which [gsplit-pic]
# leaves out of its work) and the share of the window at 45. The lane's wait depends on the cadence (console s49 Vice: 0.6-0.9 ms at the paced 30, 3.0-5.6 at 45, 1.7 at 60 in the Mega Strike).
import re
import sys

R = 1001.0 / 60000.0


def num(pat, s, d=None):
    m = re.search(pat, s)
    return float(m.group(1)) if m else d


def extract(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    parts = path.rstrip("/").split("/")
    name = parts[-1].replace(".log", "")
    if name == "log.txt" and len(parts) >= 3:   # (the relay outbox: <run>/00/log.txt)
        name = parts[-3]
    stadium = None
    out = []
    inplay = False
    last_gs = None
    entries = []
    pend = None   # an entry waiting for its fall
    rate = 30.0
    at30 = 1.0
    at45 = 0.0
    lane = 0.0
    tot = None   # the console's own time at 60 / 45 / 30 over the run (the last [tier] line's "all so far")
    for ln in lines:
        if stadium is None:
            m = re.search(r"stadium (\d+)\)", ln)
            if m:
                stadium = int(m.group(1))
        if ln.startswith("[fps] auto: 30 -> 60") and "task state 0x2 " in ln:
            t = num(r"at t=([0-9.]+)", ln)
            fr = num(r"frame (\d+)", ln)
            pend = (t, fr)
            inplay = True
        elif ln.startswith("[fps] auto: 60 -> 30") and "task state 0x2 " in ln and pend:
            t = num(r"at t=([0-9.]+)", ln)
            fr = num(r"frame (\d+)", ln)
            entries.append((fr - pend[1], t - pend[0]))
            pend = None
        elif ln.startswith("[lane] ") and "pics" in ln:
            r = num(r"([0-9.]+)/s game", ln)
            if r:
                rate = r
        elif ln.startswith("[tier] 300 live pictures"):
            m = re.search(r"time at 60 / 45 / 30 ([0-9.]+) / ([0-9.]+) / ([0-9.]+) s", ln)
            t = re.search(r"all so far ([0-9.]+) / ([0-9.]+) / ([0-9.]+) s", ln)
            if t:
                tot = tuple(float(x) for x in t.groups())
            if m:
                a, b, c = (float(x) for x in m.groups())
                at30 = c / (a + b + c) if a + b + c > 0 else 1.0
                at45 = b / (a + b + c) if a + b + c > 0 else 0.0
        elif ln.startswith("[gsplit] 300 pics"):
            lane = num(r"wait \(blocked, not CPU\) [0-9.]+ \(.* other ([0-9.]+)\)", ln, 0.0)
        elif ln.startswith("[gsplit-pic]"):
            m = re.search(r"\| 1 steps: (\d+) pictures, p50 ([0-9.]+) p95 ([0-9.]+) max [0-9.]+ \(net[^)]*\); mean sim ([0-9.]+)", ln)
            if m:
                last_gs = (float(m.group(2)), float(m.group(3)), float(m.group(4)), lane)
                if out and out[-1][1] is None:
                    out[-1][1] = last_gs
        elif ln.startswith("[late] 300 live pictures") and inplay:
            m = re.search(r"ms a picture: game ([0-9.]+) front end ([0-9.]+) render thread ([0-9.]+) GPU ([0-9.]+)", ln)
            if m:
                game, fe, be, gpu = (float(x) for x in m.groups())
                secs = 300.0 / max(rate, 20.0)
                out.append([(secs, game, fe, be, gpu, at30, at45), None])
    rows = []
    for (secs, game, fe, be, gpu, a30, a45), gs in out:
        if gs is None:
            gs = last_gs
        if gs is None:
            continue
        rows.append("seg %s %d %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f" % (name, stadium or 0, secs, gs[0], gs[1], gs[2], fe, be, gpu, a30, gs[3], a45))
    for n, s in entries:
        rows.append("entry %s %d %d %.3f" % (name, stadium or 0, int(n), s))
    if tot:
        rows.append("total %s %d %.0f %.0f %.0f" % ((name, stadium or 0) + tot))
    return rows


def main():
    print("# hold60 console traces (native/tools/fps/hold60_extract.py; real Vita, 500 MHz, 4th core: logs w8/console-1123 (23 Nov, builds s44-s46) and the relay outbox 5141-5151 (s49, hold60 on))")
    print("# seg <log> <stadium> <seconds> <1-step work p50 ms> <p95 ms> <sim ms a step> <front end ms> <render thread ms> <GPU ms at the window's size> <share of the window at the full-size 30>"
          " <blend lane wait ms a picture> <share of the window at 45>")
    print("# entry <log> <stadium> <pictures at 60 after the cut into play before the fall> <seconds>")
    print("# total <log> <stadium> <the console's seconds of live play at 60> <at 45> <at 30> (the run's last [tier] line)")
    for p in sys.argv[1:]:
        for r in extract(p):
            print(r)


if __name__ == "__main__":
    main()
