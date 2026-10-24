#!/usr/bin/env python3
"""Read the in-memory performance recorder's bank files and print a short report.

On the Vita, set `MSC_TOUCH_DIAG=1` (and optionally `MSC_PERF_DETAIL=summary`) in
`ux0:data/mscharged/np-player.txt`, play, then pause or go back to a menu so the banks are written.
Copy `ux0:data/mscharged/perf-*.bin` to the computer and run:

    python3 tools/diag/perf-report.py perf-*.bin            # text report per bank
    python3 tools/diag/perf-report.py --json perf-*.bin     # the same figures as JSON

Bank mode 1 is ordinary play, mode 2 a Mega Strike. Each bank is read on its own; frames are never
joined across files. Times are CPU wall clock on the console, not GPU timestamps. GPU-call rows need a
build made with NP_DIAG_GPU=1.
"""
import argparse
import json
import struct
import sys
import zlib

HEADER = 128
EVENT = struct.Struct("<7Q2I")  # ns, id, a, b, c, d, e, kind, flags
PREROLL = 0x80000000
KINDS = """START POLL CONTACT PANEL SAMPLE GESTURE HANDOVER DECODE BACKEND BACKEND_END SWAP CALLBACK GATE FIFO_ERROR
TASK_DT SIM_STEP ATTEMPT POINTER_BEFORE SAVE HANDOFF SEND KEEPER SHADER CTRL MEMORY QUIET CONTEXT SIM_END SWAP_END TASK
COMMAND CACHE GEOMETRY THREAD THREAD_SAMPLE_COST FRAME_SEND FRAME_JOIN POINTER_AFTER CLOCK_COST GPU MEGA_PHASE
LIBRARY_DISPLAY""".split()
KIND = {name: i + 1 for i, name in enumerate(KINDS)}
GPU_CALLS = {1: "sceGxmFinish", 2: "sceGxmNotificationWait", 3: "sceGxmDisplayQueueAddEntry",
             4: "sceGxmBeginScene", 5: "sceGxmEndScene"}
STATES = {2: "bank full", 3: "time limit", 4: "safe point or scene change", 5: "saved", 6: "write error", 8: "tail deadline"}


def stats(values):
    """count, median, p95, max of a list of milliseconds (None when empty)."""
    if not values:
        return None
    v = sorted(values)
    pick = lambda q: v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]
    return {"count": len(v), "median": round(pick(0.5), 3), "p95": round(pick(0.95), 3), "max": round(v[-1], 3)}


def read_bank(path):
    data = open(path, "rb").read()
    if len(data) < HEADER + 4 or data[:7] != b"MSCTD02":
        raise ValueError(f"{path}: not a recorder bank (MSCTD02)")
    version, size, count, capacity, nxt, overflow = struct.unpack_from("<6I", data, 8)
    state, = struct.unpack_from("<I", data, 36)
    start, stop = struct.unpack_from("<2Q", data, 40)
    clocks_start = struct.unpack_from("<4I", data, 56)
    mode, number, raced, clock_cost = struct.unpack_from("<4I", data, 92)
    title = data[112:121].split(b"\0")[0].decode("ascii", "replace")
    if size != EVENT.size:
        raise ValueError(f"{path}: event size {size}, this tool reads {EVENT.size}")
    end = HEADER + count * size
    if len(data) < end + 4:
        raise ValueError(f"{path}: truncated ({len(data)} bytes, needs {end + 4})")
    crc_ok = (zlib.crc32(data[:end]) & 0xffffffff) == struct.unpack_from("<I", data, end)[0]
    events = [EVENT.unpack_from(data, HEADER + i * size) for i in range(count)]
    return {"path": path, "version": version, "title": title, "mode": mode, "number": number, "state": state,
            "start": start, "stop": stop, "count": count, "capacity": capacity, "overflow": overflow,
            "raced": raced, "clockCostNs": clock_cost, "clocksMHz": clocks_start, "crcOk": crc_ok, "events": events}


def analyse(bank):
    ev = [e for e in bank["events"] if not e[8] & PREROLL]
    by = {}
    for e in ev:
        by.setdefault(e[7], []).append(e)
    ms = lambda ns: ns / 1e6
    out = {"kinds": {KINDS[k - 1] if 0 < k <= len(KINDS) else str(k): len(v) for k, v in sorted(by.items())}}
    # display cadence: gate records, in order
    gates = [e[0] for e in by.get(KIND["GATE"], [])]
    intervals = [ms(b - a) for a, b in zip(gates, gates[1:]) if b > a]
    out["displayIntervalMs"] = stats(intervals)
    if intervals:
        out["displayHz"] = round(1000.0 / (sum(intervals) / len(intervals)), 2)
    # frame handover (game thread) to display gate, matched by frame tag
    handed = {e[1]: e[0] for e in by.get(KIND["HANDOVER"], [])}
    out["handoverToGateMs"] = stats([ms(e[0] - handed[e[1]]) for e in by.get(KIND["GATE"], [])
                                     if e[1] in handed and e[0] >= handed[e[1]]])
    # renderer back end per frame (begin record to end record, same tag)
    begun = {e[1]: e[0] for e in by.get(KIND["BACKEND"], [])}
    out["backendMs"] = stats([ms(e[0] - begun[e[1]]) for e in by.get(KIND["BACKEND_END"], [])
                              if e[1] in begun and e[0] >= begun[e[1]]])
    draws = [e[3] for e in by.get(KIND["BACKEND"], [])]
    if draws:
        out["drawsPerFrame"] = stats([float(d) for d in draws])
    # GPU calls (NP_DIAG_GPU=1 builds): a = begin, b = end
    gpu = {}
    for e in by.get(KIND["GPU"], []):
        if e[3] >= e[2]:
            gpu.setdefault(GPU_CALLS.get(e[8] & 0xffff, f"call {e[8] & 0xffff}"), []).append(ms(e[3] - e[2]))
    out["gpuCallMs"] = {k: stats(v) for k, v in sorted(gpu.items())}
    # game tasks: flags = task priority, a = begin, b = end
    tasks = {}
    for e in by.get(KIND["TASK"], []):
        if e[3] >= e[2]:
            tasks.setdefault(e[8], []).append(ms(e[3] - e[2]))
    out["taskMs"] = {str(k): stats(v) for k, v in sorted(tasks.items())}
    for name in ("FRAME_SEND", "FRAME_JOIN"):
        out[name.lower() + "Ms"] = stats([ms(e[3] - e[2]) for e in by.get(KIND[name], []) if e[3] >= e[2]])
    out["simSteps"] = len(by.get(KIND["SIM_STEP"], []))
    out["fifoErrors"] = len(by.get(KIND["FIFO_ERROR"], []))
    out["shaderEvents"] = len(by.get(KIND["SHADER"], []))
    return out


def fmt(s):
    return "-" if not s else f"median {s['median']:.2f}  p95 {s['p95']:.2f}  max {s['max']:.2f}  (n={s['count']})"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("banks", nargs="+")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    reports, failed = [], False
    for path in args.banks:
        try:
            bank = read_bank(path)
        except (OSError, ValueError) as e:
            print(e, file=sys.stderr)
            failed = True
            continue
        r = {k: v for k, v in bank.items() if k != "events"}
        r.update(analyse(bank))
        reports.append(r)
    if args.json:
        print(json.dumps(reports, indent=1))
        return 1 if failed else 0
    for r in reports:
        seconds = (r["stop"] - r["start"]) / 1e9 if r["stop"] > r["start"] else 0
        print(f"== {r['path']}  ({r['title']}, bank {r['number']}, {'Mega Strike' if r['mode'] == 2 else 'ordinary play'}, "
              f"{seconds:.1f} s, ended: {STATES.get(r['state'], r['state'])})")
        warn = []
        if not r["crcOk"]:
            warn.append("CRC MISMATCH: the file is damaged; do not trust the figures")
        if r["overflow"]:
            warn.append(f"{r['overflow']} events dropped (bank full)")
        if r["raced"]:
            warn.append(f"{r['raced']} events raced the bank's close")
        for w in warn:
            print("   !", w)
        print(f"   clocks at start (MHz): CPU {r['clocksMHz'][0]}, GPU {r['clocksMHz'][1]}, bus {r['clocksMHz'][2]}, xbar {r['clocksMHz'][3]}")
        if r.get("displayHz"):
            print(f"   display: {r['displayHz']} Hz; interval {fmt(r['displayIntervalMs'])}")
        print(f"   handover -> display gate (ms): {fmt(r['handoverToGateMs'])}")
        print(f"   renderer back end (ms):        {fmt(r['backendMs'])}")
        if r.get("drawsPerFrame"):
            print(f"   draws per frame:               {fmt(r['drawsPerFrame'])}")
        for name, s in r["gpuCallMs"].items():
            print(f"   {name + ' (ms):':34s}{fmt(s)}")
        for name in ("frame_sendMs", "frame_joinMs"):
            if r[name]:
                print(f"   {name[:-2].replace('_', ' ') + ' (ms):':31s}{fmt(r[name])}")
        for prio, s in r["taskMs"].items():
            print(f"   task {prio} (ms):{'':{22 - len(prio)}}{fmt(s)}")
        print(f"   sim steps {r['simSteps']}, shader events {r['shaderEvents']}, FIFO errors {r['fifoErrors']}")
        print("   events: " + ", ".join(f"{k} {v}" for k, v in r["kinds"].items()))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
