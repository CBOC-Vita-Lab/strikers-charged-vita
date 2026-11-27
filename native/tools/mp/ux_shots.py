#!/usr/bin/env python3
"""Screen pictures of the multiplayer / online menus on the host (GL build, 960x544): one console runs a nav script with `shot <name>` lines against
the local fake of the online service. Each run is one situation (the environment decides what fails).
  python3 tools/mp/ux_shots.py --binary <np-host32-full> --fake-server ../server/fake/server.mjs --out <dir> --script lan/ux_modes.txt [--env K=V ...] [--no-service]
Pictures land in <out>/pics/<name>.png; the console's log is <out>/peer0/runtime.log."""
import argparse, os, subprocess, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import native_pair  # noqa: E402
import online_menu_test as omt  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--binary", required=True)
ap.add_argument("--fake-server", default=str(HERE.parent.parent.parent / "server/fake/server.mjs"))
ap.add_argument("--node", default="node")
ap.add_argument("--out", required=True)
ap.add_argument("--script", required=True)
ap.add_argument("--env", action="append", default=[])
ap.add_argument("--no-service", action="store_true", help="do not start the fake service (MSC_ONLINE_URL then comes from --env or the default)")
ap.add_argument("--service-port", type=int, default=18787)
ap.add_argument("--port-base", type=int, default=24100)
ap.add_argument("--timeout", type=int, default=900)
ap.add_argument("--proxy", default="")
ap.add_argument("--legacy", action="store_true", help="a binary without the `shot` script command: dump every 10th frame and pick the frames the script marked with `log SHOT <name>`")
ap.add_argument("--pic-lock", action="store_true", help="wait on /tmp/s13-pics-run.lock (one picture run at a time on the shared host)")
a = ap.parse_args()
out = Path(a.out).resolve(); out.mkdir(parents=True, exist_ok=True)
svc = None; env = {}
if not a.no_service:
    svc, base = omt.start_service(a.node, a.fake_server, out, a.service_port)
    env["MSC_ONLINE_URL"] = base
env.update(dict(item.split("=", 1) for item in omt.GL_ENV))
env.update({"MSC_RENDER_SIZE": "960x544", "MSC_DYNRES": "off", "MSC_SHOT_DIR": str(out / "pics")})
env.update(dict(item.split("=", 1) for item in a.env))
proxy = None
if a.proxy:
    proxy = omt.start_proxy(a.proxy, out, a.service_port + 3)
    env.update({"MSC_NET_SIM": "127.0.0.1:%d" % (a.service_port + 3), "MSC_NET_SIM_ID": "0", "MSC_NET_SIM_PRIV": "192.168.1.10",
                "MSC_NET_STUN": "198.51.100.1:3478,198.51.100.2:3478"})
script = Path(a.script).resolve()
if a.legacy:
    lines = [("log SHOT " + l[5:]) if l.startswith("shot ") else l for l in script.read_text().splitlines(True)]
    script = out / "script_legacy.txt"; script.write_text("".join(lines))
    env.update({"MSC_DUMP_FRAMES": "10", "MSC_DUMP_DIR": str(out / "frames")})
    env.pop("MSC_SHOT_DIR", None)
if a.pic_lock:
    import fcntl
    lockf = open("/tmp/s13-pics-run.lock", "w"); fcntl.flock(lockf, fcntl.LOCK_EX)
try:
    proc, log = native_pair.launch(Path(a.binary).resolve(), out, 0, script, 30000, env, False, False, a.port_base, 600)
    t0 = time.time()
    while time.time() - t0 < a.timeout and proc.poll() is None:
        time.sleep(1)
    code = proc.poll()
    if code is None:
        proc.terminate()
        try: code = proc.wait(10)
        except subprocess.TimeoutExpired: proc.kill(); code = "timeout"
finally:
    if svc: svc.terminate()
    if proxy: proxy.terminate()
if a.legacy:
    import re, shutil
    (out / "pics").mkdir(exist_ok=True)
    for m in re.finditer(r"\[script\] f=(\d+) SHOT (\S+)", Path(log).read_text(errors="replace")):
        f = -(-int(m.group(1)) // 10) * 10
        src = out / "frames" / ("frame_%06d.png" % f)
        if src.exists(): shutil.copy(src, out / "pics" / (m.group(2) + ".png"))
    shutil.rmtree(out / "frames", ignore_errors=True)
print("exit", code, "pictures:", sorted(p.name for p in (out / "pics").glob("*.png")) if (out / "pics").exists() else [])
