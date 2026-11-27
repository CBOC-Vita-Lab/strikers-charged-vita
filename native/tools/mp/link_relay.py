#!/usr/bin/env python3
"""A bad-network relay for the host pair rig (native_pair.py --link PROFILE).

Two (or more) game processes talk UDP on 127.0.0.1 (platform/sdk/net/vlink_udp.cpp: MSC_LINK_PORT, MSC_LINK_BCAST). The relay sits between
them: console i is told that console j lives at the relay's stand-in port RBASE+j, so everything console i sends to console j arrives at the
relay's socket for j, is impaired, and leaves from the socket that stands for i (console j sees the source RBASE+i and replies to it, which
arrives at the relay again and goes back the same way). The game binary needs no change.

Impairments per direction: one-way latency and jitter, uniform loss, burst loss (two-state), reordering, duplication, a bandwidth cap with a
queue limit, and outage windows (everything dropped). Profiles are plausible shapes, not measurements of a real network. Shape and idea follow
the Galaxy port's relay (profiles lan, home_wifi, bad_wifi, hotspot, terrible, outage10); written here from scratch for this rig.

The relay counts what it did (link-stats.json): a run that asked for a profile but whose relay forwarded nothing is INVALID, not a pass.
"""
import argparse
import heapq
import json
import random
import re
import select
import signal
import socket
import sys
import time

PROFILES = {
    "lan": dict(latency_ms=1, jitter_ms=0),
    "home_wifi": dict(latency_ms=4, jitter_ms=3, loss_pct=0.3),
    "bad_wifi": dict(latency_ms=25, jitter_ms=20, loss_pct=3, burst_enter_pct=0.5, burst_len=4, reorder_pct=1, reorder_ms=30, dup_pct=0.2),
    "hotspot": dict(latency_ms=60, jitter_ms=40, loss_pct=2, burst_enter_pct=0.5, burst_len=8, bw_kbps=1500, queue_ms=300, reorder_pct=0.5, reorder_ms=30),
    "terrible": dict(latency_ms=120, jitter_ms=80, loss_pct=10, burst_enter_pct=1, burst_len=10, bw_kbps=600, queue_ms=400, reorder_pct=3, reorder_ms=60, dup_pct=1),
    # the outage profiles are home_wifi plus a cut that starts when the trigger line shows in a console's log (or `outage_at` seconds after the start)
    "outage5": dict(latency_ms=4, jitter_ms=3, loss_pct=0.3, outage_len=5),
    "outage10": dict(latency_ms=4, jitter_ms=3, loss_pct=0.3, outage_len=10),
    "outage15": dict(latency_ms=4, jitter_ms=3, loss_pct=0.3, outage_len=15),
    "outage30": dict(latency_ms=4, jitter_ms=3, loss_pct=0.3, outage_len=30),
}
DEFAULTS = dict(latency_ms=1.0, jitter_ms=0.0, loss_pct=0.0, burst_enter_pct=0.0, burst_len=1.0, reorder_pct=0.0, reorder_ms=0.0, dup_pct=0.0,
                bw_kbps=0.0, queue_ms=250.0, outage_len=0.0, outage_at=-1.0)


def parse_spec(spec):
    """'bad_wifi' or 'bad_wifi,loss_pct=8,latency_ms=60' -> parameter dict (a profile name first, then overrides)."""
    parts = [p for p in spec.split(",") if p]
    params = dict(DEFAULTS)
    for p in parts:
        if "=" in p:
            k, v = p.split("=", 1)
            if k not in DEFAULTS:
                raise ValueError("unknown link key %r" % k)
            params[k] = float(v)
        else:
            if p not in PROFILES:
                raise ValueError("unknown link profile %r (have %s)" % (p, ", ".join(sorted(PROFILES))))
            params.update(PROFILES[p])
    return params


class Direction:
    """One direction's impairment state and counters."""

    def __init__(self, params, rng):
        self.p = params
        self.rng = rng
        self.burst_left = 0
        self.busy_until = 0.0     # bandwidth cap: when the link is free again
        self.stats = dict(in_=0, out=0, bytes=0, drop_loss=0, drop_burst=0, drop_outage=0, drop_queue=0, dup=0, reordered=0)
        self.delays = []

    def schedule(self, now, size, outage):
        """List of (due time) for the copies of one datagram, [] when it is dropped."""
        p, r, s = self.p, self.rng, self.stats
        s["in_"] += 1
        if outage:
            s["drop_outage"] += 1
            return []
        if self.burst_left > 0:
            self.burst_left -= 1
            s["drop_burst"] += 1
            return []
        if p["burst_enter_pct"] > 0 and r.random() * 100 < p["burst_enter_pct"]:
            self.burst_left = max(0, int(p["burst_len"]) - 1)
            s["drop_burst"] += 1
            return []
        if p["loss_pct"] > 0 and r.random() * 100 < p["loss_pct"]:
            s["drop_loss"] += 1
            return []
        start = now
        if p["bw_kbps"] > 0:
            if self.busy_until - now > p["queue_ms"] / 1000.0:
                s["drop_queue"] += 1
                return []
            start = max(now, self.busy_until)
            self.busy_until = start + size * 8.0 / (p["bw_kbps"] * 1000.0)
        delay = p["latency_ms"] / 1000.0 + (r.random() * 2 - 1) * p["jitter_ms"] / 1000.0
        delay = max(0.0, delay)
        if p["reorder_pct"] > 0 and r.random() * 100 < p["reorder_pct"]:
            delay += p["reorder_ms"] / 1000.0
            s["reordered"] += 1
        due = [start + delay]
        if p["dup_pct"] > 0 and r.random() * 100 < p["dup_pct"]:
            due.append(start + delay + 0.002)
            s["dup"] += 1
        s["out"] += len(due)
        s["bytes"] += size
        for d in due:
            self.delays.append(d - now)
        return due


class Relay:
    def __init__(self, n, real_base, relay_base, params, seed=1, log=None, trigger=None):
        self.n = n
        self.real_base = real_base
        self.relay_base = relay_base
        self.p = params
        self.rng = random.Random(seed)
        self.dirs = {(a, b): Direction(params, self.rng) for a in range(n) for b in range(n) if a != b}
        self.socks = []
        for i in range(n):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", relay_base + i))
            s.setblocking(False)
            self.socks.append(s)
        self.heap = []
        self.seq = 0
        self.t0 = time.time()
        self.log = log
        self.trigger = trigger            # (path list, compiled regex) or None
        self.trigger_pos = {}
        self.outage_from = None
        self.outage_to = None
        if params["outage_len"] > 0 and params["outage_at"] >= 0:
            self.outage_from = self.t0 + params["outage_at"]
            self.outage_to = self.outage_from + params["outage_len"]
        self.events = []

    def note(self, text):
        line = "t=%.2f %s" % (time.time() - self.t0, text)
        self.events.append(line)
        if self.log:
            self.log.write(line + "\n")
            self.log.flush()

    def in_outage(self, now):
        return self.outage_from is not None and self.outage_from <= now < self.outage_to

    def check_trigger(self, now):
        if self.outage_from is not None or not self.trigger or self.p["outage_len"] <= 0:
            return
        paths, rx, delay = self.trigger
        for path in paths:
            try:
                with open(path, "r", errors="replace") as f:
                    f.seek(self.trigger_pos.get(path, 0))
                    chunk = f.read()
                    self.trigger_pos[path] = f.tell()
            except OSError:
                continue
            if rx.search(chunk):
                self.outage_from = now + delay
                self.outage_to = self.outage_from + self.p["outage_len"]
                self.note("outage armed by %s: cut from +%.1f s for %.1f s" % (path, delay, self.p["outage_len"]))
                return

    def run(self, stop_at=None):
        last_state = False
        while stop_at is None or time.time() < stop_at:
            now = time.time()
            timeout = 0.02
            if self.heap:
                timeout = max(0.0, min(timeout, self.heap[0][0] - now))
            ready, _, _ = select.select(self.socks, [], [], timeout)
            now = time.time()
            for s in ready:
                try:
                    while True:
                        data, src = s.recvfrom(4096)
                        self.accept(self.socks.index(s), src, data, now)
                except BlockingIOError:
                    pass
            self.check_trigger(now)
            state = self.in_outage(now)
            if state != last_state:
                self.note("outage %s" % ("begins" if state else "ends"))
                last_state = state
            while self.heap and self.heap[0][0] <= now:
                _, _, via, dest, data = heapq.heappop(self.heap)
                try:
                    self.socks[via].sendto(data, ("127.0.0.1", self.real_base + dest))
                except OSError:
                    pass

    def accept(self, to_idx, src, data, now):
        frm = src[1] - self.real_base
        if not (0 <= frm < self.n) or frm == to_idx:
            return
        d = self.dirs[(frm, to_idx)]
        for due in d.schedule(now, len(data), self.in_outage(now)):
            self.seq += 1
            heapq.heappush(self.heap, (due, self.seq, frm, to_idx, data))

    def stats(self):
        out = {}
        for (a, b), d in self.dirs.items():
            dl = sorted(d.delays)
            q = lambda f: round(dl[min(len(dl) - 1, int(f * len(dl)))] * 1000, 1) if dl else 0
            out["%d>%d" % (a, b)] = dict(d.stats, delay_ms_p50=q(0.5), delay_ms_p95=q(0.95), delay_ms_max=q(1.0) if dl else 0)
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--spec", default="home_wifi", help="profile name and overrides, e.g. bad_wifi,loss_pct=8")
    ap.add_argument("--consoles", type=int, default=2)
    ap.add_argument("--real-base", type=int, default=41000, help="console i listens on real-base+i")
    ap.add_argument("--relay-base", type=int, default=41100, help="stand-in for console i is relay-base+i")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--stats", required=True)
    ap.add_argument("--log")
    ap.add_argument("--trigger-logs", nargs="*", default=[], help="console logs watched for the outage trigger")
    ap.add_argument("--trigger", default=r"\[mp-lan\] sync tick=", help="regex of the line that arms the outage")
    ap.add_argument("--trigger-delay", type=float, default=5.0)
    ap.add_argument("--duration", type=float, default=None)
    ap.add_argument("--list-profiles", action="store_true")
    a = ap.parse_args()
    if a.list_profiles:
        for k, v in sorted(PROFILES.items()):
            print(k, v)
        return 0
    params = parse_spec(a.spec)
    log = open(a.log, "w") if a.log else None
    trig = (a.trigger_logs, re.compile(a.trigger, re.M), a.trigger_delay) if a.trigger_logs else None
    relay = Relay(a.consoles, a.real_base, a.relay_base, params, a.seed, log, trig)
    # a child of a background job starts with SIGINT ignored (the shell does that), so no KeyboardInterrupt would ever arrive: take both signals ourselves
    def stop(_sig, _frm):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    relay.note("relay up: %s" % a.spec)
    try:
        relay.run(time.time() + a.duration if a.duration else None)
    except KeyboardInterrupt:
        pass
    finally:
        json.dump(dict(spec=a.spec, params=params, events=relay.events, directions=relay.stats()), open(a.stats, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
