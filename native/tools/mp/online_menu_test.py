#!/usr/bin/env python3
"""The online entry of the game's own menus on the host, against the local fake of the online service (server/fake/server.mjs).

Cases (each one is made of real game processes whose nav scripts press the real buttons of the real menus; nothing is told to host or join by the
environment; MSC_LAN_MODE is only a highlighted preference):
  modes   one console: cancel the mode chooser, back from LOCAL, enter/leave SAME WI-FI and NEARBY, then discover ONLINE host/join without a restart
  pair    two consoles: MULTIPLAYER -> ONLINE -> HOST A ROOM (the name by the test keyboard, the account made on the service) / JOIN WITH A CODE (the code from the
          host's invite file) -> status popups -> the lobby -> Create Game / Join Game -> draft -> a short match; the lockstep checksums must agree
          (tools/mp/online_pair.py --menu, then the checks of native_pair.py)
  cancel  one console hosts, Circle on the "Waiting for your partner" popup returns to the main menu, and hosting again works
  wrong   one console joins with a code no room has: the failure popup says so, OK returns to the main menu
  lost    two consoles; the natsim simulator cuts the link after the match started (needs --proxy): the game ends the match, says so, and the lobby
          offers RECONNECT (A11 reconnect; see docs/ONLINE-MENU.md for why a match is not resumed)
  ranked  (1.3) two consoles: MULTIPLAYER -> ONLINE -> RANKED MATCH -> Online Central -> ranked queue on the service -> the game's ranked match over
          the online link -> both result reports -> the leaderboards show both players (not in the default list: --cases ranked)
  python3 tools/mp/online_menu_test.py --binary <np-host32 exe> --fake-server <server/fake/server.mjs> --out <dir> [--cases pair,cancel,wrong,lost]
        [--dump-every 25] [--proxy <natsim_proxy>] (default cases: modes,pair,cancel,wrong,badcode,conntest,lost)
The service refuses loopback addresses as endpoints (rooms.js checkEndpoint), so every console that gets as far as publishing an endpoint (pair, cancel,
lost) runs behind the NAT simulator (tools/net/natsim_proxy.cpp: private 192.168.x.y, public 203.0.113.x, two STUN servers). --proxy names a built one;
without it the test compiles one into <out> (a couple of seconds) and skips the cases that need it when that fails.
Run on a quiet host with real-time clocks. The game's PNG dumps (--dump-every N, needs the GL build) go to <out>/<case>/peer<i>/pics.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import native_pair  # noqa: E402

LAN = HERE / "lan"
GL_ENV = ["EGL_PLATFORM=surfaceless", "GALLIUM_DRIVER=llvmpipe", "LP_NUM_THREADS=2", "MSC_VGX_LEAD=0", "MSC_FRAMESEND=nofork"]
LOG_POLL_SECONDS = 0.05
OBSERVER_ALLOWANCE_SECONDS = 0.5
RECONNECT_LIMIT_SECONDS = 45.0
PUMP_ALLOWANCE_SECONDS = 1.0
POPUP_ALLOWANCE_SECONDS = 2.0


def require_nonreaping_wait():
    required = ("waitid", "P_PID", "WEXITED", "WNOHANG", "WNOWAIT")
    if any(getattr(os, name, None) is None for name in required):
        raise RuntimeError("online menu tests require POSIX nonreaping waitid with WNOWAIT")
    flags = os.WEXITED | os.WNOHANG | os.WNOWAIT
    try:
        os.waitid(os.P_PID, os.getpid(), flags)
    except ChildProcessError:
        pass  # The current process is not its own child; the kernel accepted the required flags.
    except OSError as error:
        raise RuntimeError("POSIX nonreaping waitid is unavailable") from error
    return flags


def cleanup_note(error, detail):
    message = "online pair cleanup incomplete: " + detail
    add_note = getattr(error, "add_note", None)
    if callable(add_note):
        add_note(message)
    else:
        print(message, file=sys.stderr)


class ReconnectObserver:
    """Timestamp existing line-buffered stderr, independently of game frame counts."""
    patterns = (
        ("start", re.compile(r"^\[np-net\] rebuilding the link \(generation \d+\)$")),
        ("choice", re.compile(r"^\[online-menu\] reconnecting$")),
        ("terminal", re.compile(r"^\[np-net\] failed:")),
        ("popup", re.compile(r"^\[online-menu\] lobby: reconnect failed: .+")),
    )

    def __init__(self, logs):
        self.logs = logs
        self.peers = [{"offset": 0, "pending": "", "events": [], "max_poll_gap": 0.0} for _ in logs]
        self.last_poll = time.monotonic()

    def poll(self):
        started = time.monotonic()
        gap = started - self.last_poll
        active = [False] * len(self.peers)
        for index, (path, peer) in enumerate(zip(self.logs, self.peers)):
            active[index] = any(e["event"] == "start" for e in peer["events"]) and not any(e["event"] == "popup" for e in peer["events"])
            try:
                with path.open("r", encoding="utf-8", errors="replace") as log:
                    log.seek(peer["offset"])
                    chunk = log.read()
                    peer["offset"] = log.tell()
            except FileNotFoundError:
                continue
            lines = (peer["pending"] + chunk).split("\n")
            peer["pending"] = lines.pop()
            for line in lines:
                for event, pattern in self.patterns:
                    if pattern.search(line) and (event == "start" or any(e["event"] == "start" for e in peer["events"])):
                        peer["events"].append({"event": event, "at": time.monotonic(), "line": line})
                        active[index] = True
        finished = time.monotonic()
        gap += finished - started
        self.last_poll = started  # Carry slow prior reads into the bound for a newly discovered rebuild.
        for peer, was_active in zip(self.peers, active):
            if was_active:
                peer["max_poll_gap"] = max(peer["max_poll_gap"], gap)

    def failure_within_window(self, index):
        peer = self.peers[index]
        events = {name: [e["at"] for e in peer["events"] if e["event"] == name] for name, _ in self.patterns}
        if any(len(events[name]) != 1 for name in ("start", "choice", "popup")) or not events["terminal"]:
            return False, "need one rebuild, reconnect choice and popup, plus a transport failure event"
        sequence = [e["event"] for e in peer["events"]]
        if not sequence.index("start") < sequence.index("choice") < sequence.index("terminal") < sequence.index("popup"):
            return False, "reconnect/transport/popup log lines were out of order"
        start, choice, terminal, popup = events["start"][0], events["choice"][0], events["terminal"][0], events["popup"][0]
        if not start <= choice <= terminal <= popup:
            return False, "reconnect/transport/popup events were out of order"
        if peer["max_poll_gap"] > OBSERVER_ALLOWANCE_SECONDS:
            return False, "observer missed its %.1f s sampling bound (gap %.3f s)" % (OBSERVER_ALLOWANCE_SECONDS, peer["max_poll_gap"])
        limit = RECONNECT_LIMIT_SECONDS + PUMP_ALLOWANCE_SECONDS + OBSERVER_ALLOWANCE_SECONDS
        if terminal - start > limit or popup - start > limit + POPUP_ALLOWANCE_SECONDS or popup - terminal > POPUP_ALLOWANCE_SECONDS + OBSERVER_ALLOWANCE_SECONDS:
            return False, "transport %.3f s, popup %.3f s after rebuild (45 s cap + bounded pump/observer/render allowances)" % (terminal - start, popup - start)
        return True, "transport %.3f s, popup %.3f s after rebuild" % (terminal - start, popup - start)


def start_service(node, fake, out, port, control=False):
    log = open(out / "service.log", "w")
    proc = subprocess.Popen([node, str(fake), "--http-port", str(port), "--https-port", str(port + 1)] + (["--allow-control"] if control else []), stdout=log, stderr=subprocess.STDOUT)
    import urllib.request
    base = "http://127.0.0.1:%d" % port
    for _ in range(120):
        time.sleep(0.25)
        try:
            urllib.request.urlopen(base + "/v1/info", timeout=1).read()
            return proc, base
        except Exception:
            continue
    proc.terminate()
    raise SystemExit("the fake service did not start (see %s)" % (out / "service.log"))


def ensure_proxy(a, out):
    """The natsim_proxy executable: --proxy, else compiled into <out>; "" when that is not possible."""
    if a.proxy:
        return a.proxy
    root = HERE.parent.parent
    exe = out / "natsim_proxy"
    # (the same sources as tools/net/build_natsim.sh: the proxy also runs the test TURN server, which needs turnsim.cpp and np_hash.cpp; without them
    # the link failed and pair, cancel and lost were reported as SKIP "no NAT simulator")
    srcs = [root / "tools/net/natsim.cpp", root / "tools/net/turnsim.cpp", root / "tools/net/natsim_proxy.cpp", root / "platform/sdk/net/online/np_net.cpp",
            root / "platform/sdk/net/online/np_hash.cpp"]
    for cxx in ("clang++", "g++"):
        if shutil.which(cxx) and all(p.exists() for p in srcs):
            r = subprocess.run([cxx, "-O1", "-std=gnu++11", "-fno-exceptions", "-fno-rtti", "-DNP_TARGET_HOST32=1", "-DNP_NATIVE=1", "-I" + str(root), "-I" + str(root / "include"),
                                "-I" + str(root / "tools/net"), *map(str, srcs), "-o", str(exe), "-lpthread"], capture_output=True, text=True)
            if r.returncode == 0:
                return str(exe)
            print(f"natsim_proxy did not build with {cxx}: {r.stderr.strip()[-400:]}", flush=True)
    return ""


def start_proxy(proxy, out, port):
    """A natsim_proxy for one console (client 0 behind a port-restricted NAT); returns the process."""
    out.mkdir(parents=True, exist_ok=True)
    log = open(out / "proxy.log", "w")
    p = subprocess.Popen([proxy, "--port", str(port), "--nat0", "port", "--nat1", "port"], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True, bufsize=1)
    for _ in range(100):
        time.sleep(0.05)
        if "ready" in (out / "proxy.log").read_text():
            return p
    p.terminate()
    raise RuntimeError("the NAT simulator did not start")


def run_single(binary, out, script, env, timeout, dump_every, port_base):
    """One console with a nav script; returns (exit code, runtime.log text)."""
    out.mkdir(parents=True, exist_ok=True)
    extra = dict(item.split("=", 1) for item in env)
    if dump_every:
        extra["MSC_DUMP_FRAMES"] = str(dump_every)
        extra["MSC_DUMP_DIR"] = str(out / "peer0" / "pics")
        extra.update(dict(item.split("=", 1) for item in GL_ENV))
    proc, log = native_pair.launch(Path(binary).resolve(), out, 0, Path(script).resolve(), 12000, extra, False, False, port_base, 600)
    deadline = time.time() + timeout
    while time.time() < deadline and proc.poll() is None:
        time.sleep(1)
    code = proc.poll()
    if code is None:
        proc.terminate()
        try:
            code = proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = "timeout"
    return code, Path(log).read_text(errors="replace")


def check(results, name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + ((" - " + detail) if detail and not ok else ""))


def warn_resources():
    """Each game process on the host takes 2-2.5 GB and the pair runs in real time: say so when the machine cannot hold two (an OOM kill of one
    instance looks like a lost partner and fails every later check)."""
    try:
        avail = int([l for l in open("/proc/meminfo") if l.startswith("MemAvailable:")][0].split()[1]) // 1024
        load = os.getloadavg()[0]
    except (OSError, IndexError, ValueError):
        return
    if avail < 6000 or load > 4:
        print("WARN pair: MemAvailable %d MB (two instances need about 6000) and load average %.1f (needs < 4): expect starvation or an OOM kill, not a verdict" % (avail, load))


def case_pair(a, out, base, results):
    if not a.proxy:
        print("SKIP pair: no NAT simulator (set --proxy, or install a C++ compiler)")
        return
    cmd = [sys.executable, str(HERE / "online_pair.py"), "--binary", a.binary, "--out", str(out), "--service", "--menu", "--proxy", a.proxy,
           "--fake-server", a.fake_server, "--node", a.node, "--service-port", str(a.service_port + 10), "--sim-port", str(a.sim_port),
           "--port-base", str(a.port_base), "--timeout", str(a.timeout), "--host-script", str(LAN / "online_menu_host.txt"),
           "--join-script", str(LAN / "online_menu_join.txt"),
           # the online service fails under the live match (its data is wiped: both consoles' next poll is refused): the match must go on and the
           # player must be told (np_notice.cpp); the lockstep checks of native_pair then prove that nothing about the match changed
           "--service-reset-after", r"STATE open play@3",
           # real-time test on a shared machine: the 15 s room limit of a real console is a starvation failure here (two game processes on llvmpipe)
           "--env", "MSC_NET_ROOM_LIMIT_MS=180000",
           # llvmpipe on a shared machine can hold one frame for minutes (the [hitch] lines show the time in the GL back end, with no shader, texture or disc work in it): the game's own
           # 20 s keep-alive (TransportConnection) would end the session over it. A Vita's GPU does not block the game thread like that.
           "--env", "MSC_LAN_KEEPALIVE_MS=600000"]
    warn_resources()
    for e in os.environ.get("OMT_ENV", "").split():   # extra game variables for both instances (diagnostics, e.g. OMT_ENV="MSC_HITCH_MS=1000")
        cmd += ["--env", e]
    if a.dump_every:
        for i in (0, 1):
            cmd += ["--env%d" % i, "MSC_DUMP_FRAMES=%d" % a.dump_every, "--env%d" % i, "MSC_DUMP_DIR=%s" % (out / "pair" / ("peer%d" % i) / "pics")]
        for e in GL_ENV:
            cmd += ["--env", e]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (out.parent / "pair.out").write_text(r.stdout + r.stderr)
    logs = [(out / "pair" / ("peer%d" % i) / "runtime.log") for i in (0, 1)]
    texts = [p.read_text(errors="replace") if p.exists() else "" for p in logs]
    check(results, "pair: lockstep checks pass (native_pair)", r.returncode == 0, "exit %d, see %s" % (r.returncode, out.parent / "pair.out"))
    for i, t in enumerate(texts):
        who = "host" if i == 0 else "joiner"
        check(results, "pair: %s chose ONLINE inside the game" % who, "[online-menu] multiplayer mode choice" in t and "[script]" in t and "chose online" in t)
        check(results, "pair: %s went through the online menu" % who, "[online-menu] online selected" in t and "[online-menu] link up" in t)
        check(results, "pair: %s account made by the game" % who, "[online-menu] account: name" in t)
        check(results, "pair: %s entered the lobby over the online link" % who, "[mp-lan] online link up; entering the lobby" in t and "[mp-lan] LAN mode on" in t)
    check(results, "pair: host chose HOST, joiner chose JOIN", "[online-menu] chose host" in texts[0] and "[online-menu] chose join" in texts[1])
    check(results, "pair: the name was typed with the keyboard helper", "[kbd] typed" in texts[0])
    check(results, "pair: the invite code was typed with the keyboard helper", "[kbd] typed" in texts[1] and "[online-menu] invite code typed" in texts[1])
    check(results, "pair: no failure popup on either console", not any("[online-menu] failure" in t for t in texts))
    for i, t in enumerate(texts):
        who = "host" if i == 0 else "joiner"
        check(results, "pair: %s: the service failing under the match was noticed by the transport" % who, "signaling failed while a link is up" in t)
        check(results, "pair: %s: the player was told on screen (banner), the match went on" % who,
              "[online-notice] banner:" in t and "The match goes on." in t and "DESYNC" not in t)
    for i, t in enumerate(texts):
        who = "host" if i == 0 else "joiner"
        views = [line for line in t.splitlines() if line.startswith("[watch] ") and re.search(r"\bgame=1(?:\s|$)", line)]
        fixed = bool(views) and all(re.search(r"\bteams=5v3\s+stad=13\s+game=1(?:\s|$)", line) for line in views)
        check(results, "pair: %s played the fixed teams and stadium" % who, fixed,
              "require in-match watch records for teams 5v3, stadium 13, with no scene drift")


def case_modes(a, out, base, results):
    code, text = run_single(a.binary, out, LAN / "multiplayer_modes.txt",
                            ["MSC_LAN_MODE=adhoc", "MSC_ONLINE_URL=" + base],
                            a.timeout, a.dump_every, a.port_base + 14)
    check(results, "modes: clean complete navigation", code == 0 and "STATE modes complete" in text and "[script] TIMEOUT" not in text, "exit %s" % code)
    check(results, "modes: ONLINE is discoverable from the local default", "STATE mode chooser opened without online preference" in text and "STATE online host join visible after local play" in text)
    check(results, "modes: cancel and local back work", "STATE cancelled mode choice" in text and "STATE local back returned to mode chooser" in text)
    check(results, "modes: both local links enter and leave the lobby", "[online-menu] chose same wi-fi" in text and "[online-menu] chose nearby" in text and
          "STATE same wifi lobby" in text and "STATE nearby lobby" in text and text.count("[mp-lan] LAN mode on") == 2 and text.count("[mp-lan] left multiplayer") == 2)
    check(results, "modes: cancelled online choice starts no account or link", "[online-menu] online selected" in text and "[online-menu] cancelled at the choice" in text and
          "[online-menu] account:" not in text and "[online-menu] host started" not in text and "[online-menu] join started" not in text and "[online-menu] failure" not in text)


def case_cancel(a, out, base, results):
    if not a.proxy:
        print("SKIP cancel: no NAT simulator (set --proxy, or install a C++ compiler)")
        return
    proxy = start_proxy(a.proxy, out, a.sim_port + 3)
    try:
        code, text = run_single(a.binary, out, LAN / "online_menu_cancel.txt",
                                ["MSC_LAN_MODE=online", "MSC_ONLINE_URL=" + base, "MSC_KEYBOARD_NAME=Cancel1", "MSC_NET_SIM=127.0.0.1:%d" % (a.sim_port + 3), "MSC_NET_SIM_ID=0",
                                 "MSC_NET_SIM_PRIV=192.168.1.10", "MSC_NET_STUN=198.51.100.1:3478,198.51.100.2:3478"],
                                a.timeout, a.dump_every, a.port_base + 10)
    finally:
        proxy.terminate()
    check(results, "cancel: the script ran to its end", code == 0 and "STATE cancelled twice" in text, "exit %s" % code)
    check(results, "cancel: Circle while waiting for the partner ended the flow", "[online-menu] cancelled while connecting (stage 3)" in text)
    check(results, "cancel: Circle on the HOST / JOIN popup ended the flow", "[online-menu] cancelled at the choice" in text)
    check(results, "cancel: back in the main menu, no link left", text.count("online entry ended without a link") == 2 and "LAN mode on" not in text)
    check(results, "cancel: the account was made once and kept", "STATE cancelled twice" in text and text.count("[online-menu] account: name") == 1)


def case_wrong(a, out, base, results):
    code, text = run_single(a.binary, out, LAN / "online_menu_wrong.txt",
                            ["MSC_LAN_MODE=online", "MSC_ONLINE_URL=" + base, "MSC_ONLINE_NAME=Wrong1", "MSC_KEYBOARD_CODE=ZZZZZZ", "MSC_KEYBOARD_ONCE=1", "MSC_NET_LOCAL_IP=127.0.0.1",
                             "MSC_NET_STUN=127.0.0.1:9", "MSC_NET_TRY_LOCAL=1", "MSC_NET_PORT=%d" % (a.sim_port + 4)],
                            a.timeout, a.dump_every, a.port_base + 12)
    check(results, "wrong: the script ran to its end", code == 0 and "STATE back in the main menu after the wrong code" in text, "exit %s" % code)
    check(results, "wrong: the player is told in one sentence", re.search(r"\[online-menu\] failure: .*(no room|not found|code)", text, re.I) is not None,
          "; ".join(re.findall(r"\[online-menu\] failure: (.*)", text)))
    check(results, "wrong: no lobby, no LAN mode", "LAN mode on" not in text)
    # (1.3 mpux) a code that failed is asked for again with what the player typed in the field: the text is never lost
    opens = re.findall(r'\[kbd\] open: .*\(code\) initial="([^"]*)"', text)
    check(results, "wrong: the code keyboard opened again, with the typed code in it", len(opens) >= 2 and opens[0] == "" and opens[1] == "ZZZZZZ", "keyboard openings: %r" % opens)


def case_badcode(a, out, base, results):
    """A text that cannot be an invite code: one plain sentence that says which characters a code never has, then the keyboard again with the text in it."""
    code, text = run_single(a.binary, out, LAN / "online_menu_wrong.txt",
                            ["MSC_LAN_MODE=online", "MSC_ONLINE_URL=" + base, "MSC_ONLINE_NAME=Bad1", "MSC_KEYBOARD_CODE=AB0LO1", "MSC_KEYBOARD_ONCE=1"],
                            a.timeout, a.dump_every, a.port_base + 16)
    check(results, "badcode: the script ran to its end", code == 0 and "STATE back in the main menu after the wrong code" in text, "exit %s" % code)
    sentence = "; ".join(re.findall(r"\[online-menu\] failure: (.*)", text))
    check(results, "badcode: the sentence says what a code looks like", "6 letters and digits" in sentence and "never has the letters O, I or L" in sentence, sentence)
    opens = re.findall(r'\[kbd\] open: .*\(code\) initial="([^"]*)"', text)
    check(results, "badcode: the keyboard opened again with the typed text in it", len(opens) >= 2 and opens[1] == "AB0LO1", "keyboard openings: %r" % opens)
    check(results, "badcode: nothing was sent to the service", "[online-menu] join started" not in text)


def case_conntest(a, out, base_unused, results):
    """Hosting fails because the service is down; the failure popup offers TEST CONNECTION; the test makes a room and leaves it, says one sentence, OK returns
    to the main menu. Needs the NAT simulator (the room's endpoint must be public)."""
    if not a.proxy:
        print("SKIP conntest: no NAT simulator (set --proxy, or install a C++ compiler)")
        return
    out.mkdir(parents=True, exist_ok=True)
    svc, base = start_service(a.node, a.fake_server, out, a.service_port + 20, control=True)
    proxy = start_proxy(a.proxy, out, a.sim_port + 5)
    try:
        service_json(base, "POST", "/__fake/fault", body={"failRate": 1})
        extra = {"MSC_ONLINE_URL": base, "MSC_ONLINE_NAME": "Test1", "MSC_NET_SIM": "127.0.0.1:%d" % (a.sim_port + 5), "MSC_NET_SIM_ID": "0",
                 "MSC_NET_SIM_PRIV": "192.168.1.10", "MSC_NET_STUN": "198.51.100.1:3478,198.51.100.2:3478", "MSC_LAN_MODE": "online"}
        if a.dump_every:
            extra["MSC_DUMP_FRAMES"] = str(a.dump_every)
            extra["MSC_DUMP_DIR"] = str(out / "peer0" / "pics")
            extra.update(dict(item.split("=", 1) for item in GL_ENV))
        proc, log = native_pair.launch(Path(a.binary).resolve(), out, 0, (LAN / "online_menu_conntest.txt").resolve(), 12000, extra, False, False, a.port_base + 18, 600)
        cleared = False
        deadline = time.time() + a.timeout
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
            if not cleared and "[online-menu] failure:" in Path(log).read_text(errors="replace"):
                service_json(base, "POST", "/__fake/fault", body={})
                cleared = True
        code = proc.poll()
        if code is None:
            proc.terminate()
            try: code = proc.wait(10)
            except subprocess.TimeoutExpired: proc.kill(); code = "timeout"
        text = Path(log).read_text(errors="replace")
    finally:
        proxy.terminate()
        svc.terminate()
    check(results, "conntest: the script ran to its end", code == 0 and "STATE back in the main menu after the test" in text, "exit %s" % code)
    check(results, "conntest: the failure popup was shown first", "STATE failure popup with the test offered" in text and "[online-menu] failure:" in text)
    check(results, "conntest: TEST CONNECTION was chosen from the popup", "[online-menu] connection test chosen" in text and "[online-menu] connection test started" in text)
    check(results, "conntest: the room was made and left at once (no partner needed)", "[online-menu] connection test: nat " in text and "[np-net] online session stopped" in text)
    sentences = re.findall(r"\[online-menu\] failure: (.*)", text)
    check(results, "conntest: one plain sentence about the connection", len(sentences) >= 2 and sentences[-1].startswith("Your connection is good"), "; ".join(sentences))


def lost_pair(a, out, host_script, join_script, restore, port_shift):
    """Two consoles in the lobby; the simulator cuts the route 2 s after the partner joined; with `restore` it brings the route back 1 s after the transport
    declared the link lost. The lobby-only gate requires clean exits, one loss and explicit script endings, not match CRCs.
    The transport's and the game's silence limits are 6 s here instead of 25 / 20; reconnect policy remains unchanged."""
    wait_flags = require_nonreaping_wait()
    endings = ["STATE peer joined again", "STATE joined again"] if restore else ["STATE back in the main menu after the failed reconnect"] * 2
    cmd = [sys.executable, str(HERE / "online_pair.py"), "--binary", a.binary, "--out", str(out), "--service", "--menu", "--proxy", a.proxy,
           "--fake-server", a.fake_server, "--node", a.node, "--service-port", str(a.service_port + port_shift), "--sim-port", str(a.sim_port + port_shift),
           "--port-base", str(a.port_base + port_shift), "--timeout", str(a.timeout), "--host-script", str(LAN / host_script), "--join-script", str(LAN / join_script),
           "--env", "MSC_NET_PEER_LOST_MS=6000", "--env", "MSC_LAN_KEEPALIVE_MS=6000", "--env", "MSC_FPS=30",
           "--lobby-only", "--expect-lost", "1", "--end-marker0", endings[0], "--end-marker1", endings[1],
           "--cmd-after", "STATE peer joined@2@down 0"]
    if restore:
        cmd += ["--cmd-after", r"link lost \(@1@up 0"]
    if a.dump_every:
        for i in (0, 1):
            cmd += ["--env%d" % i, "MSC_DUMP_FRAMES=%d" % a.dump_every, "--env%d" % i, "MSC_DUMP_DIR=%s" % (out / "pair" / ("peer%d" % i) / "pics")]
        for e in GL_ENV:
            cmd += ["--env", e]
    out.parent.mkdir(parents=True, exist_ok=True)
    logs = [out / "pair" / ("peer%d" % i) / "runtime.log" for i in (0, 1)]
    observer = ReconnectObserver(logs)
    with (out.parent / (out.name + ".out")).open("w") as output:
        proc = subprocess.Popen(cmd, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + a.timeout + 60  # bounded service startup and helper cleanup, separate from reconnect timing
        owns_leader = True
        try:
            while True:
                observer.poll()
                owns_leader = False
                exited = os.waitid(os.P_PID, proc.pid, wait_flags)
                owns_leader = True
                if exited is not None:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("online pair helper exceeded its watchdog")
                time.sleep(LOG_POLL_SECONDS)
        except BaseException as original:
            # An unreaped leader pins this newly-owned session ID against PID reuse.
            try:
                if owns_leader and proc.returncode is None:
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    stop_deadline = time.monotonic() + 5
                    while True:
                        try:
                            os.killpg(proc.pid, 0)
                        except ProcessLookupError:
                            break
                        remaining = stop_deadline - time.monotonic()
                        if remaining <= 0:
                            try:
                                os.killpg(proc.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                            break
                        time.sleep(min(LOG_POLL_SECONDS, remaining))
                elif not owns_leader and proc.returncode is None:
                    cleanup_note(original, "group not signaled because helper waitability is uncertain")
            except BaseException as error:
                cleanup_note(original, "group cleanup raised %s: %s" % (type(error).__name__, error))
            try:
                proc.wait(5)
            except BaseException as error:
                cleanup_note(original, "bounded helper reap raised %s: %s" % (type(error).__name__, error))
            raise
        code = proc.wait()
        observer.poll()  # Final normal-exit observation must not signal an already-reaped group on error.
    texts = [path.read_text(errors="replace") if path.exists() else "" for path in logs]
    try:
        codes = json.loads((out / "pair" / "codes.json").read_text())
        report = native_pair.analyse(logs, codes, "1", lobby_only=True, end_markers=endings)
    except (OSError, ValueError, TypeError) as error:
        report = {"passed": False, "error": str(error)}
    (out / "reconnect-observation.json").write_text(json.dumps(observer.peers, indent=2) + "\n")
    return texts, code == 0 and report["passed"], observer


def script_frame(text, label):
    m = re.search(r"\[script\] f=(\d+) %s" % re.escape(label), text)
    return int(m.group(1)) if m else None


def case_lost(a, out, base, results):
    if not a.proxy:
        print("SKIP lost: no NAT simulator (set --proxy, or install a C++ compiler)")
        return
    # 1. the route comes back: RECONNECT restores the lobby, and the lobby works again (a new game is created and joined over the rebuilt link)
    texts, clean, _ = lost_pair(a, out / "restore", "online_menu_host_lost.txt", "online_menu_join_lost.txt", True, 30)
    for i, t in enumerate(texts):
        who = "host" if i == 0 else "joiner"
        check(results, "lost: %s: the link was declared lost and the game ended its session" % who, "[mp-lan] link lost" in t and "[mp-lan] connection lost" in t)
        check(results, "lost: %s: the lobby offered RECONNECT and the player chose it" % who, "[online-menu] reconnecting" in t and "chose reconnect" in t)
        check(results, "lost: %s: RECONNECT brought the link back" % who, "[online-menu] reconnected" in t and "STATE lobby restored" in t, "see %s" % (out / "restore"))
        check(results, "lost: %s: no failure popup, the script ran to its end" % who, clean and "[online-menu] lobby: reconnect failed" not in t and "[script] TIMEOUT" not in t,
              "require helper/peer exits 0, one loss, terminal script markers and no error flags")
    check(results, "lost: the restored lobby works (a new game was created and joined)", "STATE peer joined again" in texts[0] and "STATE joined again" in texts[1])
    # 2. the route stays down: RECONNECT ends with one plain sentence inside its window and the main menu follows
    texts, clean, observer = lost_pair(a, out / "stay_down", "online_menu_host_lostfail.txt", "online_menu_join_lostfail.txt", False, 40)
    for i, t in enumerate(texts):
        who = "host" if i == 0 else "joiner"
        sentences = re.findall(r"^\[online-menu\] lobby: reconnect failed: (.+)$", t, re.M)
        sentence = sentences[0] if len(sentences) == 1 else ""
        check(results, "lost: %s: with no route RECONNECT fails with a sentence" % who, len(sentence) > 10 and sentence.strip().endswith("."),
              sentence or "need exactly one failure sentence")
        fail, mark = t.find("[online-menu] lobby: reconnect failed:"), t.find("STATE reconnect failed popup")
        timed, detail = observer.failure_within_window(i)
        check(results, "lost: %s: the failure came inside the reconnect window (45 s transport cap)" % who,
              timed and fail >= 0 and mark > fail, detail)
        ack, stopped = t.find("STATE reconnect failure acknowledged"), t.find("[np-net] online session stopped", max(0, fail))
        main = t.find("STATE back in the main menu after the failed reconnect")
        check(results, "lost: %s: back in the main menu, no link left" % who, clean and fail >= 0 and mark > fail and ack > mark and stopped > fail and main > ack and main > stopped,
              "require actual popup, acknowledgement, stopped transport, main menu and clean complete peers")


def service_json(base, method, path, token=None, body=None):
    import urllib.request
    import urllib.error
    req = urllib.request.Request(base + path, method=method, data=None if body is None else json.dumps(body).encode())
    req.add_header("X-Msc-App", "1.3.0")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def case_ranked(a, out, base, results):
    """1.3: two consoles go MULTIPLAYER -> ONLINE -> RANKED MATCH -> Online Central -> RANKED MATCH; the service's queue pairs them, they play the
    game's ranked match over the online link (DWC transport on np_online), both report the result, the service verifies it, the score puts follow and
    the leaderboards (the game's rank/get and the public web board) show both players."""
    if not a.proxy:
        print("SKIP ranked: no NAT simulator (set --proxy, or install a C++ compiler)")
        return
    out.mkdir(parents=True, exist_ok=True)
    port = a.sim_port + 6
    proxy = start_proxy(a.proxy, out, port)
    names = ("RankA1", "RankB2")
    procs, logs = [], []
    try:
        for i in (0, 1):
            extra = {"MSC_ONLINE_URL": base, "MSC_ONLINE_NAME": names[i], "MSC_NET_SIM": "127.0.0.1:%d" % port, "MSC_NET_SIM_ID": str(i),
                     "MSC_NET_SIM_PRIV": "192.168.%d.10" % (i + 1), "MSC_NET_STUN": "198.51.100.1:3478,198.51.100.2:3478",
                     "MSC_LAN_TEST_SCORE": "3000:%d" % 0, "MSC_ONLINE_MII_ICON": "0"}
            if a.dump_every:
                extra["MSC_DUMP_FRAMES"] = str(a.dump_every)
                extra["MSC_DUMP_DIR"] = str(out / ("peer%d" % i) / "pics")
                extra.update(dict(item.split("=", 1) for item in GL_ENV))
            proc, log = native_pair.launch(Path(a.binary).resolve(), out, i, (LAN / "online_menu_ranked.txt").resolve(), 40000, extra, False, False,
                                           a.port_base + 20 + 4 * i, 600)
            procs.append(proc)
            logs.append(Path(log))
            time.sleep(1)
        deadline = time.time() + a.timeout
        while time.time() < deadline and any(p.poll() is None for p in procs):
            time.sleep(1)
        codes = []
        for p in procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
                codes.append("timeout")
            else:
                codes.append(p.returncode)
    finally:
        proxy.terminate()
    texts = [l.read_text(errors="replace") for l in logs]
    for i, (code, text) in enumerate(zip(codes, texts)):
        who = "console %d" % i
        check(results, "ranked: %s ran its script to the end" % who, code == 0 and "STATE after the match" in text and "[script] TIMEOUT" not in text,
              "exit %s, last state: %s" % (code, (re.findall(r"STATE ([^\n]*)", text) or ["none"])[-1]))
        check(results, "ranked: %s chose RANKED MATCH in the online menu and reached Online Central" % who,
              "[online-menu] chose ranked: Online Central" in text and "STATE online hub" in text)
        check(results, "ranked: %s was paired by the service's queue and linked" % who,
              re.search(r"\[ranked\] paired: room \w+, match \S+, slot [01]", text) is not None and "[ranked] connected: I am AID" in text)
        check(results, "ranked: %s played the match to its end" % who, "STATE match over" in text and "[ranked] match over at step" in text)
        check(results, "ranked: %s queued its result report" % who, re.search(r"\[ranked\] result 0 \d+-\d+ \(slot [01]\) for match \S+ queued", text) is not None)
        # one game per ranked match: the game's series would start a second game on the link of a match the service already closed
        check(results, "ranked: %s went back to Online Central after the one game" % who,
              "STATE back in Online Central" in text and text.count("simulation started") == 1, "games started: %d" % text.count("simulation started"))
    digests = [re.findall(r"\[ranked\] match over at step (\d+): digest (\w+)/(\w+)", t) for t in texts]
    check(results, "ranked: both consoles ended on the same step with the same digest", all(digests) and digests[0][-1] == digests[1][-1], str(digests))
    match_ids = [re.findall(r"\[ranked\] paired: room \w+, match (\S+), slot", t) for t in texts]
    mid = match_ids[0][-1] if match_ids[0] else ""
    check(results, "ranked: the same match on both consoles", mid and match_ids[1] and match_ids[1][-1] == mid, str(match_ids))
    # the service's view: the match was verified, the score puts went through, both players are on the boards
    code, board = service_json(base, "GET", "/v1/public/leaderboard?board=alltime&limit=10")
    rows = {r["name"]: r for r in (board or {}).get("rows", [])} if code == 200 else {}
    check(results, "ranked: the service verified the match (both players on the all-time board, last played set)",
          all(n in rows and rows[n]["lastPlayedMs"] for n in names), json.dumps(board)[:400])
    code, season = service_json(base, "GET", "/v1/public/leaderboard?board=season&limit=10")
    srows = {r["name"]: r for r in (season or {}).get("rows", [])} if code == 200 else {}
    check(results, "ranked: the game's score puts reached the season board (both rows)", all(n in srows for n in names), json.dumps(season)[:400])
    check(results, "ranked: the winner has more season points than the loser",
          len(srows) >= 2 and len({srows[n]["points"] for n in names if n in srows}) == 2, json.dumps(season)[:400])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--fake-server", required=True)
    ap.add_argument("--node", default=os.environ.get("NODE", "node"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--cases", default="modes,pair,cancel,wrong,badcode,conntest,lost")
    ap.add_argument("--dump-every", type=int, default=0)
    ap.add_argument("--proxy", default=os.environ.get("PROXY", ""))
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--service-port", type=int, default=47410)
    ap.add_argument("--sim-port", type=int, default=47420)
    ap.add_argument("--port-base", type=int, default=41400)
    a = ap.parse_args()
    out = Path(a.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    results = []
    if any(c in a.cases.split(",") for c in ("pair", "cancel", "lost", "ranked", "conntest")):
        a.proxy = ensure_proxy(a, out)
    service, base = start_service(a.node, a.fake_server, out, a.service_port)
    import atexit
    atexit.register(lambda: service.poll() is None and service.kill())   # never leave the fake service running
    try:
        for case in a.cases.split(","):
            fn = globals().get("case_" + case)
            if not fn:
                raise SystemExit("unknown case " + case)
            print("== " + case)
            try:
                fn(a, out / case, base, results)
            except Exception as e:   # a case that could not even run is a failed check, not a traceback the gate cannot read
                check(results, "%s: the case ran" % case, False, "%s: %s" % (type(e).__name__, e))
    finally:
        service.terminate()
    failed = [r for r in results if not r[1]]
    # the gate shows only the tail of the output: every failed check is repeated here, right above the summary line
    for name, _, detail in failed:
        print("FAIL " + name + ((" - " + detail) if detail else ""))
    print("online menu test: %d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
