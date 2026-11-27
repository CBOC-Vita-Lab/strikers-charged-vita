#!/usr/bin/env python3
"""The pass rules of the online runs (unit ON of r17-sitting-extra.txt), read from one run's log.

    python3 tools/console-1.2/online_check.py <log.txt> --run on-hub [--json]
    python3 tools/console-1.2/online_check.py --self-test

A console log or a host log (the host proof, online_host_proof.py). Exit 0 = PASS, 1 = FAIL, 2 = INVALID (the log is not what the run claims: no request lines, the route did
not finish). Rules (CONSOLE-PLAN-r17.md, unit ON):
  * every STATE line of the run's route, in order, and `script exit`; no `[script] TIMEOUT` / `cannot parse`; no crash words (FATAL, psp2core, Segmentation fault);
  * `[np-net] req N T+s METHOD /path host=H -> STATUS ...` (MSC_NET_REQLOG=1, np_reqlog.h): every request ANSWERED (a 2xx, or a 4xx / 5xx the service itself wrote: it carries code=...);
    a `-> FAIL` line is a failure (expected only in on-conn, where the address cannot resolve);
  * no `trying the other address` line in an auto run (a fallback line means the new address failed: reported as FAIL, the run is still valid evidence), host=new on every request of an
    auto run, host=old in the old variant (the host proof points MSC_ONLINE_URL at a local service: host=other is accepted there with --local);
  * at most 100 requests in any 10 s window (the zone's shared rule) and the per-minute count is printed against the budget figure; the queue runs hold at least 45 s with no
    opponent (real seconds from the request times / `[watch] rt=` lines);
  * pollScale / over_budget `level=` / retryAfterSec seen in the log are listed.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_online as GO  # noqa: E402

REQ = re.compile(r"\[np-net\] req (\d+) T\+(\d+)\.(\d)s (\S+) (\S+) host=(\S+) -> (?:(\d{3}) (\d+) ms reused=(\d)(.*)|FAIL (\S+) \((.*?)\) (\d+) ms)\s*$")
STATE = re.compile(r"\[script\] f=(\d+) STATE (.*?)\s*$")
CRASH = ("FATAL", "psp2core", "Segmentation fault", "SIGSEGV", "Aborted", "abort()")
WINDOW_LIMIT = 100          # requests per 10 s per address (the shared zone rule)
DAILY_BUDGET = 45000        # DAILY_REQUEST_BUDGET of the service (server/wrangler.toml)
QUEUE_RUNS = ("on-quick", "on-ranked", "mp-quick")
QUEUE_STATES = {"on-quick": ("quick match searching", "search cancelled"), "on-ranked": ("ranked searching", "search cancelled"), "mp-quick": ("quick match searching", "search cancelled")}
LAN_RUNS = ("mp-lan-wifi", "mp-lan-adhoc")   # (mp18) local play: nothing goes to the online service, so no request lines are wanted
VITA_ONLY = re.compile(r"\[vita\]|UDP port|relay")   # (mp18) words of the Vita build (fourth-core guard, the bind chain, the TURN relay): a host proof (--local) has none


def route_key(run):
    return next(sp["route"] for sp in GO.RUN_SPECS if sp["id"] == run)


def expected_states(run):
    key = route_key(run)
    return GO.ROUTES[key][2] if key in GO.ROUTES else []


def parse(text):
    reqs, states, watch = [], [], []
    for ln in text.splitlines():
        m = REQ.search(ln)
        if m:
            g = m.groups()
            if g[6]:
                reqs.append(dict(n=int(g[0]), t=int(g[1]) + int(g[2]) / 10.0, method=g[3], path=g[4], host=g[5], status=int(g[6]), ms=int(g[7]), extra=g[9] or "", fail=None))
            else:
                reqs.append(dict(n=int(g[0]), t=int(g[1]) + int(g[2]) / 10.0, method=g[3], path=g[4], host=g[5], status=0, ms=int(g[12]), extra="", fail=g[10]))
            continue
        m = STATE.search(ln)
        if m:
            states.append((int(m.group(1)), m.group(2)))
            continue
        m = re.search(r"\[watch\] f=(\d+) .*? rt=([\d.]+)", ln)
        if m:
            watch.append((int(m.group(1)), float(m.group(2))))
    return reqs, states, watch


def real_seconds(watch, frame):
    """wall seconds at a frame, interpolated between the [watch] lines (rt= real seconds)"""
    if not watch:
        return None
    pts = sorted(watch)
    if frame <= pts[0][0]:
        return pts[0][1]
    for (f0, r0), (f1, r1) in zip(pts, pts[1:]):
        if f0 <= frame <= f1 and f1 > f0:
            return r0 + (r1 - r0) * (frame - f0) / (f1 - f0)
    return pts[-1][1]


def max_window(reqs, width=10.0):
    best, j = 0, 0
    ts = [r["t"] for r in reqs]
    for i in range(len(ts)):
        while ts[i] - ts[j] >= width:
            j += 1
        best = max(best, i - j + 1)
    return best


def max_minute(reqs):
    return max_window(reqs, 60.0)


def check(text, run, local=False):
    """returns (verdict 'PASS' | 'FAIL' | 'INVALID', [lines])"""
    spec = next((sp for sp in GO.RUN_SPECS if sp["id"] == run), None)
    if spec is None:
        return "INVALID", ["unknown run id %s" % run]
    out, bad, invalid = [], [], []
    reqs, states, watch = parse(text)
    # the route
    names = [s for _, s in states]
    want = expected_states(run)
    if run == "on-report":
        if "[settings] save a report: written" not in text:
            invalid.append("no `[settings] save a report: written` line")
        else:
            out.append("report: `[settings] save a report: written`")
        if "[settings] screen open" not in text:
            invalid.append("no `[settings] screen open` line (the screen did not open)")
        out.append("(Settings screen run: no route, no requests; fetch mp-report.txt by hand)")
        return ("INVALID" if invalid else "PASS"), out + ["INVALID: " + x for x in invalid]
    pos = 0
    for s in want:
        try:
            pos = names.index(s, pos) + 1
        except ValueError:
            invalid.append("STATE line missing (or out of order): %r" % s)
            break
    else:
        out.append("route: all %d STATE lines in order" % len(want))
    if "script exit" not in text and "[plat] script exit" not in text:
        invalid.append("no `script exit` line (the route did not finish)")
    if "[script] TIMEOUT" in text or "cannot parse" in text:
        invalid.append("a script TIMEOUT / parse error line")
    for w in CRASH:
        if w in text:
            bad.append("crash word in the log: %s" % w)
    # the name was preset: the system keyboard never opens
    if "ime" in text.lower() and "[kbd]" in text and run != "on-conn":
        out.append("note: a keyboard line is in the log (check that the name came from MSC_ONLINE_NAME)")
    # the requests
    fb = [ln for ln in text.splitlines() if "trying the other address" in ln]
    auto = run != "on-hub-old"
    if run == "on-conn":
        fails = [r for r in reqs if r["fail"]]
        out.append("requests: %d, FAIL lines %d (expected: the address cannot resolve)" % (len(reqs), len(fails)))
        if not fails and not reqs:
            invalid.append("no request line at all (MSC_NET_REQLOG missing in the executable?)")
        if "connection test" not in text and "TEST CONNECTION" not in text and "test connection" not in text:
            out.append("note: no connection-test line in the log (read the popup by eye / the report)")
    elif run in LAN_RUNS:
        if reqs:
            bad.append("%d request(s) to the online service in a local run" % len(reqs))
        out.append("local run: no online request (as wanted)")
    else:
        if not reqs:
            invalid.append("no `[np-net] req` line (the executable has no MSC_NET_REQLOG, or the service was never asked)")
        fails = [r for r in reqs if r["fail"]]
        if fails:
            bad.append("%d request(s) FAILED before an answer: %s" % (len(fails), ", ".join("%s %s -> %s" % (r["method"], r["path"], r["fail"]) for r in fails[:5])))
        unanswered = [r for r in reqs if not r["fail"] and not (200 <= r["status"] < 300) and "code=" not in r["extra"]]
        if unanswered:
            bad.append("%d answer(s) with a non-2xx status and no service error code: %s" % (len(unanswered), ", ".join("%s %s %d" % (r["method"], r["path"], r["status"]) for r in unanswered[:5])))
        non2 = [r for r in reqs if r["status"] >= 300]
        if non2:
            out.append("service errors (answered): " + ", ".join("%s %s %d%s" % (r["method"], r["path"], r["status"], r["extra"]) for r in non2[:8]))
        if fb:
            bad.append("FALLBACK: %d `trying the other address` line(s): the new address failed (%s)" % (len(fb), fb[0].strip()[:120]))
        hosts = {}
        for r in reqs:
            hosts[r["host"]] = hosts.get(r["host"], 0) + 1
        out.append("requests: %d, by address %s" % (len(reqs), hosts))
        if reqs and not local:
            want_host = "new" if auto else "old"
            wrong = [h for h in hosts if h != want_host]
            if wrong:
                (bad if auto and wrong == ["old"] else bad).append("address(es) %s answered in a %s run (wanted %s only)" % (wrong, "auto" if auto else "old-only", want_host))
        if reqs and local and set(hosts) - {"other"}:
            out.append("note: local run with host labels %s" % sorted(hosts))
    if reqs:
        w10, w60 = max_window(reqs), max_minute(reqs)
        out.append("rate: at most %d requests in a 10 s window (limit %d), %d in a minute (the day's budget is %d requests)" % (w10, WINDOW_LIMIT, w60, DAILY_BUDGET))
        if w10 > WINDOW_LIMIT:
            bad.append("%d requests in 10 s is over the shared %d / 10 s rule" % (w10, WINDOW_LIMIT))
        if w10 > 40:
            out.append("note: more than 40 requests in a 10 s window: more than 2 consoles on one address would reach the zone rule")
        ps = sorted({m.group(1) for r in reqs for m in [re.search(r"pollScale=([\d.]+)", r["extra"])] if m})
        lv = sorted({m.group(1) for r in reqs for m in [re.search(r"level=(\S+)", r["extra"])] if m})
        ra = sorted({m.group(1) for r in reqs for m in [re.search(r"retryAfterSec=(\d+)", r["extra"])] if m})
        out.append("pacing words in the log: pollScale %s, budget level %s, retryAfterSec %s" % (ps or "none", lv or "none", ra or "none"))
        if ps or lv or ra:
            out.append("REPORT: the service sent pacing / budget words (above)")
        per = {}
        for r in reqs:
            k = "%s %s" % (r["method"], r["path"])
            per[k] = per.get(k, 0) + 1
        out.append("per call: " + ", ".join("%s x%d" % kv for kv in sorted(per.items(), key=lambda kv: -kv[1])[:8]))
    # the queue runs: the wait, in real seconds
    if run in QUEUE_RUNS:
        a, b = QUEUE_STATES[run]
        fa = next((f for f, s in states if s == a), None)
        fb_ = next((f for f, s in states if s == b), None)
        if fa is not None and fb_ is not None:
            ra_, rb_ = real_seconds(watch, fa), real_seconds(watch, fb_)
            secs = (rb_ - ra_) if (ra_ is not None and rb_ is not None) else (fb_ - fa) / 30.0
            out.append("queue wait: frames %d..%d, %.0f s (%s)" % (fa, fb_, secs, "real seconds from [watch] rt=" if ra_ is not None else "frames / 30"))
            if secs < 45:
                bad.append("the queue wait was only %.0f s (wanted about 60 s)" % secs)
        posts = [r for r in reqs if r["path"] == "/v1/ranked/queue" and r["method"] == "POST"]
        if not posts:
            bad.append("no POST /v1/ranked/queue: the queue was never joined")
        else:
            out.append("queue calls: %d POST /v1/ranked/queue (the poll runs about every 2 s)" % len(posts))
        if "matchmaking cancelled" not in text:
            bad.append("no `[ranked] matchmaking cancelled` line")
        if run == "on-quick" and "chose anyone (unranked quick match)" not in text:   # [online-menu] on host and console; the DWC `quick match: unranked` line is console only
            bad.append("the queue was not the quick one (no `chose anyone (unranked quick match)` line)")
    # (mp18) the multiplayer words of each single-Vita run (gen_online.MP18_RULES): the guard, the binds below 32768, the room's wait, the TURN relay
    for rx, why in GO.MP18_RULES.get(run, []):
        if local and VITA_ONLY.search(rx.replace("\\", "")) and "online-menu" not in rx:
            continue
        if re.search(rx, text):
            out.append("ok: " + why)
        else:
            bad.append("missing (%s): %s" % (rx.replace("\\", ""), why))
    for ln in text.splitlines():
        if "[online-menu] failure:" in ln and run.startswith("mp-"):
            bad.append("the game showed a failure popup: %s" % ln.strip()[:160])
            break
    if run in ("mp-hub", "mp-host", "mp-host-relay", "mp-quick") and "[online] login ok" not in text and "[online-menu] account" not in text:
        out.append("note: no `[online] login ok` / account line")
    if run in ("on-hub", "on-hub-old", "on-lists", "on-ranked", "mp-hub"):
        if "[online] login ok" not in text:
            bad.append("no `[online] login ok` line: the profile was not signed in")
        else:
            out.append("profile: `%s`" % re.search(r"\[online\] login ok[^\n]*", text).group(0).replace("\r", "")[:80])
    verdict = "INVALID" if invalid else ("FAIL" if bad else "PASS")
    return verdict, out + ["FAIL: " + x for x in bad] + ["INVALID: " + x for x in invalid]


def self_test():
    ok = True
    good = "\n".join(["[script] f=%d STATE %s" % (100 * i, s) for i, s in enumerate(expected_states("on-hub"), 1)]
                      + ["[np-net] req 1 T+0.0s GET /v1/info host=new -> 200 30 ms reused=0", "[np-net] req 2 T+0.1s POST /v1/rank/get host=new -> 200 31 ms reused=1 pollScale=1.2",
                         "[online] login ok: profile 7 (new account)", "[script] f=999 exit", "[plat] script exit at frame 999, exiting"])
    v, _ = check(good, "on-hub")
    ok &= v == "PASS"
    for name, bad_text, want in (
        ("fallback line", good + "\n[np-net] service host: DNS (dns), trying the other address", "FAIL"),
        ("old host in an auto run", good.replace("host=new", "host=old"), "FAIL"),
        ("a failed request", good + "\n[np-net] req 3 T+0.2s GET /v1/me host=new -> FAIL DNS (no) 6001 ms", "FAIL"),
        ("a bare 503", good + "\n[np-net] req 3 T+0.2s GET /v1/me host=new -> 503 30 ms reused=0", "FAIL"),
        ("a missing STATE line", good.replace("STATE hub read, leaving", "STATE x"), "INVALID"),
        ("no request lines", "\n".join(ln for ln in good.splitlines() if "req" not in ln), "INVALID"),
        ("a script timeout", good + "\n[script] TIMEOUT at f=5", "INVALID"),
        ("101 requests in 10 s", good + "\n" + "\n".join("[np-net] req %d T+1.0s GET /v1/x host=new -> 200 1 ms reused=1" % i for i in range(3, 110)), "FAIL"),
        ("a crash word", good + "\nFATAL: x", "FAIL"),
    ):
        got, _ = check(bad_text, "on-hub")
        print("self-test %-26s -> %s (wanted %s)" % (name, got, want))
        ok &= got == want
    old = good.replace("host=new", "host=old")
    ok &= check(old, "on-hub-old")[0] == "PASS"
    ok &= check(good, "on-hub-old")[0] == "FAIL"   # the old-only run must not see the new address
    # (mp18) the single-Vita multiplayer runs
    lan = "\n".join(["[script] f=%d STATE %s" % (100 * i, s) for i, s in enumerate(expected_states("mp-lan-wifi"), 1)]
                     + ["[vlink-vita] UDP port 25100 bound (asked 25100, 1 bind tried)", "[vita] core 3 rules: an online or LAN session is up: 3 thread(s) moved off the system core (they come back 3000 ms after the session ends)",
                        "[vita] core 3 rules: no online session any more: 3 thread(s) back on the system core", "[script] f=9999 exit", "[plat] script exit at frame 9999, exiting"])
    for name, text, run, want, local in (
        ("lan wifi: all words", lan, "mp-lan-wifi", "PASS", False),
        ("lan wifi: no guard return", lan.replace("no online session any more", "x"), "mp-lan-wifi", "FAIL", False),
        ("lan wifi: bind 41000", lan.replace("UDP port 25100 bound (asked 25100", "UDP port 41000 bound (asked 41000"), "mp-lan-wifi", "FAIL", False),
        ("lan wifi: a request", lan + "\n[np-net] req 1 T+0.0s GET /v1/info host=new -> 200 30 ms reused=0", "mp-lan-wifi", "FAIL", False),
        ("lan wifi host proof: no Vita words", "\n".join(ln for ln in lan.splitlines() if "[vita]" not in ln and "UDP port" not in ln), "mp-lan-wifi", "PASS", True),
        ("lan wifi: failure popup", lan + "\n[online-menu] failure: The online connection ended.", "mp-lan-wifi", "FAIL", False),
    ):
        got, _ = check(text, run, local)
        print("self-test %-34s -> %s (wanted %s)" % (name, got, want))
        ok &= got == want
    host = "\n".join(["[script] f=%d STATE %s" % (100 * i, s) for i, s in enumerate(expected_states("mp-host-relay"), 1)]
                      + ["[np-net] req 1 T+0.0s POST /v1/rooms host=new -> 200 30 ms reused=0", "[np-net] relay: allocating on 1.2.3.4:3478", "[np-net] relay ready: 5.6.7.8:40000",
                         "[online-menu] cancelled while connecting (stage 3)", "[script] f=9999 exit", "[plat] script exit at frame 9999, exiting"])
    for name, text, want in (("host relay: all words", host, "PASS"), ("host relay: no relay ready", host.replace("relay ready", "relay failed"), "FAIL"),
                             ("host relay: cancelled before the room", host.replace("(stage 3)", "(stage 1)"), "FAIL")):
        got, _ = check(text, "mp-host-relay")
        print("self-test %-34s -> %s (wanted %s)" % (name, got, want))
        ok &= got == want
    print("self-test:", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?")
    ap.add_argument("--run")
    ap.add_argument("--local", action="store_true", help="host proof: the service is a local one, host=other is expected")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.log or not a.run:
        ap.error("a log and --run")
    verdict, lines = check(Path(a.log).read_text(errors="replace"), a.run, a.local)
    if a.json:
        print(json.dumps({"run": a.run, "verdict": verdict, "lines": lines}))
    else:
        print("%s: %s" % (a.run, verdict))
        for ln in lines:
            print("  " + ln)
    return {"PASS": 0, "FAIL": 1, "INVALID": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main())
