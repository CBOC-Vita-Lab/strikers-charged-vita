#!/usr/bin/env python3
"""Host proof of the online runs (unit ON): each route on the HOST build against a LOCAL fake of the service, never the real one.

    python3 tools/console-1.2/online_host_proof.py --binary <np-host32-full-glnull> --disc <extracted game folder> --out <dir> [--runs on-hub,on-quick,...]

For every run the kit's env file (env/<id>.txt) gives the online switches (MSC_ONLINE_HOST, MSC_NET_REQLOG, MSC_ONLINE_NAME, MSC_ONLINE_FRIENDS); MSC_ONLINE_URL is pointed at the local fake
(server/fake/server.mjs, the Worker's own application code over an in-memory SQLite: a fresh one per run, plain HTTP on a loopback port) and, for on-conn, at a loopback port nobody listens on.
The route is the kit's routes/on-*.txt. The game runs in real time under `flock /tmp/s13-host-2` (the shared two-core machine's slot) at nice 15 (2.5 GB for the GL build; glnull needs less).
Then online_check.py --local reads the log. What the host cannot prove (the real service, the new address and its fallback, the console's TLS, the Settings screen run on-report) is said in the
summary: with a local http:// address the MSC_ONLINE_HOST rule does not apply (an address named with MSC_ONLINE_URL is used as written), so the log's host= label is `other`.
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import gen_online as GO  # noqa: E402
import online_check as OC  # noqa: E402

KEEP = ("MSC_ONLINE_", "MSC_NET_REQLOG")
LOCK = "/tmp/s13-host-2"


def env_of(run):
    out = {}
    for ln in (HERE / "env" / ("%s.txt" % run)).read_text().splitlines():
        if ln.startswith("#") or "=" not in ln:
            continue
        k, v = ln.split("=", 1)
        if k.startswith(KEEP):
            out[k] = v
    return out


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def rival(base, mode, log, stop, game_log):
    """--pair: a second player of the LOCAL fake (plain HTTP, never the real service) who joins the same queue once the game is in it, so the fake pairs the two.
    It only talks to the queue: the game then gets its pairing, joins the ranked room and polls it; the link itself needs a second game and is not made here."""
    import json
    import urllib.request

    def call(method, path, body=None, token=None):
        h = {"X-Msc-App": "1.3.0", "X-Msc-Proto": "1"}
        if body is not None:
            h["Content-Type"] = "application/json"
        if token:
            h["Authorization"] = "Bearer " + token
        rq = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(), method=method, headers=h)
        try:
            with urllib.request.urlopen(rq, timeout=30) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")
        except OSError as e:
            return 0, {"error": str(e)}

    st, j = call("POST", "/v1/register", {"name": "Rival01", "region": 2})
    token = j.get("token")
    log.write("rival: register %s profile %s\n" % (st, j.get("profileId", j.get("pid"))))
    while not stop.is_set():   # wait for the game's own queue entry
        if "POST /v1/ranked/queue" in Path(game_log).read_text(errors="replace"):
            break
        time.sleep(1)
    time.sleep(8)   # the game shows its search screen for a while first
    st, j = call("POST", "/v1/ranked/queue", {"mode": "quick"} if mode == "quick" else {}, token)
    log.write("rival: POST /v1/ranked/queue %s %s\n" % (st, j.get("status", j)))
    while not stop.is_set() and j.get("status") == "queued":
        time.sleep(2)
        st, j = call("GET", "/v1/ranked/queue", None, token)
    log.write("rival: queue view %s %s room %s\n" % (st, j.get("status", j), j.get("roomCode")))
    log.flush()


def one(run, binary, disc, out, node, maxf, timeout, tls=False, url_port=0, pair=False, hold_max=None):
    d = Path(out) / run
    shutil.rmtree(d, ignore_errors=True)
    (d / "data").mkdir(parents=True)
    spec = next(sp for sp in GO.RUN_SPECS if sp["id"] == run)
    env = {"PATH": os.environ["PATH"], "HOME": str(d), "NP_DISC_DIR": str(disc), "NP_DATA_DIR": str(d / "data"), "MSC_MAX_FRAMES": str(maxf), "MSC_WATCH": "1", "MSC_WATCH_EVERY": "600",
           "MSC_FRAME_LOG": "0", "MSC_INPUT_SCRIPT": str(HERE / "routes" / GO.ROUTES[spec["route"]][0])}
    env.update(env_of(run))
    svc = None
    if run == "on-conn":
        env["MSC_ONLINE_URL"] = "http://127.0.0.1:%d" % free_port()   # nobody listens: the connect is refused at once (the console's run names a name that cannot resolve)
    else:
        port = free_port()
        tport = url_port or port + 1
        svc = subprocess.Popen([node, "fake/server.mjs", "--http-port", str(port), "--https-port", str(tport)], cwd=str(REPO / "server"), stdout=open(d / "service.log", "w"), stderr=subprocess.STDOUT,
                               env=dict(os.environ, **({"HOLD_MAX_SEC": str(hold_max)} if hold_max is not None else {})))
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", port), 0.3).close()
                break
            except OSError:
                time.sleep(0.25)
        env["MSC_ONLINE_URL"] = "http://127.0.0.1:%d" % port
        if tls:
            # the console's way: https through the game's own TLS client, the fake's throw-away root trusted by the test build (tools/net/console_rules_host.sh).
            # With --url-port the address is the one compiled into the build (NP_ONLINE_URL, as on the console) and MSC_ONLINE_URL is not set at all.
            env["NP_HTTPS_TEST_ROOTS"] = str(REPO / "server" / "fake" / "certs" / "root.pem")
            if url_port:
                del env["MSC_ONLINE_URL"]
            else:
                env["MSC_ONLINE_URL"] = "https://localhost:%d" % tport
    t0 = time.time()
    with open(d / "runtime.log", "w") as log:
        p = subprocess.Popen(["flock", LOCK, "nice", "-n", "15", str(binary)], cwd=str(d), env=env, stdout=log, stderr=subprocess.STDOUT)
        stop, th = None, None
        if pair and svc and run in OC.QUEUE_RUNS:
            import threading
            stop = threading.Event()
            th = threading.Thread(target=rival, args=("http://127.0.0.1:%d" % port, "quick" if run == "on-quick" else "ranked", open(d / "rival.log", "w"), stop, d / "runtime.log"), daemon=True)
            th.start()
        try:
            code = p.wait(timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            code = "timeout"
        if stop:
            stop.set()
            th.join(40)
    if svc:
        svc.terminate()
    text = (d / "runtime.log").read_text(errors="replace")
    verdict, lines = OC.check(text, run, local=True)
    if tls and run != "on-conn":   # every request must have gone through the TLS client, to the https address
        if "[online] service: https://localhost:" not in text:
            verdict, lines = "FAIL", lines + ["FAIL: no `[online] service: https://localhost:...` line (the sign-in did not use the TLS client)"]
        if "plain HTTP" in text:
            verdict, lines = "FAIL", lines + ["FAIL: a plain HTTP line in a console-rules run"]
        lines.append("tls: %d `[net-https]` lines" % text.count("[net-https] "))
    if pair and run in OC.QUEUE_RUNS:
        m = re.search(r"\[ranked\] paired: room \S+, match \S+, slot \d", text)
        room = re.findall(r"\[np-net\] req \d+ \S+ (?:GET|POST) /v1/rooms/\S+ \S+ -> 2\d\d", text)
        sess = "[np-net] online session started" in text
        mode = "quick match: unranked" in text if run == "on-quick" else "quick match: unranked" not in text
        ok = bool(m) and len(room) >= 2 and sess and mode
        # The second player is a script, not a game: the link can never come up, so after the pairing the game's own "no route" error popup ends the route
        # (it wants Cross; the route only knows the search screen). The pairing run is judged by the pairing, the room's requests and the queue's kind alone.
        verdict = "PASS" if ok and not any(w in text for w in OC.CRASH) else "FAIL"
        lines = [ln for ln in lines if not ln.startswith(("INVALID:", "FAIL: no `[ranked] matchmaking cancelled`"))]
        lines.append("pairing run (route end not judged): %s; online session started: %s; answered room requests: %d; queue kind as chosen: %s" % (m.group(0) if m else "NO `[ranked] paired` line", sess, len(room), mode))
    (d / "check.txt").write_text("%s: %s (exit %s, %.0f s)\n%s\n" % (run, verdict, code, time.time() - t0, "\n".join(lines)))
    return code, verdict, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--node", default=shutil.which("node") or "/opt/node22/bin/node")
    ap.add_argument("--runs", default=",".join(sp["id"] for sp in GO.RUN_SPECS if sp["id"] != "on-report"))
    ap.add_argument("--max-frames", type=int, default=12000)
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--tls", action="store_true", help="console rules: https to the fake through the game's TLS client (a binary of native/tools/net/console_rules_host.sh)")
    ap.add_argument("--url-port", type=int, default=0, help="with --tls: the https port compiled into the binary (URL_PORT of console_rules_host.sh); MSC_ONLINE_URL is then not set")
    ap.add_argument("--pair", action="store_true", help="queue runs: a second player of the fake joins the queue, so the game is paired and joins the room")
    ap.add_argument("--hold-max", type=int, default=None, help="HOLD_MAX_SEC of the fake: the longest held poll in seconds (the service's default is 55; the game asks for 12)")
    a = ap.parse_args()
    bad = 0
    for run in a.runs.split(","):
        code, verdict, lines = one(run, a.binary, a.disc, a.out, a.node, a.max_frames, a.timeout, a.tls, a.url_port, a.pair, a.hold_max)
        print("%s: %s (game exit %s)" % (run, verdict, code), flush=True)
        for ln in lines:
            print("   " + ln, flush=True)
        bad |= (code != 0) or verdict != "PASS"
    print("on-report: console only (the Settings screen is Vita-only code; its key words are checked against np_settings.cpp by test_kit)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
