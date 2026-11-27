#!/usr/bin/env python3
"""Stand-in for curl and nc in the dry run of the Mac-VM queue: a fake console directory under $FAKE, a virtual clock ($FAKE/clock, advanced by the
stub sleep and by every stub call), and a record of every operation. It opens no socket and runs no other program. Anything the real tools must
never do (another host, another title, a path outside the four allowed ones) is written to $FAKE/violations.txt and refused."""
import fcntl, hashlib, json, os, re, sys
from pathlib import Path

FAKE = Path(os.environ["FAKE"]); CON = FAKE / "console"; CLOCK = FAKE / "clock"; ST = FAKE / "state.json"
IP = os.environ.get("FAKE_IP", "fake-vita.invalid"); PERF = "ux0:/data/mscharged-perf-test/"
DUR = {"fb-tour": 150, "fb-dump": 262, "fb-crep": 190, "mega": 148}
LOGSRC = os.environ.get("FAKE_LOG")   # a real log to replay (e.g. an emulator log), else a synthetic one


def synth():
    L = ["[script] f=178 @begin st-walkout", "[script] f=306 @end st-walkout", "[script] f=306 STATE kick-off", "[script] f=317 @begin st-play"]
    for k in range(400): L.append("[lane] %d pics 5.0s 29.%d/s game 30.1 send 1.0 fe 2.0 gpu 25.%d" % (k, k % 10, k % 7))
    L += ["[script] f=2117 @end st-play", "[script] f=4586 exit", "[plat] script exit at frame 4586, exiting"]
    L += ["[env] 300 pictures: padding line after the exit, as the real log has about 5 KB there %04d" % k for k in range(60)]
    return ("\n".join(L) + "\n").encode()


def now(): return int(CLOCK.read_text())
def put(path, text):   # whole-file replace: a reader (the stub date, another stub) never sees a half-written or empty file
    t = path.with_name(path.name + ".tmp%d" % os.getpid()); t.write_text(text); os.replace(t, path)
def adv(n): put(CLOCK, str(now() + int(n)))
def sha(b): return hashlib.sha256(b).hexdigest()
def load(): return json.loads(ST.read_text()) if ST.exists() else {"running": False, "ebup": 0}
def save(s): put(ST, json.dumps(s))
def note(f, msg): open(FAKE / f, "a").write("%d %s\n" % (now(), msg))
def viol(msg): note("violations.txt", msg)


def tick(s):
    """the running app's log grows with the clock: a prefix of a real emulator log, whole (with its exit line) at the end of the route"""
    if not s["running"]: return
    full = Path(LOGSRC).read_bytes() if LOGSRC else synth(); el = now() - s["start"]
    n = len(full) if el >= s["dur"] else int(len(full) * 0.95 * el / s["dur"])
    (CON / "ux0/data/mscharged-perf-test/log.txt").write_bytes(full[:n])
    if el >= s["dur"]: s["running"] = False; note("ops.log", "route %s ended" % s["route"])


def path_of(url):
    m = re.match(r"ftp://([^:/]+):(\d+)/(.*)$", url)
    if not m: viol("not an ftp url: " + url); return None, None
    if m.group(1) != IP or m.group(2) != "1337": viol("wrong host/port: " + url); return None, None
    return m.group(3).lstrip("/"), True


def allowed(p, write):
    if p == "ux0:/data/mscharged-perf-test/np-player.txt": return True
    if p == "ux0:/app/CDXMP0001/eboot.bin": return True
    m = re.fullmatch(r"ux0:/data/mscharged-perf-test/([A-Za-z0-9_-]+\.txt)", p)
    if m: return (not write) or m.group(1) not in ("log.txt",) and not m.group(1).startswith("np-")
    return False


def real(p): return CON / p.replace("ux0:/", "ux0/", 1)


def curl(a):
    adv(1); s = load(); tick(s); save(s)
    mt = T = o = Q = r = url = None; i = 0
    while i < len(a):
        x = a[i]
        if x in ("--max-time",): i += 1
        elif x == "-T": T = a[i + 1]; i += 1
        elif x == "-o": o = a[i + 1]; i += 1
        elif x == "-Q": Q = a[i + 1]; i += 1
        elif x == "-r": r = a[i + 1]; i += 1
        elif x.startswith("ftp://"): url = x
        i += 1
    p, ok = path_of(url or "")
    if not ok: return 7
    if Q:
        m = re.fullmatch(r"DELE /(ux0:/data/mscharged-perf-test/log\.txt)", Q)
        if not m: viol("refused FTP command: " + Q); return 21
        f = real(m.group(1))
        if f.exists(): f.unlink()
        note("ops.log", "DELE log.txt"); return 0
    if T:
        if not allowed(p, True): viol("write outside the allowed paths: " + p); return 23
        data = Path(T).read_bytes()
        if p.endswith("eboot.bin"):
            s = load(); s["ebup"] += 1
            if str(s["ebup"]) in os.environ.get("FAKE_CORRUPT_ESWAP", "").split(","): data = data[:-1] + bytes([data[-1] ^ 1])
            save(s); adv(len(data) // 600000)
        real(p).parent.mkdir(parents=True, exist_ok=True); real(p).write_bytes(data)
        note("ops.log", "PUT %s %s" % (p, sha(data)[:12])); return 0
    if p.rstrip("/") == PERF.rstrip("/"):
        if o: return 0
        for f in sorted((CON / "ux0/data/mscharged-perf-test").glob("*")):
            sys.stdout.write("-rw-r--r-- 1 user group %d Nov 14 00:00 %s\r\n" % (f.stat().st_size, f.name))
        return 0
    if not allowed(p, False): viol("read outside the allowed paths: " + p); return 23
    f = real(p)
    if not f.exists(): return 78
    d = f.read_bytes()
    if r: d = d[int(r.rstrip("-")):]
    if o: Path(o).write_bytes(d)
    else: sys.stdout.buffer.write(d)
    return 0


def nc(a):
    adv(1); cmd = STDIN.strip(); s = load(); tick(s)
    if not any(x == "1338" for x in a) or IP not in a: viol("nc to wrong host/port: %s" % a); return 1
    if cmd == "quit CDXMP0001":
        s["running"] = False; note("ops.log", "quit"); print("OK")
    elif cmd == "launch CDXMP0001":
        npf = CON / "ux0/data/mscharged-perf-test/np-player.txt"
        m = re.search(r"^MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/([^\s]+)\.txt", npf.read_text(), re.M)
        route = m.group(1); rf = CON / "ux0/data/mscharged-perf-test" / (route + ".txt")
        eb = CON / "ux0/app/CDXMP0001/eboot.bin"
        if not rf.exists(): viol("launch: route %s.txt is NOT on the console" % route)
        s.update(running=True, start=now(), dur=DUR.get(route, 170), route=route)
        (CON / "ux0/data/mscharged-perf-test/log.txt").write_bytes(b"")
        note("launches.log", "route=%s np=%s eboot=%s routefile=%s" % (route, sha(npf.read_bytes())[:12], sha(eb.read_bytes())[:12], rf.exists()))
        print("OK")
    else:
        viol("refused command: " + cmd); print("ERR")
    save(s); return 0


if __name__ == "__main__":
    tool, args = sys.argv[1], sys.argv[2:]
    # One call at a time: in `(printf ..; sleep 2) | nc ..` the stub sleep and the stub nc run at the same moment, and an unlocked read-modify-write of the
    # clock / state lost updates (a clock reset to ~0 gave "virtual minutes: -16545", a launch that was never recorded gave "launches 61 == runs 62"). The stubs only; q.sh and slice.sh were not at fault.
    STDIN = sys.stdin.read() if tool == "nc" else ""   # (read BEFORE the lock: the writer of this pipe is `(printf; sleep 2)`, whose stub sleep needs the lock)
    lk = open(FAKE / "lock", "a"); fcntl.flock(lk, fcntl.LOCK_EX)
    if tool == "sleep": adv(int(float(args[0]))); sys.exit(0)
    if tool == "curl": sys.exit(curl(args))
    sys.exit(nc(args))
