#!/usr/bin/env python3
"""Two native game processes (np-host32 / np-armhf) as two consoles on the LAN link: lobby, draft, match, series.

Each console is an independent process of the same executable with its own data dir (save file / NAND), its own nav
script (tools/mp/lan/) and its own virtual IP; they reach each other through real UDP on 127.0.0.1, the same datagrams
two Vitas would exchange over ad hoc (platform/sdk/net/vlink_udp.cpp, MSC_LINK_PORT / _BCAST / _VIP).

A run passes when
  * both processes exit 0 with their scripts finished (no "[script] TIMEOUT"),
  * neither logs DESYNC / sync error / queue overflow / malformed message (np_lan.cpp lines "[mp-lan] ..."),
  * the game's own lockstep checksum and random seed (logged by NpLanSyncSeen every MSC_LAN_SYNC_LOG ticks, default 100)
    agree: within each log between the two machines, and between the two logs at every tick both logged,
  * repeated sync keys never contradict, and any completed end digests have corresponding counts, full matching keys/values,
    and entries for every machine observed in that match (neither peer needs an end record when both exit mid-match),
  * (with --expect-lost N) the number of "connection lost" lines per console is as expected (scripted disconnects).
Only explicit --lobby-only omits the common match-CRC requirement. That mode instead requires --expect-lost, each peer's
--end-marker0 / --end-marker1 before its script exit, and clean peers; any CRCs that are present must still agree.
The checksum is the game's own CalculateChecksum (simulation time, game object, ball, both teams), computed on every
console and compared by the router every tick; the log lines show that the check ran and what it saw.

  python3 tools/mp/native_pair.py --binary build/np/NPM2/host32/np-host32-full-null --out build/np/NPM2/pairs/p1 \
      [--host-script tools/mp/lan/lan_host.txt --join-script tools/mp/lan/lan_join.txt] [--env MSC_LAN_TEST_SCORE=3000:0] ...
Run through tools/heavy.sh (two game processes at once). --analyse-only re-reads an existing run directory.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
GAME = Path(os.environ.get("NP_DISC_DIR", "/work/msc/game"))
LAN = Path(__file__).resolve().parent / "lan"
QEMU_ARM = os.environ.get("QEMU_ARM", "/work/research/qemu-full/build/qemu-arm")


def launch(binary, out, index, script, max_frames, extra_env, virtual, qemu, port_base, watch_every):
    peer = out / ("peer%d" % index)
    (peer / "data").mkdir(parents=True, exist_ok=True)
    # Same environment size on both consoles (the stack layout of the two processes should not differ because of it):
    # equal-length values everywhere an index is involved.
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MSC_", "NP_"))}
    env.update({
        "NP_DISC_DIR": str(GAME), "NP_DATA_DIR": str(peer / "data"), "HOME": str(peer),
        "MSC_INPUT_SCRIPT": str(script), "MSC_MAX_FRAMES": str(max_frames),
        "MSC_WATCH": "1", "MSC_WATCH_EVERY": str(watch_every), "MSC_FRAME_LOG": "0",
        "MSC_LINK_PORT": str(port_base + index), "MSC_LINK_BCAST": "127.0.0.1:%d,127.0.0.1:%d" % (port_base, port_base + 1),
        "MSC_LINK_VIP": "10.0.0.%d" % (index + 1), "MSC_LINK_NAME": "VITA-%d" % (index + 1),
    })
    if virtual:
        env["MSC_VIRTUAL_TIME"] = "1"
    env.update(extra_env)
    log = open(peer / "runtime.log", "w")
    cmd = [str(binary)]
    if qemu:
        cmd = ["nice", "-n", "10", QEMU_ARM, "-cpu", "cortex-a9", "-L", "/usr/arm-linux-gnueabihf", str(binary)]
    proc = subprocess.Popen(cmd, cwd=str(peer), env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, peer / "runtime.log"


def start_relay(args, out, relay_base):
    """The bad-network relay (link_relay.py) as a child process; stats and events go to out/link-stats.json, out/link-relay.log."""
    cmd = [sys.executable, str(Path(__file__).resolve().parent / "link_relay.py"), "--spec", args.link, "--real-base", str(args.port_base),
           "--relay-base", str(relay_base), "--stats", str(out / "link-stats.json"), "--log", str(out / "link-relay.log"),
           "--trigger-logs", str(out / "peer0/runtime.log"), str(out / "peer1/runtime.log"), "--trigger", args.link_trigger,
           "--trigger-delay", str(args.link_delay)]
    (out / "peer0").mkdir(parents=True, exist_ok=True)
    (out / "peer1").mkdir(parents=True, exist_ok=True)
    return subprocess.Popen(cmd)


def stop_relay(relay, out):
    """Stop the relay (SIGINT: it writes its stats), and say INVALID when it carried nothing."""
    import signal
    relay.send_signal(signal.SIGINT)
    try:
        relay.wait(10)
    except subprocess.TimeoutExpired:
        relay.kill()
        relay.wait()
    try:
        stats = json.loads((out / "link-stats.json").read_text())
    except (OSError, ValueError):
        return ["the link relay wrote no statistics"]
    carried = sum(d["out"] for d in stats["directions"].values())
    both = all(d["in_"] > 0 for d in stats["directions"].values())
    bad = []
    if carried == 0:
        bad.append("the link relay forwarded no datagram: the run did not go through the profile %s" % stats["spec"])
    elif not both:
        bad.append("the link relay saw traffic in one direction only")
    return bad


PARTS_RE = re.compile(r"^\[mp-lan\] parts tick=(\d+) (.*)$", re.M)
END_RE = re.compile(r"^\[mp-lan\] end digest: score (-?\d+)-(-?\d+)(.*)$", re.M)
END_ENTRY_RE = re.compile(r"m([0-3])@(\d+)=([0-9a-f]{8})/([0-9a-f]{8})")
SYNC_RE = re.compile(r"^\[mp-lan\] sync tick=(\d+) m=(\d+) crc=([0-9a-f]{8}) seed=([0-9a-f]{8})", re.M)


NET_STEP_RE = re.compile(r"^\[mp-lan\] net( match)?: t=[\d.]+ s active=([\d.]+) s steps=(\d+) ", re.M)
MIN_REAL_TIME_RATIO = 0.6   # a console that ran its match below this share of real time (50 steps/s = 1.0) makes the run INVALID, not failed
MIN_ACTIVE_SECONDS = 10.0   # shorter matches say nothing about speed


def speed_ratio(text):
    """(ratio of real time, active seconds) of one console from its "[mp-lan] net" lines, or None when it logged none. The match totals
    are used when there are any (one per match), else the windows. 50 simulation steps per second of active match time is full speed."""
    totals, windows = [], []
    for m in NET_STEP_RE.finditer(text):
        (totals if m.group(1) else windows).append((float(m.group(2)), int(m.group(3))))
    rows = totals or windows
    active = sum(r[0] for r in rows)
    if not rows or active <= 0:
        return None
    return sum(r[1] for r in rows) / active / 50.0, active


def analyse(logs, codes, expect_lost=None, lobby_only=False, end_markers=None, check_speed=False):
    report = {"exit_codes": codes, "peers": [], "analysis_mode": "lobby-only" if lobby_only else "game"}
    syncs, parts, ends = [], [], []
    duplicate_bad = []
    zero_crc_lines = 0
    for path in logs:
        text = Path(path).read_text(errors="replace")
        script_stop = any(re.search(r"^\[plat\] script stop condition at frame %s, exiting$" % stop.group(1),
                                    text[stop.end():], re.M)
                          for stop in re.finditer(r"^\[script\] f=(\d+) stopwhen: .+$", text, re.M))
        peer = {
            "log": str(path),
            "desync": re.findall(r"^\[mp-lan\] DESYNC.*$", text, re.M),
            "sync_error": len(re.findall(r"^\[mp-lan\] sync error", text, re.M)),
            "overflow": len(re.findall(r"^\[mp-lan\] input queue overflow", text, re.M)),
            "connection_lost": len(re.findall(r"^\[mp-lan\] connection lost", text, re.M)),
            "malformed": len(re.findall(r"^\[mp-lan\] malformed", text, re.M)),
            "lan_on": bool(re.search(r"^\[mp-lan\] LAN mode on", text, re.M)),
            "script_timeout": "[script] TIMEOUT" in text,
            "script_logs": re.findall(r"^\[script\] f=\d+ (.*)$", text, re.M)[-14:],
            "score_events": re.findall(r"^\[nav\] f=\d+ (score \d+-\d+ -> \d+-\d+ \(clock [\d.]+\))", text, re.M),
            "crash": bool(re.search(r"SIGSEGV|SIGABRT|SIGILL|Segmentation fault|NP STUB:.*abort", text)),
            "stubs": sorted(set(re.findall(r"^NP STUB: (\S+)", text, re.M)))[:20],
            "test_score": re.findall(r"^\[mp-lan\] test score.*$", text, re.M),
            "fatal": bool(re.search(r"\[fatal\]|FATAL:|ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:|Assertion .*failed", text)),
            "script_exit": bool(re.search(r"^\[script\] f=\d+ exit$", text, re.M)),
            "script_stop": script_stop,
        }
        d = {}
        last = {}
        match = 0
        boundaries = iter(END_RE.finditer(text))
        boundary = next(boundaries, None)
        for sync in SYNC_RE.finditer(text):
            # An explicit end partitions even equal sampled ticks. Clear the fallback history so the following tick drop cannot count twice.
            while boundary is not None and boundary.start() < sync.start():
                match += 1
                last.clear()
                boundary = next(boundaries, None)
            tick, m, crc, seed = sync.groups()
            zero_crc_lines += crc in ("00000000", "ffffffff")
            tick, m = int(tick), int(m)
            # the tick counter restarts with every match of a series: number the matches (a drop of the tick counter starts a new one)
            if m in last and tick < last[m] and m == 0:
                match += 1
            last[m] = tick
            key, value = (match, tick, m), (crc, seed)
            if key in d and d[key] != value:
                duplicate_bad.append((len(syncs), key, d[key], value))
            d[key] = value
        peer_ends, previous_end = [], 0
        for end in END_RE.finditer(text):
            expected = {int(sync.group(2)) for sync in SYNC_RE.finditer(text, previous_end, end.start())}
            peer_ends.append((end.groups(), expected))
            previous_end = end.end()
        ends.append(peer_ends)
        pd, last_p, pm = {}, None, 0
        for tick, rest in PARTS_RE.findall(text):
            tick = int(tick)
            if last_p is not None and tick < last_p:
                pm += 1
            last_p = tick
            pd[(pm, tick)] = rest
        parts.append(pd)
        peer["parts_lines"] = len(pd)
        peer["sync_lines"] = len(d)
        peer["last_sync_tick"] = max((t for _, t, _ in d), default=0)
        peer["matches_seen"] = len({g for g, _, _ in d})
        syncs.append(d)
        report["peers"].append(peer)
    # inside each log: both machines' values at one tick
    within_bad = []
    for i, d in enumerate(syncs):
        for (g, t, m), v in d.items():
            other = d.get((g, t, 1 - m))
            if other is not None and other != v and m == 0:
                within_bad.append((i, g, t))
    # between the logs: the same (tick, machine)
    common = sorted(set(syncs[0]) & set(syncs[1])) if len(syncs) == 2 else []
    cross_bad = [k for k in common if syncs[0][k] != syncs[1][k]]
    pcommon = sorted(set(parts[0]) & set(parts[1])) if len(parts) == 2 else []
    parts_bad = [k for k in pcommon if parts[0][k] != parts[1][k]]
    report["parts_compared"] = len(pcommon)
    report["parts_mismatch"] = parts_bad[:10]
    cross_bad = cross_bad + [("parts",) + k for k in parts_bad]
    # A9: the end-of-match digest (final score, last checksum/seed per machine) of every match must be the same on both consoles
    end_bad = []
    parsed_ends = []
    for i, peer_ends in enumerate(ends):
        parsed = []
        for k, (end, expected) in enumerate(peer_ends):
            digest, machines = {}, {}
            for token in end[2].split():
                entry = END_ENTRY_RE.fullmatch(token)
                if not entry:
                    end_bad.append((k, "malformed", i, token))
                    continue
                m, t, crc, seed = entry.groups()
                key, value = (int(m), int(t)), (crc, seed)
                if key in digest and digest[key] != value:
                    end_bad.append((k, "duplicate", i, key, digest[key], value))
                if key[0] in machines and machines[key[0]] != key:
                    end_bad.append((k, "duplicate-machine", i, machines[key[0]], key))
                digest[key] = value
                machines[key[0]] = key
            if not digest:
                end_bad.append((k, "empty", i))
            if expected - set(machines):
                end_bad.append((k, "missing-machines", i, sorted(expected - set(machines))))
            parsed.append(digest)
        parsed_ends.append(parsed)
    if len(ends) == 2:
        if len(ends[0]) != len(ends[1]):
            end_bad.append(("count", len(ends[0]), len(ends[1])))
        for k, ((a, _), (b, _)) in enumerate(zip(ends[0], ends[1])):
            if a[:2] != b[:2]:
                end_bad.append((k, "score", a[:2], b[:2]))
            da, db = parsed_ends[0][k], parsed_ends[1][k]
            if set(da) != set(db):
                end_bad.append((k, "keys", sorted(da), sorted(db)))
            for key in set(da) & set(db):
                if da[key] != db[key]:
                    end_bad.append((k, "digest", key, da[key], db[key]))
    report["end_digests"] = [len(e) for e in ends]
    report["end_digest_mismatch"] = end_bad[:10]
    report["sync_ticks_compared"] = len(common)
    report["sync_mismatch_within_log"] = within_bad[:10]
    report["sync_mismatch_between_logs"] = cross_bad[:10]
    report["sync_conflicting_duplicates"] = duplicate_bad[:10]
    report["sync_conflicting_duplicate_lines"] = len(duplicate_bad)
    report["sync_zero_crc_lines"] = zero_crc_lines
    clean = all(not p["desync"] and not p["sync_error"] and not p["overflow"] and not p["malformed"]
                and not p["script_timeout"] and not p["crash"] and not p["fatal"]
                and p["lan_on"] and (p["script_exit"] or p["script_stop"]) for p in report["peers"])
    lost_ok = True
    if expect_lost is not None:
        # "N": the same count on both consoles; "H,J": host and joiner (a scripted disconnect: the console that
        # leaves abruptly never sees a loss, so a host-side disconnect test is "1,0" or "0,1")
        want = [int(x) for x in str(expect_lost).split(",")]
        if len(want) == 1:
            want = want * len(report["peers"])
        lost_ok = [p["connection_lost"] for p in report["peers"]] == want
    lobby_complete = True
    if lobby_only:
        # Lobby tests deliberately never enter a simulated match. Require their
        # explicit terminal states instead of silently relaxing the game gate.
        lobby_complete = expect_lost is not None and len(logs) == 2 and bool(end_markers) and len(end_markers) == 2
        for index, peer in enumerate(report["peers"]):
            text = Path(peer["log"]).read_text(errors="replace")
            marker = end_markers[index] if end_markers and index < len(end_markers) else ""
            end = re.search(r"^\[script\] f=\d+ %s$" % re.escape(marker), text, re.M) if marker else None
            exits = list(re.finditer(r"^\[script\] f=\d+ exit$", text, re.M))
            peer["end_marker"] = bool(end and exits and end.end() < exits[-1].start())
            lobby_complete = lobby_complete and peer["lan_on"] and peer["end_marker"]
    # INVALID: the game ran below 0.6 of real time on a console (an overloaded machine). Lockstep stretches, so a pair can still agree on every
    # checksum, but the timing results (starved steps, input delay, stalls) of such a run mean nothing and must not be read as pass or fail.
    invalid = []
    if check_speed:
        for peer in report["peers"]:
            sp = speed_ratio(Path(peer["log"]).read_text(errors="replace"))
            peer["real_time_ratio"] = round(sp[0], 3) if sp else None
            if sp and sp[1] >= MIN_ACTIVE_SECONDS and sp[0] < MIN_REAL_TIME_RATIO:
                invalid.append("%s ran at %.2f of real time (below %.1f)" % (peer["log"], sp[0], MIN_REAL_TIME_RATIO))
    report["invalid"] = invalid
    report["verdict"] = "INVALID" if invalid else None
    report["passed"] = bool(not invalid and clean and codes == [0, 0] and (common or lobby_only) and lobby_complete
                            and not within_bad and not cross_bad and not duplicate_bad and not end_bad and not zero_crc_lines and lost_ok)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--binary1", default=None, help="the joiner's executable when it differs (a mixed pair: an older build against this one)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--host-script", default=str(LAN / "lan_host.txt"))
    parser.add_argument("--join-script", default=str(LAN / "lan_join.txt"))
    parser.add_argument("--max-frames", type=int, default=60000)
    parser.add_argument("--timeout", type=int, default=1500)
    parser.add_argument("--virtual-time", action="store_true", help="MSC_VIRTUAL_TIME=1 (the two processes' virtual clocks are independent: unreliable for pairs)")
    parser.add_argument("--port-base", type=int, default=41000)
    parser.add_argument("--qemu-arm", action="store_true", help="--binary is an armhf build: run it under qemu-arm (Cortex-A9)")
    parser.add_argument("--watch-every", type=int, default=600)
    parser.add_argument("--expect-lost", default=None, help="expected number of 'connection lost' lines: N for both consoles, or HOST,JOINER (scripted disconnect where one console just leaves: 1,0 or 0,1)")
    parser.add_argument("--analyse-only", action="store_true")
    parser.add_argument("--lobby-only", action="store_true", help="explicit lobby-only gate: no shared match CRC required; needs --expect-lost and both --end-marker options")
    parser.add_argument("--end-marker0", default="", help="required host script log before its exit (lobby-only)")
    parser.add_argument("--end-marker1", default="", help="required joiner script log before its exit (lobby-only)")
    parser.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for both consoles")
    parser.add_argument("--env0", action="append", default=[], help="extra KEY=VALUE for the host only")
    parser.add_argument("--env1", action="append", default=[], help="extra KEY=VALUE for the joiner only")
    parser.add_argument("--link", default=None, help="bad-network relay between the consoles (link_relay.py): a profile (lan, home_wifi, bad_wifi, hotspot, terrible, "
                        "outage5/10/15/30) with optional overrides, e.g. bad_wifi,loss_pct=8; an outage starts --link-delay seconds after the first [mp-lan] sync line of a console "
                        "(use --env MSC_LAN_SYNC_LOG=100) or at outage_at=SECONDS after the start")
    parser.add_argument("--link-delay", type=float, default=5.0)
    parser.add_argument("--link-trigger", default=r"\[mp-lan\] sync tick=")
    args = parser.parse_args()
    if args.lobby_only and (args.expect_lost is None or not args.end_marker0 or not args.end_marker1):
        parser.error("--lobby-only requires --expect-lost, --end-marker0 and --end-marker1")
    # (virtual time: the clocks run free, the ratio says nothing)
    analysis = {"lobby_only": args.lobby_only, "end_markers": [args.end_marker0, args.end_marker1], "check_speed": not args.virtual_time and not args.lobby_only}
    out = Path(args.out).resolve()
    if args.analyse_only:
        codes = json.loads((out / "codes.json").read_text())
        report = analyse([out / "peer0/runtime.log", out / "peer1/runtime.log"], codes, args.expect_lost, **analysis)
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        return 2 if report["invalid"] else (0 if report["passed"] else 1)
    out.mkdir(parents=True, exist_ok=False)
    binaries = [Path(args.binary).resolve(), Path(args.binary1 or args.binary).resolve()]
    procs, logs = [], []
    relay = None
    relay_base = args.port_base + 100
    if args.link:
        relay = start_relay(args, out, relay_base)
    for index, script in enumerate((args.host_script, args.join_script)):
        extra = dict(item.split("=", 1) for item in args.env + (args.env0 if index == 0 else args.env1))
        if relay:   # each console is told that its partner lives at the relay's stand-in port
            extra.setdefault("MSC_LINK_BCAST", "127.0.0.1:%d" % (relay_base + 1 - index))
        p, log = launch(binaries[index], out, index, Path(script).resolve(), args.max_frames, extra, args.virtual_time,
                        args.qemu_arm, args.port_base, args.watch_every)
        procs.append(p)
        logs.append(log)
        time.sleep(0.5)
    deadline = time.time() + args.timeout
    codes = [None, None]
    while time.time() < deadline and None in codes:
        for i, p in enumerate(procs):
            if codes[i] is None:
                codes[i] = p.poll()
        time.sleep(1)
    for i, p in enumerate(procs):
        if codes[i] is None:
            p.terminate()
            try:
                codes[i] = p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
                codes[i] = p.wait()
            codes[i] = "timeout(%s)" % codes[i]
    (out / "codes.json").write_text(json.dumps(codes))
    link_invalid = stop_relay(relay, out) if relay else []
    report = analyse(logs, codes, args.expect_lost, **analysis)
    if link_invalid:
        report["invalid"] = list(report["invalid"]) + link_invalid
        report["passed"] = False
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if report["invalid"]:
        print("INVALID: %s" % "; ".join(report["invalid"]), file=sys.stderr)
    return 2 if report["invalid"] else (0 if report["passed"] else 1)


if __name__ == "__main__":
    sys.exit(main())
