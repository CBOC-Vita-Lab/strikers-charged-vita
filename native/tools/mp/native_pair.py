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
  * (with --expect-lost N) the number of "connection lost" lines per console is as expected (scripted disconnects).
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
GAME = Path(os.environ.get("NP_DISC_DIR", "/home/claude/msc/game"))
LAN = Path(__file__).resolve().parent / "lan"
QEMU_ARM = os.environ.get("QEMU_ARM", "/home/claude/research/qemu-full/build/qemu-arm")


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


PARTS_RE = re.compile(r"^\[mp-lan\] parts tick=(\d+) (.*)$", re.M)
SYNC_RE = re.compile(r"^\[mp-lan\] sync tick=(\d+) m=(\d+) crc=([0-9a-f]{8}) seed=([0-9a-f]{8})", re.M)


def analyse(logs, codes, expect_lost=None):
    report = {"exit_codes": codes, "peers": []}
    syncs, parts = [], []
    for path in logs:
        text = Path(path).read_text(errors="replace")
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
        }
        d = {}
        last = {}
        match = 0
        for tick, m, crc, seed in SYNC_RE.findall(text):
            tick, m = int(tick), int(m)
            # the tick counter restarts with every match of a series: number the matches (a drop of the tick counter starts a new one)
            if m in last and tick < last[m] and m == 0:
                match += 1
            last[m] = tick
            d[(match, tick, m)] = (crc, seed)
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
        peer["matches_seen"] = match + 1 if d else 0
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
    report["sync_ticks_compared"] = len(common)
    report["sync_mismatch_within_log"] = within_bad[:10]
    report["sync_mismatch_between_logs"] = cross_bad[:10]
    report["sync_zero_crc_lines"] = sum(1 for d in syncs for v in d.values() if v[0] == "00000000" or v[0] == "ffffffff")
    clean = all(not p["desync"] and not p["sync_error"] and not p["overflow"] and not p["malformed"]
                and not p["script_timeout"] and not p["crash"] for p in report["peers"])
    lost_ok = True
    if expect_lost is not None:
        # "N": the same count on both consoles; "H,J": host and joiner (a scripted disconnect: the console that
        # leaves abruptly never sees a loss, so a host-side disconnect test is "1,0" or "0,1")
        want = [int(x) for x in str(expect_lost).split(",")]
        if len(want) == 1:
            want = want * len(report["peers"])
        lost_ok = [p["connection_lost"] for p in report["peers"]] == want
    report["passed"] = bool(clean and codes == [0, 0] and common and not within_bad and not cross_bad and lost_ok)
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
    parser.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for both consoles")
    parser.add_argument("--env0", action="append", default=[], help="extra KEY=VALUE for the host only")
    parser.add_argument("--env1", action="append", default=[], help="extra KEY=VALUE for the joiner only")
    args = parser.parse_args()
    out = Path(args.out).resolve()
    if args.analyse_only:
        codes = json.loads((out / "codes.json").read_text())
        report = analyse([out / "peer0/runtime.log", out / "peer1/runtime.log"], codes, args.expect_lost)
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        return 0 if report["passed"] else 1
    out.mkdir(parents=True, exist_ok=False)
    binaries = [Path(args.binary).resolve(), Path(args.binary1 or args.binary).resolve()]
    procs, logs = [], []
    for index, script in enumerate((args.host_script, args.join_script)):
        extra = dict(item.split("=", 1) for item in args.env + (args.env0 if index == 0 else args.env1))
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
    report = analyse(logs, codes, args.expect_lost)
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
