#!/usr/bin/env python3
"""Two native game processes playing over the ONLINE transport (platform/sdk/net/online) through the NAT / loss / latency simulator.

Same checks as native_pair.py (lockstep checksums and seeds equal on both consoles at every logged tick, no DESYNC / sync error / queue overflow /
malformed message / crash), but the consoles do not talk over the LAN backend: each one starts an online session (MSC_NET_MODE=host | join, the
file-based fake signaling in <out>/signal, or --service: the local fake of the online service; the invite code is handed over through a file), asks the simulator's STUN servers for its public address, punches a hole
through the simulated NATs and then plays encrypted datagrams routed by the simulator (tools/net/natsim_proxy.cpp). The game's lobby, lockstep
and keep-alives run unchanged on top.

  python3 tools/mp/online_pair.py --binary <exe> --out <dir> --proxy <natsim_proxy> [--nat0 port] [--nat1 sym] [--same-lan] [--loss 2] [--delay 75]
        [--jitter 10] [--reorder 1] [--dup 0] [--cmd 40:down\\ 0 --cmd 55:up\\ 0] [native_pair.py options: --host-script ... --env ... --expect-lost ...]
  --menu (with --service): the consoles are NOT told to host / join by the environment: their scripts drive the game's own menus (MULTIPLAYER, the
        HOST / JOIN popup, the name, the invite code; MSC_LAN_MODE=online) and the account is made by the game itself on the fake service. The host types
        its name with the test keyboard (MSC_KEYBOARD_NAME), the joiner has MSC_ONLINE_NAME; the joiner's code keyboard waits for the host's invite file.
Run through heavy.sh (two game processes at once). Options this script does not know are passed to native_pair.py.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import native_pair  # noqa: E402


def service_url_checked(url, ap=None):
    """--service-url: a local service only (http on a loopback address); a pair run must never reach the live service."""
    from urllib.parse import urlsplit
    u = urlsplit(url)
    if u.scheme != "http" or u.hostname not in ("127.0.0.1", "localhost") or u.path not in ("", "/") or u.query or u.fragment:
        msg = "--service-url must be http://127.0.0.1:<port> (a local service), not %r" % url
        if ap:
            ap.error(msg)
        raise ValueError(msg)
    return "http://127.0.0.1:%d" % (u.port or 80)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, add_help=True)
    ap.add_argument("--proxy", default="", help="the natsim_proxy executable (not needed with --no-proxy)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sim-port", type=int, default=47000)
    ap.add_argument("--nat0", default="port")
    ap.add_argument("--nat1", default="port")
    ap.add_argument("--same-lan", action="store_true")
    ap.add_argument("--loss", default="0")
    ap.add_argument("--delay", default="0")
    ap.add_argument("--jitter", default="0")
    ap.add_argument("--reorder", default="0")
    ap.add_argument("--dup", default="0")
    ap.add_argument("--idle", default="120000")
    ap.add_argument("--invite", default="TEST42")
    ap.add_argument("--cmd", action="append", default=[], help="SECONDS:command for the simulator's stdin, seconds after the consoles started (down 0, up 0, flush 0, impair L D J)")
    ap.add_argument("--cmd-after", action="append", default=[], help="REGEX@SECONDS@command: when the host console's log shows REGEX, wait SECONDS and send the command to the simulator (down 0, up 0 ...)")
    ap.add_argument("--service-reset-after", action="append", default=[], help="REGEX@SECONDS (with --service): when the host console's log shows REGEX, wait SECONDS and wipe the fake service (POST /__fake/reset): the next "
                    "poll of both consoles is refused (the account is unknown), which the transport must report as a warning while the match goes on")
    ap.add_argument("--service", action="store_true", help="signaling through the online service (the local fake: --fake-server) instead of the file fake")
    ap.add_argument("--fake-server", default=os.environ.get("FAKE_SERVER", ""), help="path of server/fake/server.mjs (or FAKE_SERVER)")
    ap.add_argument("--node", default=os.environ.get("NODE", "node"))
    ap.add_argument("--service-port", type=int, default=0, help="HTTP port of the fake (default: sim port + 5000)")
    ap.add_argument("--service-url", default="", help="(with --service) an online service that is ALREADY running locally, e.g. the real Worker under `wrangler dev` (http://127.0.0.1:8787): "
                    "no fake is started; only loopback addresses are accepted (a pair run never talks to the live service). --turn then needs the Worker's TURN stub variables")
    ap.add_argument("--turn", action="store_true", help="the test TURN server (tools/net/turnsim.cpp, inside the simulator) at 198.51.100.9:3478; credentials are minted with --turn-secret")
    ap.add_argument("--turn-secret", default="turn-test-secret")
    ap.add_argument("--turn-opts", default="", help="extra simulator options for the TURN server, e.g. '--turn-lifetime 40 --turn-perm 30'")
    ap.add_argument("--turn-ttl", default="3600", help="credential lifetime in seconds (file-fake signaling)")
    ap.add_argument("--relay-only", default="", help="which consoles play 'Relay only': 0, 1 or 01 (MSC_NET_RELAY_ONLY=1; with --service the profile flag is registered as well)")
    ap.add_argument("--menu", action="store_true", help="drive the game's online menus (see above); needs --service")
    ap.add_argument("--no-proxy", action="store_true", help="no simulator: the consoles use the real loopback (MSC_NET_LOCAL_IP) and no STUN")
    args, rest = ap.parse_known_args()
    if not args.proxy and not args.no_proxy:
        ap.error("--proxy <natsim_proxy> is required unless --no-proxy is given")
    if args.service_reset_after and (not args.service or args.service_url):
        ap.error("--service-reset-after needs --service with the fake (not --service-url: only the fake has /__fake/reset)")
    if args.service_url:
        if not args.service:
            ap.error("--service-url needs --service")
        service_url_checked(args.service_url, ap)
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "signal").mkdir(exist_ok=True)

    proxy = None
    proxy_log = open(out / "proxy.log", "w")
    if not args.no_proxy:
        cmd = [args.proxy, "--port", str(args.sim_port), "--nat0", args.nat0, "--nat1", args.nat1, "--loss", args.loss, "--delay", args.delay,
               "--jitter", args.jitter, "--reorder", args.reorder, "--dup", args.dup, "--idle", args.idle]
        if args.same_lan:
            cmd.append("--same-lan")
        if args.turn:
            cmd += ["--turn", "--turn-secret", args.turn_secret] + args.turn_opts.split()
        proxy = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=proxy_log, stderr=subprocess.STDOUT, text=True, bufsize=1)
        for _ in range(100):
            time.sleep(0.05)
            if "ready" in (out / "proxy.log").read_text():
                break

    fake = None
    if args.service:
        import json
        import urllib.request
        if args.service_url:
            base = service_url_checked(args.service_url, ap)
        else:
            port = args.service_port or args.sim_port + 5000
            fake_log = open(out / "service.log", "w")
            fake_env = dict(os.environ)
            if args.turn:
                fake_env.update({"TURN_PROVIDER": "stub", "TURN_STUB_URLS": "turn:198.51.100.9:3478?transport=udp,turn:198.51.100.9:53?transport=udp", "TURN_STUB_SECRET": args.turn_secret})
            fake = subprocess.Popen([args.node, args.fake_server, "--http-port", str(port), "--https-port", str(port + 1)], stdout=fake_log, stderr=subprocess.STDOUT, env=fake_env)
            base = "http://127.0.0.1:%d" % port
        tokens = []
        for _ in range(120):
            time.sleep(0.25)
            try:
                urllib.request.urlopen(base + "/v1/info", timeout=1).read()
                break
            except Exception:
                continue
        for idx, name in enumerate(() if args.menu else ("Sam1", "Sam2")):
            rq = urllib.request.Request(base + "/v1/register", data=json.dumps({"name": name, "region": 2, "relayOnly": str(idx) in args.relay_only}).encode(),
                                        headers={"Content-Type": "application/json", "X-Msc-App": "1.2.0"})
            tokens.append(json.loads(urllib.request.urlopen(rq, timeout=5).read())["token"])
        if args.menu:
            common = ["MSC_LAN_MODE=online", "MSC_ONLINE_URL=" + base, "MSC_NET_INVITE_FILE=%s" % (out / "invite.txt")]
            extra_tok = None
        else:
            common = ["MSC_NET_SERVER=" + base, "MSC_NET_INVITE_FILE=%s" % (out / "invite.txt")]
            extra_tok = ["MSC_NET_TOKEN=" + tokens[0], "MSC_NET_TOKEN=" + tokens[1]]
    else:
        # the host's process writes its invite code to a file and the joiner's waits for it (the join of a room that does not exist yet would fail)
        common = ["MSC_NET_SIGNAL_DIR=%s" % (out / "signal"), "MSC_NET_INVITE_FILE=%s" % (out / "invite.txt")]
        if args.invite != "TEST42":
            common.append("MSC_NET_INVITE=" + args.invite)
        extra_tok = None
    env0 = ["MSC_NET_SIM_ID=0"] + ([] if args.menu else ["MSC_NET_MODE=host"])
    env1 = ["MSC_NET_SIM_ID=1"] + ([] if args.menu else ["MSC_NET_MODE=join"])
    if args.menu:
        if not args.service:
            ap.error("--menu needs --service")
        env0.append("MSC_KEYBOARD_NAME=Sam1")                       # the host types its name (the first run has no account)
        env1 += ["MSC_ONLINE_NAME=Sam2", "MSC_KEYBOARD_CODE_FILE=%s" % (out / "invite.txt")]   # the joiner's name is given; its code comes from the host's file
    if not args.no_proxy:
        common += ["MSC_NET_SIM=127.0.0.1:%d" % args.sim_port, "MSC_NET_STUN=198.51.100.1:3478,198.51.100.2:3478"]
        env0.append("MSC_NET_SIM_PRIV=192.168.1.10" if args.same_lan else "MSC_NET_SIM_PRIV=192.168.1.10")
        env1.append("MSC_NET_SIM_PRIV=192.168.1.11" if args.same_lan else "MSC_NET_SIM_PRIV=192.168.2.10")
    else:
        common += ["MSC_NET_LOCAL_IP=127.0.0.1", "MSC_NET_STUN=127.0.0.1:9", "MSC_NET_TRY_LOCAL=1"]
        env0.append("MSC_NET_PORT=%d" % (args.sim_port + 1))
        env1.append("MSC_NET_PORT=%d" % (args.sim_port + 2))
    if args.turn and not args.service:
        common += ["MSC_NET_TURN_URLS=turn:198.51.100.9:3478?transport=udp,turn:198.51.100.9:53?transport=udp", "MSC_NET_TURN_SECRET=" + args.turn_secret, "MSC_NET_TURN_TTL=" + args.turn_ttl]
    if "0" in args.relay_only:
        env0.append("MSC_NET_RELAY_ONLY=1")
    if "1" in args.relay_only:
        env1.append("MSC_NET_RELAY_ONLY=1")
    if extra_tok:
        env0.append(extra_tok[0])
        env1.append(extra_tok[1])
    fwd = []
    for e in common:
        fwd += ["--env", e]
    for e in env0:
        fwd += ["--env0", e]
    for e in env1:
        fwd += ["--env1", e]

    def scheduled():
        t0 = time.time()
        for item in sorted(args.cmd, key=lambda s: float(s.split(":", 1)[0])):
            secs, command = item.split(":", 1)
            time.sleep(max(0.0, t0 + float(secs) - time.time()))
            if proxy and proxy.poll() is None:
                proxy.stdin.write(command + "\n")
                proxy.stdin.flush()

    def after(spec):
        import re
        pattern, secs, command = spec.split("@", 2)
        log = out / "pair" / "peer0" / "runtime.log"
        rx = re.compile(pattern)
        while True:
            time.sleep(0.5)
            try:
                if rx.search(log.read_text(errors="replace")):
                    break
            except OSError:
                continue
        time.sleep(float(secs))
        if proxy and proxy.poll() is None:
            proxy.stdin.write(command + "\n")
            proxy.stdin.flush()

    def reset_service(spec):
        import re
        import urllib.request
        pattern, secs = spec.rsplit("@", 1)
        log = out / "pair" / "peer0" / "runtime.log"
        rx = re.compile(pattern)
        while True:
            time.sleep(0.5)
            try:
                if rx.search(log.read_text(errors="replace")):
                    break
            except OSError:
                continue
        time.sleep(float(secs))
        try:
            rq = urllib.request.Request(base + "/__fake/reset", data=b"{}", headers={"Content-Type": "application/json"})
            urllib.request.urlopen(rq, timeout=5).read()
            print("service wiped (%s)" % spec, flush=True)
        except Exception as e:
            print("service reset failed: %s" % e, flush=True)

    for spec in args.service_reset_after:
        threading.Thread(target=reset_service, args=(spec,), daemon=True).start()
    for spec in args.cmd_after:
        threading.Thread(target=after, args=(spec,), daemon=True).start()
    if args.cmd:
        threading.Thread(target=scheduled, daemon=True).start()
    sys.argv = ["native_pair.py", "--out", str(out / "pair")] + fwd + rest
    try:
        rc = native_pair.main()
    finally:
        if fake:
            fake.terminate()
            try:
                fake.wait(5)
            except subprocess.TimeoutExpired:
                fake.kill()
        if proxy:
            try:
                proxy.stdin.write("stats\n")
                proxy.stdin.flush()
            except Exception:
                pass
            time.sleep(0.3)
            proxy.terminate()
            try:
                proxy.wait(5)
            except subprocess.TimeoutExpired:
                proxy.kill()
    return rc


if __name__ == "__main__":
    sys.exit(main())
