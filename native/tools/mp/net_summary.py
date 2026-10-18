#!/usr/bin/env python3
"""Input delay, starved steps, steps per second, round trip and packet figures of LAN pairs (wave 5 NET).

Reads the "[mp-lan] net" lines (np_lan.cpp: one per MSC_LAN_NET_LOG window, default 5 s, and "net match" totals when a
match ends or the process exits) of each console of each pair directory, and prints one row per console:

  python3 tools/mp/net_summary.py build/np/W5NET/pairs/base_A build/np/W5NET/pairs/base_B ...   [--md]

delay = the lockstep's input delay in ticks (NetworkInputRouter::mQueueLimit, mean over the steps; one tick = 20 ms of input
lag for both players), starved = steps that found no input (each one 20 ms of match clock lost and a frame without a step),
sps = simulation steps per second of active match time (50 = full speed), rtt = the game's own round-trip estimate (last value),
tx/rx = datagrams per second each way and their mean payload, send = the link backend's send time (mean / max, microseconds).
A pair's checksums are checked by native_pair.py / pair_summary.py, not here.
"""
import json
import re
import sys
from pathlib import Path

NET_RE = re.compile(r"^\[mp-lan\] (net(?: match)?): (.*)$", re.M)
KV = {
    "t": r"t=([\d.]+) s", "active": r"active=([\d.]+) s", "steps": r"steps=(\d+)", "starved": r"starved=(\d+)",
    "sps": r"sps=([\d.]+)", "delay": r"delay=([\d.]+)", "dmin": r"delay=[\d.]+ \[(\d+)-", "dmax": r"delay=[\d.]+ \[\d+-(\d+)\]",
    "mult": r"mult=([\d.]+)", "qlocal": r"qlocal=([\d.]+)", "qremote": r"qremote=([\d.]+)", "rtt": r"rtt=(-?\d+) ms",
    "tx": r"tx=([\d.]+)/s", "txb": r"tx=[\d.]+/s (\d+) B", "txmax": r"tx=[\d.]+/s \d+ B max (\d+)",
    "rx": r"rx=([\d.]+)/s", "rxb": r"rx=[\d.]+/s (\d+) B", "rxmax": r"rx=[\d.]+/s \d+ B max (\d+)",
    "send": r"send=([\d.]+) us", "sendmax": r"max (\d+) us", "sends": r"\((\d+) sends\)", "rxq": r"rxq=(\d+)", "drops": r"drops=(\d+)", "flush": r"flush=(\d+)",
}


def parse(line):
    out = {}
    for k, rx in KV.items():
        m = re.search(rx, line)
        if m:
            out[k] = float(m.group(1))
    return out


def console(log):
    text = Path(log).read_text(errors="replace")
    windows, totals = [], []
    for kind, rest in NET_RE.findall(text):
        (totals if kind == "net match" else windows).append(parse(rest))
    transport = re.findall(r"^\[mp-lan\] input transport: (.*)$", text, re.M)
    lost = len(re.findall(r"^\[mp-lan\] connection lost", text, re.M))
    if totals:
        # one per match (and one at exit): sum the counts, weight the means by the steps
        rows = totals
    else:
        rows = windows
    if not rows:
        return {"transport": transport[-1] if transport else "?", "lines": 0, "lost": lost}
    steps = sum(r.get("steps", 0) for r in rows)
    starved = sum(r.get("starved", 0) for r in rows)
    active = sum(r.get("active", 0) for r in rows)
    wall = sum(r.get("t", 0) for r in rows)
    ticks = steps + starved or 1

    def wmean(k, weight="steps"):
        num = sum(r.get(k, 0) * (r.get(weight, 0) + (r.get("starved", 0) if weight == "steps" else 0)) for r in rows)
        den = sum((r.get(weight, 0) + (r.get("starved", 0) if weight == "steps" else 0)) for r in rows)
        return num / den if den else 0.0

    def tmean(k):
        num = sum(r.get(k, 0) * r.get("t", 0) for r in rows)
        return num / wall if wall else 0.0

    return {
        "transport": transport[-1] if transport else "?", "lines": len(rows), "from": "match totals" if totals else "windows",
        "steps": int(steps), "starved": int(starved), "starved_pct": 100.0 * starved / ticks, "active_s": active, "wall_s": wall,
        "sps": steps / active if active else 0.0, "delay": wmean("delay"), "dmin": min(r.get("dmin", 99) for r in rows),
        "dmax": max(r.get("dmax", 0) for r in rows), "qlocal": wmean("qlocal"), "qremote": wmean("qremote"),
        "rtt_last": rows[-1].get("rtt", -1), "rtt_windows": [int(r.get("rtt", -1)) for r in windows][-12:],
        "tx": tmean("tx"), "txb": tmean("txb"), "txmax": max(r.get("txmax", 0) for r in rows), "rx": tmean("rx"), "rxb": tmean("rxb"),
        "send_us": wmean("send", "sends"), "send_max_us": max(r.get("sendmax", 0) for r in rows), "rxq": max(r.get("rxq", 0) for r in rows),
        "flush": int(sum(r.get("flush", 0) for r in rows)), "lost": lost,
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    md = "--md" in sys.argv
    rows = []
    for d in args:
        d = Path(d)
        res = {}
        try:
            res = json.loads((d / "result.json").read_text())
        except Exception:
            pass
        for i in (0, 1):
            log = d / ("peer%d" % i) / "runtime.log"
            if not log.exists():
                continue
            c = console(log)
            c["pair"], c["peer"], c["passed"] = d.name, i, res.get("passed")
            c["ticks_compared"] = res.get("sync_ticks_compared")
            rows.append(c)
    if md:
        print("| pair | console | passed (checksums compared) | input delay, ticks (min-max) | = ms | starved steps | sps | rtt ms | tx/s, B | rx/s | send us mean/max | rx queue max |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for c in rows:
        if not c.get("lines"):
            print("%s peer%d: no net lines (%s)" % (c["pair"], c["peer"], c["transport"]))
            continue
        if md:
            print("| %s | %s | %s (%s) | %.2f (%d-%d) | %.0f | %d (%.2f %%) | %.2f | %s | %.1f, %.0f | %.1f | %.0f / %d | %d |" % (
                c["pair"], "host" if c["peer"] == 0 else "joiner", c["passed"], c["ticks_compared"], c["delay"], c["dmin"], c["dmax"],
                c["delay"] * 20, c["starved"], c["starved_pct"], c["sps"], int(c["rtt_last"]), c["tx"], c["txb"], c["rx"],
                c["send_us"], c["send_max_us"], c["rxq"]))
        else:
            print("%-14s peer%d passed=%s cmp=%s delay=%.2f ticks (%d-%d) = %.0f ms, qlocal %.2f qremote %.2f | starved %d (%.2f%%) | "
                  "sps %.2f over %.0f s | rtt %d %s | tx %.1f/s %.0f B (max %d) rx %.1f/s %.0f B | send %.0f us max %d | rxq %d | flush %d | lost %d | %s" % (
                      c["pair"], c["peer"], c["passed"], c["ticks_compared"], c["delay"], c["dmin"], c["dmax"], c["delay"] * 20, c["qlocal"],
                      c["qremote"], c["starved"], c["starved_pct"], c["sps"], c["active_s"], int(c["rtt_last"]), c["rtt_windows"], c["tx"],
                      c["txb"], c["txmax"], c["rx"], c["rxb"], c["send_us"], c["send_max_us"], c["rxq"], c["flush"], c["lost"], c["transport"]))


if __name__ == "__main__":
    main()
