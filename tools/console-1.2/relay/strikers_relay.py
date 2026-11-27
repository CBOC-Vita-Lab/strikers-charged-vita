#!/usr/bin/env python3
"""Strikers console relay: lets the coordinating session run the Vita test title (CDXMP0001) in short adaptive rounds.

Run it on the Mac (double-click "Start Strikers Relay.command") and leave the window open (Ctrl+C stops it):
    python3 strikers_relay.py
It watches relay/inbox next to this script. A request file (JSON) there is carried out over VitaCompanion (FTP 1337, commands 1338)
and the result goes to relay/outbox/<id>/ (result.json), the request to relay/done/<id>.json; every action is in relay/relay.log.
The Vita's address: the VITA environment variable, or the first line of vita-address.txt next to this script (git-ignored, never committed).
When this file changes the relay restarts itself.

Actions (ONLY the test title CDXMP0001, its data root ux0:data/mscharged-perf-test and ux0:app/CDXMP0001):
  status                      version, listing of the data root, state of the relay
  ls    {"path"}              list the data root (or a folder in it), the data root's shots/, or ux0:app/CDXMP0001
  get   {"path"}              copy one file from the data root or ux0:app/CDXMP0001 (crash dumps ux0:data/psp2core-*.psp2dmp too)
  put   {"src","path"}        relay/staging/<src> to ux0:data/mscharged-perf-test/<name>.txt (env, route, np-player.txt: the owner's np-player.txt is
                              kept first, once) or to ux0:app/CDXMP0001/eboot.bin (backed up once, read back and compared; a failed backup read
                              stops before anything is written)
  launch / close              start / quit CDXMP0001
  run   {...}                 session.sh's run unit: route + env in (read back by hash), old log out, launch, wait for the end of the route or a
                              time cap, close, lean log pull, the time of every step
  restore                     the owner's np-player.txt back (hash checked), or the session's one removed when there was none
  pull_logs {"all","close"}   lean: log.txt, stderr*, env.txt, settings*, crash*, np-player.txt (not env.txt.* / log.txt.prev unless "all": true);
                              the game is closed first unless "close": false
  pull_cache {"close"}        read-only copy of the console's shader cache: shadercache/vgx_shaderlist.bin (+ .stamp) and every shadercache/<key>/v3/{v,f}/XX/<hash>.gxp
                              the game compiled, into the job's outbox shadercache/ (same layout: tools/seed/assemble_seed.sh takes it as a profile); the game is
                              closed first unless "close": false; a file already in that job's outbox with the same size is skipped
  shot  {"name"}              a screenshot: NOT FOR JUDGING PICTURES (torn through the relay; the owner looks at the screen)
  vc    {"cmd"}               press / release / wait / screen / unlock / nosleep / capture only
  batch {"steps": [...]}      several in order; {"sleep": s}; a step with "always": true runs even after an earlier step failed
  eswap {"name","src"}      session.sh eswap: that executable alone (staged), read back; skipped when the Vita has it already (\"force\": true)
  sleep {"s"}
  A request file named *.json.hold is parked: it is not taken until renamed to *.json.
Never (default mode): destroy / quit all / reboot, launch of another title, any other path, a package install, a delete other than the files session.sh itself replaces.

FULL ACCESS (STRIKERS_RELAY_FULL=1 or a file full-access.on in the relay root; the owner's choice of 23 Nov 2026): every path on every device (ux0:, ur0:, ...) for ls / get / put
(up to 400 MB), plus rm / mkdir / rmdir / mv; every command-port line (cmd {"line"}) and every vc verb. Each file that a write, rm or mv
would overwrite or remove outside the default limits is copied first to <root>/backups/<date-time>/<path> on the Mac (a failed read of an
existing file stops before anything changes). The relay reloads after any job when this file changes.
"""
import ftplib, hashlib, io, json, os, re, shutil, socket, subprocess, sys, time, traceback
from pathlib import Path

TITLE = "CDXMP0001"
DATA = "ux0:data/mscharged-perf-test"
APP = f"ux0:app/{TITLE}"
SCRIPT = Path(__file__).resolve()
# one folder per sitting (kit20, fable-da21 F4): the kept owner np-player.txt, the done jobs and the executable name of an older sitting are never reused.
# STRIKERS_SITTING names the sitting (default r17); STRIKERS_RELAY_ROOT still wins when set.
SITTING = re.sub(r"[^A-Za-z0-9._-]", "", os.environ.get("STRIKERS_SITTING") or "r17") or "r17"
ROOT = Path(os.environ.get("STRIKERS_RELAY_ROOT") or SCRIPT.parent / ("relay-" + SITTING))
INBOX, OUTBOX, STAGING, DONE, STATE = (ROOT / n for n in ("inbox", "outbox", "staging", "done", "state"))
LOG = ROOT / "relay.log"
FTP_PORT = int(os.environ.get("FTP_PORT", 1337))
CMD_PORT = int(os.environ.get("CMD_PORT", 1338))
RETRY_S = 3.0          # pause between tries of a transfer (tests set 0)
TRIES = 3
DONE_RE = re.compile(r"script exit|settings screen done|\[settings\] test script finished")
FATAL_RE = re.compile(r"\[fatal \+\d|\[plat\] EXIT STUCK")   # a run that aborted: the wait ends at once (status "crashed"), not after the stall time
ROUTE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.txt$")
NAME_OK = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")
BACKUP_NAME = "eboot.before-claude-relay.bin"
VC_VERBS = ("press", "release", "wait", "screen", "unlock", "nosleep", "capture")
HOST = None
sleep = time.sleep
FULL = os.environ.get("STRIKERS_RELAY_FULL", "0") == "1" or (ROOT / "full-access.on").is_file()   # the owner's switch: a file in the relay root
DEVICE_RE = re.compile(r"^(ux0|ur0|uma0|imc0|grw0|xmc0|gro0|vs0|os0|vd0|tm0|ud0|pd0|sa0|ux0):")
BACKED = set()   # remote paths already copied to the Mac in this job


class Refused(ValueError):
    pass


def setup_dirs():
    for d in (INBOX, OUTBOX, STAGING, DONE, STATE):
        d.mkdir(parents=True, exist_ok=True)


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    try:
        with LOG.open("a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def vita_address():
    a = os.environ.get("VITA", "").strip()
    if not a:
        p = SCRIPT.parent / "vita-address.txt"
        if p.is_file():
            a = (p.read_text().splitlines() or [""])[0].strip()
    return a


# ---- the hard limits ------------------------------------------------------------------------------------------------------------------
def norm(p):
    if not isinstance(p, str):
        raise Refused(f"bad path {p!r}")
    p = re.sub(r"^([a-z0-9]+):/*", r"\1:", p)
    p = re.sub(r"/+", "/", p)
    return p.rstrip("/") if len(p) > 4 else p


def bad_chars(p):
    return ".." in p or "\\" in p or "\0" in p or "\n" in p or "\r" in p or len(p) > 240


def check_path(path, mode):
    """strict_path, or under FULL ACCESS any device path without '..' (refused characters stay refused)"""
    try:
        return strict_path(path, mode)
    except Refused:
        if not FULL: raise
        p = norm(path)
        if bad_chars(p) or not DEVICE_RE.match(p):
            raise Refused(f"path not allowed even with full access: {path!r}")
        return p


def is_strict(path, mode):
    try: strict_path(path, mode); return True
    except Refused: return False


def keep_copy(p):
    """FULL ACCESS: the file at p (outside the default limits) is copied to the Mac before it is overwritten or removed"""
    if is_strict(p, "write") or is_strict(p, "delete") or p in BACKED:
        return
    try:
        data = rget(p)
    except ftplib.error_perm:
        data = None   # not there (or a folder): nothing to keep
    BACKED.add(p)
    if data is not None:
        dest = ROOT / "backups" / time.strftime("%Y%m%d-%H%M%S") / p.replace(":", "_")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        log(f"kept a copy of {p} ({len(data)} bytes, sha {sha(data)[:12]}) in {dest}")


def strict_path(path, mode):
    """The one gate for every remote path. mode: read | write | delete | list. Returns the normalised path or raises Refused."""
    p = norm(path)
    if bad_chars(p):
        raise Refused(f"path not allowed: {path!r}")
    leaf = p.rsplit("/", 1)[-1]
    if mode == "write":
        if p in (f"{APP}/eboot.bin", f"{APP}/{BACKUP_NAME}", f"{DATA}/np-settings-open"):
            return p
        if p.startswith(DATA + "/") and p.count("/") == DATA.count("/") + 1 and ROUTE_NAME.match(leaf):
            return p
    elif mode == "delete":   # only what session.sh replaces: the old log, the env file (restore), a crash dump copy's gpucrash, the shots of the run, the settings hook
        if p in (f"{DATA}/log.txt", f"{DATA}/np-player.txt", f"{DATA}/np-settings-open", f"{DATA}/gpucrash.sgx"):
            return p
        if p.startswith(DATA + "/shots/") and p.count("/") == DATA.count("/") + 2 and NAME_OK.match(leaf):
            return p
    elif mode == "list":
        if p in (DATA, f"{DATA}/shots", APP):
            return p
    elif mode == "cachelist":   # (pull_cache only) the shader cache folders: shadercache, <key>, <key>/v3, v3/v|f, v3/v|f/XX
        sub = p[len(DATA) + 1:].split("/") if p.startswith(DATA + "/") else []
        if (sub and sub[0] == "shadercache" and len(sub) <= 5 and all(NAME_OK.match(x) for x in sub)
                and (len(sub) < 3 or sub[2] == "v3") and (len(sub) < 4 or sub[3] in ("v", "f"))):
            return p
    elif mode == "read":
        if p.startswith(DATA + "/") and all(NAME_OK.match(x) for x in p[len(DATA) + 1:].split("/")):
            return p
        if p.startswith(APP + "/") and all(NAME_OK.match(x) for x in p[len(APP) + 1:].split("/")):
            return p
        if re.fullmatch(r"ux0:data/psp2core-[A-Za-z0-9_.-]*\.psp2dmp", p):
            return p
    raise Refused(f"path not allowed ({mode}): {path!r}: only {DATA}/ and {APP}/ of {TITLE}")


def galaxy_running():
    try:
        r = subprocess.run(["pgrep", "-f", "vita_relay.py"], capture_output=True, text=True, timeout=10)
        return r.returncode == 0 and bool(r.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        return False


def need_free_vita():
    if galaxy_running():
        raise RuntimeError("Galaxy's relay (vita_relay.py) is running: both drive the same Vita, close it first")


# ---- FTP / command port --------------------------------------------------------------------------------------------------------------
def ftp():
    f = ftplib.FTP()
    f.connect(HOST, FTP_PORT, timeout=60)
    f.login()
    return f


def retry(fn, what="transfer"):
    last = None
    for i in range(TRIES):
        try:
            return fn()
        except ftplib.error_perm:
            raise   # a 5xx answer (no such file) is an answer, not a fault
        except (ftplib.Error, OSError, EOFError) as e:   # 426, 4xx, timeouts, resets
            last = e
            log(f"{what}: try {i + 1} failed ({type(e).__name__}: {e})")
            if i + 1 < TRIES:
                sleep(RETRY_S)
    raise RuntimeError(f"{what} failed after {TRIES} tries ({type(last).__name__}: {last})")


def sha(b):
    return hashlib.sha256(b).hexdigest()


def rget(path):
    p = check_path(path, "read")
    def one():
        f = ftp()
        try:
            b = io.BytesIO()
            f.retrbinary(f"RETR {p}", b.write)
            return b.getvalue()
        finally:
            try: f.close()
            except Exception: pass
    return retry(one, f"get {p}")


def rget_or_none(path):
    try:
        return rget(path)
    except ftplib.error_perm:
        return None


def rput(path, data):
    p = check_path(path, "write")
    keep_copy(p)
    def one():
        f = ftp()
        try:
            f.sock.settimeout(120)
            f.storbinary(f"STOR {p}", io.BytesIO(data), blocksize=32768)
        finally:
            try: f.close()
            except Exception: pass
    retry(one, f"put {p}")
    return p


def rput_verified(path, data):
    last = None
    for i in range(TRIES):
        rput(path, data)
        if rget(path) == data:
            return
        last = "readback mismatch"
        log(f"put {path}: attempt {i + 1}: {last}")
        sleep(RETRY_S)
    raise RuntimeError(f"put {path}: {last} after {TRIES} attempts")


def rdel(path):
    p = check_path(path, "delete")
    keep_copy(p)
    def one():
        f = ftp()
        try:
            try: f.delete(p)
            except ftplib.error_perm: pass   # not there
        finally:
            try: f.close()
            except Exception: pass
    retry(one, f"delete {p}")


def rlist(path, mode="list"):
    """[(name, is_dir, size)] of a folder; a folder that is not there gives []"""
    p = check_path(path, mode)
    def one():
        f = ftp()
        try:
            lines = []
            try: f.retrlines(f"LIST {p}", lines.append)
            except ftplib.error_perm: return []
            out = []
            for l in lines:
                fl = l.split(None, 8)
                if len(fl) < 9 or fl[-1] in (".", ".."): continue
                try: size = int(fl[4])
                except ValueError: size = -1
                out.append((fl[8], l.startswith("d"), size))
            return out
        finally:
            try: f.close()
            except Exception: pass
    return retry(one, f"ls {p}")


def names(folder):
    return [n for n, _, _ in rlist(folder)]


def dump_names():
    """names only of crash dumps in ux0:data (session.sh dump_names): a listing filtered at once"""
    def one():
        f = ftp()
        try:
            lines = []
            try: f.retrlines("LIST ux0:data", lines.append)
            except ftplib.error_perm: return []
            return sorted(l.split(None, 8)[-1] for l in lines if len(l.split(None, 8)) >= 9 and re.fullmatch(r"psp2core-[A-Za-z0-9_.-]*\.psp2dmp", l.split(None, 8)[-1]))
        finally:
            try: f.close()
            except Exception: pass
    return retry(one, "dump names")


def command(line, read_s=2.0):
    """one line to the command port; only version, help, quit/launch of the test title"""
    if not FULL and line not in ("version", "help", f"quit {TITLE}", f"launch {TITLE}"):
        raise Refused(f"command not allowed: {line!r}")
    return _send(line, read_s)


def _send(text, read_s):
    last = None
    for i in range(TRIES):
        try:
            with socket.create_connection((HOST, CMD_PORT), timeout=10) as s:
                s.sendall((text + "\n").encode())
                s.settimeout(read_s)
                out = b""
                try:
                    while True:
                        d = s.recv(65536)
                        if not d: break
                        out += d
                except (socket.timeout, OSError):
                    pass
            return out.decode("utf-8", "replace").replace("\r", "").strip()
        except OSError as e:
            last = e
            sleep(RETRY_S)
    raise RuntimeError(f"command port: {last}")


def vc(text, wait=1.0):
    parts = [p.strip() for p in str(text).split(";") if p.strip()]
    if not parts or (not FULL and any(p.split()[0] not in VC_VERBS for p in parts)):
        raise Refused(f"vc command not allowed: {text!r}")
    return _send(text, max(1.5, float(wait)))


# ---- the owner's np-player.txt (session.sh owner_backup / cmd_restore) ----------------------------------------------------------------
NP = f"{DATA}/np-player.txt"


def owner_backup():
    st = STATE / "owner-np-player.state"
    if st.exists():
        return
    data = rget_or_none(NP)
    if data is not None:
        (STATE / "owner-np-player.txt").write_bytes(data)
        st.write_text("present\n")
        log(f"the owner's np-player.txt is kept ({len(data)} bytes, sha {sha(data)[:12]})")
    else:
        st.write_text("none\n")
        log("no np-player.txt on the Vita before the session (restore deletes the session's)")


def do_restore():
    st = STATE / "owner-np-player.state"
    if not st.exists():
        return "restore: nothing was kept (no env file has been put in this session)"
    if st.read_text().strip() == "present":
        keep = (STATE / "owner-np-player.txt").read_bytes()
        rput(NP, keep)
        if sha(rget(NP)) != sha(keep):
            raise RuntimeError("np-player.txt on the Vita differs from the kept one after restore")
        return f"the owner's np-player.txt is back (sha {sha(keep)[:12]} checked)"
    rdel(NP)
    if "np-player.txt" in names(DATA):
        raise RuntimeError("the session's np-player.txt could not be removed")
    return "the np-player.txt of the session is removed (there was none before)"


def staged(name):
    p = STAGING / Path(str(name)).name
    if not p.is_file() or p.is_symlink():
        raise Refused(f"not in staging: {name!r}")
    return p


# ---- eboot ---------------------------------------------------------------------------------------------------------------------------
EBOOT = f"{APP}/eboot.bin"
EBOOT_OK = True


def put_eboot(data):
    global EBOOT_OK
    try:
        previous = rget(EBOOT)   # a failed read (not "no such file") stops here: nothing is written
    except ftplib.error_perm:
        previous = None
        log("put eboot: no eboot on the Vita now (an earlier install was cut off)")
    if previous is not None and BACKUP_NAME not in names(APP):
        rput(f"{APP}/{BACKUP_NAME}", previous)
        if rget(f"{APP}/{BACKUP_NAME}") != previous:
            raise RuntimeError("backup readback mismatch; nothing installed")
        (STATE / "eboot-before.bin").write_bytes(previous)
        log("eboot backed up on the Vita")
    last = None
    for i in range(TRIES):
        try:
            rput(EBOOT, data)
            if rget(EBOOT) == data:
                EBOOT_OK = True
                return f"{len(data)} bytes sha256 {sha(data)} verified (attempt {i + 1})"
            last = "readback mismatch"
        except RuntimeError as e:
            last = str(e)
        log(f"put eboot: attempt {i + 1} failed ({last})")
        sleep(RETRY_S)
    EBOOT_OK = False
    if previous is not None:
        try:
            rput(EBOOT, previous)
            if rget(EBOOT) == previous:
                EBOOT_OK = True
                log("put eboot: the previous eboot was put back")
        except Exception as e:
            log(f"put eboot: could not put the previous eboot back ({e}); launching is blocked until an install succeeds")
    raise RuntimeError(f"put eboot failed after {TRIES} attempts ({last})")


def do_eswap(req):
    """session.sh eswap: versions/<name>/eboot.bin (staged here as req["src"]) alone onto the test app, read back; state/eboot keeps the name"""
    name = str(req.get("name", ""))
    if not NAME_OK.match(name): raise Refused(f"bad executable name {name!r}")
    data = staged(req["src"]).read_bytes()
    st = STATE / "eboot-name"
    if st.exists() and st.read_text().strip() == name and not req.get("force"):
        return f"the Vita already has the executable {name} (state file); \"force\": true to push again"
    t0 = time.time()
    st.write_text("unknown\n")      # a cut-off swap must not leave the old name claiming the old executable
    try: command(f"quit {TITLE}")
    except Exception: pass
    sleep(float(req.get("nap_s", 2)))
    r = put_eboot(data)
    st.write_text(name + "\n")
    return f"the Vita now runs the executable {name} ({time.time() - t0:.1f} s, eboot.bin alone): {r}"


# ---- the run unit (session.sh cmd_run) -------------------------------------------------------------------------------------------------
def slice_log(raw):
    lines = raw.split(b"\n")
    idx = [i for i, l in enumerate(lines) if b"=====" in l and b"build" in l]
    return b"\n".join(lines[idx[-1]:]) if idx else raw


def do_run(req, out):
    if not EBOOT_OK:
        raise RuntimeError("run refused: the last eboot install failed (run a successful put first)")
    rid = Path(str(req.get("id", "run"))).name
    if not NAME_OK.match(rid):
        raise Refused(f"bad run id {rid!r}")
    steps, T0 = [], time.time()
    def step(name, fn):
        t = time.time(); r = fn(); steps.append(f"{name} [{time.time() - t:.1f} s]"); return r
    env = staged(req["env"]).read_bytes()
    if len(env) > 16384: raise Refused("env file over 16384 bytes")
    route_name = req.get("route_name")
    route_data = None
    if route_name:
        if not ROUTE_NAME.match(route_name): raise Refused(f"bad route name {route_name!r}")
        route_data = staged(req["route"]).read_bytes()
    settings = str(req.get("settings") or "")
    shots = bool(req.get("shots"))
    stab = bool(req.get("stab"))
    est = float(req.get("est_s", 0)); poll = float(req.get("poll_s", 10)); nap = float(req.get("nap_s", 3))
    stall = float(req.get("stall_s", 150 if stab else 90))
    first = float(req.get("first_poll_s", est))
    cap = float(req.get("cap_s", est * 2 + 120))
    lwait, lpoll = float(req.get("launch_wait_s", 75)), float(req.get("launch_poll_s", 5))
    step("owner backup", owner_backup)
    if route_data is not None:
        def put_route():
            rput_verified(f"{DATA}/{route_name}", route_data)
        step(f"route {route_name} in", put_route)
    step("env in as np-player.txt", lambda: rput_verified(NP, env))
    def clear_log():
        rdel(f"{DATA}/log.txt")
        if "log.txt" in names(DATA):
            rdel(f"{DATA}/log.txt")
            if "log.txt" in names(DATA):
                raise RuntimeError("the old log.txt could not be deleted: run not started")
    step("old log out", clear_log)
    if shots:
        def clear_shots():
            for n in names(f"{DATA}/shots"):
                rdel(f"{DATA}/shots/{n}")
        step("old shots out", clear_shots)
    if settings:
        step("np-settings-open in", lambda: rput(f"{DATA}/np-settings-open", (settings + "\n").encode()))
    dumps0 = step("dump names", dump_names)
    step("quit", lambda: command(f"quit {TITLE}")); sleep(nap)
    def launch():
        r = command(f"launch {TITLE}"); log(f"run {rid}: launch -> {r!r}"); return r
    step("launch", launch)
    launched = time.time()
    def log_started():
        t = 0.0
        while t < lwait:
            if rget_or_none(f"{DATA}/log.txt") is not None: return True
            sleep(lpoll); t += max(lpoll, 0.001)
        return False
    status = None
    if not step("wait for log start", log_started):
        step("quit", lambda: command(f"quit {TITLE}")); sleep(float(req.get("relaunch_nap_s", 10)))
        step("launch again", launch)
        if not step("wait for log start (2)", log_started): status = "nolaunch"
    wait_s = 0.0
    if status is None:
        t0 = time.time(); status = "timeout"; last_size = -1; same = 0
        sleep(min(first, 30.0))   # (a crash at boot is seen within 30 s, not after the route's estimate)
        while True:
            raw = rget_or_none(f"{DATA}/log.txt")
            if raw is not None:
                text = slice_log(raw).decode("utf-8", "replace")
                if FATAL_RE.search(text): status = "crashed"; break
                if DONE_RE.search(text): status = "done"; break
                same = same + 1 if len(raw) == last_size else 0
                last_size = len(raw)
                if same * poll >= stall and time.time() - t0 >= first: status = "stalled"; break   # (the stall rule as before: only after the first wait)
            if time.time() - t0 >= cap: break
            sleep(poll)
        wait_s = time.time() - t0
        steps.append(f"wait for the end of the route: {status} [{wait_s:.1f} s]")
    # the game is closed before the files are pulled (Galaxy's lesson)
    step("close", lambda: command(f"quit {TITLE}"))
    if status != "nolaunch":
        sleep(nap)
        def pull():
            raw = rget(f"{DATA}/log.txt")
            (out / "log.txt").write_bytes(slice_log(raw))
            return len(raw)
        step("log pull", pull)
        nshots = 0
        if shots:
            def pull_shots():
                n = 0
                for nm in names(f"{DATA}/shots"):
                    (out / "shots").mkdir(exist_ok=True)
                    (out / "shots" / nm).write_bytes(rget(f"{DATA}/shots/{nm}")); n += 1
                return n
            nshots = step("shots pull (not for judging pictures)", pull_shots)
        if settings:
            step("np-settings-open out", lambda: rdel(f"{DATA}/np-settings-open"))
        new = [d for d in dump_names() if d not in dumps0]
        if new:
            (out / "dumps.txt").write_text("\n".join(new) + "\n")
            log(f"CRASH DUMP written during {rid}: {' '.join(new)} (get ux0:data/<name> to copy)")
        if "gpucrash.sgx" in names(DATA):
            def gpu():
                (out / "gpucrash.sgx").write_bytes(rget(f"{DATA}/gpucrash.sgx")); rdel(f"{DATA}/gpucrash.sgx")
            step("gpucrash.sgx copied and removed", gpu)
    (out / "status.txt").write_text(status + "\n")
    total = time.time() - T0
    return {"id": rid, "status": status, "ok_run": status == "done", "route_wait_s": round(wait_s, 1), "since_launch_s": round(time.time() - launched, 1),
            "total_s": round(total, 1), "steps": steps, "log": "log.txt" if status != "nolaunch" else None}


# ---- pull_logs, status, shot ---------------------------------------------------------------------------------------------------------
def do_pull_logs(req, out):
    t0 = time.time(); steps = []
    if req.get("close", True):
        command(f"quit {TITLE}"); sleep(float(req.get("nap_s", 2))); steps.append("closed")
    got = []
    for n in names(DATA):
        if n.startswith(("log", "stderr", "env", "settings", "crash", "np-player")) and not n.endswith(".pack"):
            if not req.get("all") and (n.startswith("env.txt.") or n == "log.txt.prev"): continue
            t = time.time(); (out / n).write_bytes(rget(f"{DATA}/{n}")); got.append(f"{n} [{time.time() - t:.1f} s]")
    return {"files": got, "steps": steps, "total_s": round(time.time() - t0, 1)}


def do_pull_cache(req, out):
    """Read-only: the console's own shader cache (what the game compiled in play) for tools/seed/assemble_seed.sh. Only names the cachelist gate allows."""
    t0 = time.time(); steps = []
    if req.get("close") is not False:
        command(f"quit {TITLE}"); sleep(float(req.get("nap_s", 2))); steps.append("closed")
    root = out / "shadercache"; root.mkdir(parents=True, exist_ok=True)
    got, kept, bad = 0, 0, []
    top = rlist(f"{DATA}/shadercache", "cachelist")
    for n, is_dir, size in top:
        if not is_dir and n in ("vgx_shaderlist.bin", "vgx_shaderlist.stamp"):
            (root / n).write_bytes(rget(f"{DATA}/shadercache/{n}")); got += 1
    for key, kdir, _ in top:
        if not kdir or not NAME_OK.match(key): continue
        for kind in ("v", "f"):
            for sub, sdir, _ in rlist(f"{DATA}/shadercache/{key}/v3/{kind}", "cachelist"):
                if not sdir or not NAME_OK.match(sub): continue
                for fn, fdir, size in rlist(f"{DATA}/shadercache/{key}/v3/{kind}/{sub}", "cachelist"):
                    if fdir or not fn.endswith(".gxp") or not NAME_OK.match(fn): bad.append(fn); continue
                    dst = root / key / "v3" / kind / sub / fn
                    if dst.is_file() and dst.stat().st_size == size: kept += 1; continue
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    dst.write_bytes(rget(f"{DATA}/shadercache/{key}/v3/{kind}/{sub}/{fn}")); got += 1
    return {"files": got, "already_there": kept, "skipped_names": bad[:10], "steps": steps, "total_s": round(time.time() - t0, 1)}


def do_shot(req, out):
    r = vc("capture 0", 4)
    def one():   # the one file outside the data root this relay reads: VitaCompanion's own capture, read-only, never through get
        f = ftp()
        try:
            b = io.BytesIO(); f.retrbinary("RETR ux0:data/vitacompanion/plane0.bmp", b.write); return b.getvalue()
        finally:
            try: f.close()
            except Exception: pass
    data = retry(one, "shot")
    name = Path(str(req.get("name", "shot"))).name
    (out / f"{name}.bmp").write_bytes(data)
    return f"{name}: {len(data)} bytes {r}; NOT FOR JUDGING PICTURES: screenshots through the relay are torn, look at the screen"


def do_status(req, out):
    ver = command("version")
    listing = [f"{n}\t{'dir' if d else s}" for n, d, s in rlist(DATA)]
    (out / "listing.txt").write_text("\n".join(listing) + "\n")
    st = STATE / "owner-np-player.state"
    return {"version": ver, "data_entries": len(listing), "owner_np_player": st.read_text().strip() if st.exists() else "not kept yet",
            "eboot_ok": EBOOT_OK, "galaxy_relay_running": galaxy_running()}


def handle(req, out):
    act = req.get("action")
    if act != "status" and act is not None:
        need_free_vita() if act != "sleep" else None
    if act == "status":
        return do_status(req, out)
    if act == "ls":
        folder = req.get("path", DATA)
        listing = [f"{'d' if d else '-'} {s}\t{n}" for n, d, s in rlist(folder)]
        (out / "listing.txt").write_text("\n".join(listing) + "\n"); return f"{len(listing)} entries"
    if act == "get":
        p = check_path(req["path"], "read")
        data = rget(p); (out / p.rsplit("/", 1)[1]).write_bytes(data)
        return f"{len(data)} bytes sha256 {sha(data)}"
    if act == "put":
        p = check_path(req["path"], "write")
        if p == f"{APP}/{BACKUP_NAME}": raise Refused("the backup file is written by the eboot put only")
        data = staged(req["src"]).read_bytes()
        if p == EBOOT:
            return put_eboot(data)
        if len(data) > (419430400 if FULL else 1048576): raise Refused("file too big")
        if p == NP: owner_backup()
        rput_verified(p, data)
        return f"{len(data)} bytes sha256 {sha(data)} verified"
    if act == "launch":
        if not EBOOT_OK: raise RuntimeError("launch refused: the last eboot install failed (run a successful put first)")
        return command(f"launch {TITLE}") or "sent"
    if act == "close":
        return command(f"quit {TITLE}") or "sent"
    if act == "eswap": return do_eswap(req)
    if act == "run": return do_run(req, out)
    if act == "restore": return do_restore()
    if act == "pull_logs": return do_pull_logs(req, out)
    if act == "pull_cache": return do_pull_cache(req, out)
    if act == "shot": return do_shot(req, out)
    if act == "vc": return vc(req["cmd"], float(req.get("wait", 1.0)))
    if act in ("rm", "mkdir", "rmdir", "mv", "cmd") and not FULL:
        raise Refused(f"{act}: only with full access (STRIKERS_RELAY_FULL=1)")
    if act == "cmd": return _send(str(req["line"]), float(req.get("wait", 2.0))) or "sent"
    if act == "rm":
        p = check_path(req["path"], "delete"); keep_copy(p)
        def one():
            f = ftp()
            try: f.delete(p)
            finally: f.close()
        retry(one, f"rm {p}"); return f"removed {p}"
    if act in ("mkdir", "rmdir"):
        p = check_path(req["path"], "write")
        def one():
            f = ftp()
            try: (f.mkd if act == "mkdir" else f.rmd)(p)
            finally: f.close()
        retry(one, f"{act} {p}"); return f"{act} {p}"
    if act == "mv":
        a, b = check_path(req["src"], "delete"), check_path(req["path"], "write")
        keep_copy(a); keep_copy(b)
        def one():
            f = ftp()
            try: f.rename(a, b)
            finally: f.close()
        retry(one, f"mv {a}"); return f"{a} -> {b}"
    if act == "sleep":
        sleep(float(req.get("s", 1))); return f"slept {req.get('s', 1)}"
    if act == "batch":
        results, failed = [], None
        for i, st in enumerate(req["steps"]):
            if failed and not st.get("always"):
                results.append(f"{st.get('action', 'sleep')}: SKIPPED (step {failed} failed)"); continue
            if "sleep" in st and "action" not in st:
                sleep(float(st["sleep"])); results.append(f"slept {st['sleep']}"); continue
            sub = out / f"{i:02d}"; sub.mkdir(exist_ok=True)
            t0 = time.time()
            try:
                r = handle(st, sub)
                results.append({"action": st.get("action"), "result": r, "s": round(time.time() - t0, 1)})
            except Exception as e:
                failed = failed or i
                results.append({"action": st.get("action"), "error": f"{type(e).__name__}: {e}", "s": round(time.time() - t0, 1)})
        if failed: raise RuntimeError(json.dumps(results, indent=1))
        return results
    raise ValueError(f"unknown action {act!r}")


def process(req_file):
    rid = req_file.stem
    BACKED.clear()
    out = OUTBOX / rid; out.mkdir(exist_ok=True)
    try:
        req = json.loads(req_file.read_text())
        log(f"{rid}: {req.get('action')} {req.get('path', req.get('id', ''))}")
        result = {"ok": True, "result": handle(req, out)}
    except Exception as e:
        result = {"ok": False, "error": f"{type(e).__name__}: {e}", "trace": traceback.format_exc()}
    result["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    (out / "result.json").write_text(json.dumps(result, indent=1))
    log(f"{rid}: {'ok' if result['ok'] else 'FAILED ' + result['error'][:300]}")
    try: req_file.replace(DONE / req_file.name)
    except OSError: pass
    return result


def main():
    global HOST
    HOST = vita_address()
    if not HOST:
        sys.exit("No Vita address: set VITA=<address> or put it on the first line of vita-address.txt next to this script (never committed).")
    setup_dirs()
    if galaxy_running():
        sys.exit("Galaxy's relay (vita_relay.py) is running: both drive the same Vita. Close it, then start this one.")
    stamp = SCRIPT.stat().st_mtime
    log(f"relay up: Strikers test title {TITLE}, Vita {HOST}, watching {INBOX}" + (" -- FULL ACCESS (backups in " + str(ROOT / "backups") + ")" if FULL else ""))
    paused = False
    while True:
        if SCRIPT.stat().st_mtime != stamp:
            sleep(1); log("relay reloaded (new version)"); os.execv(sys.executable, [sys.executable, str(SCRIPT)])
        if galaxy_running():
            if not paused: log("Galaxy's relay is running: no jobs are taken until it is closed"); paused = True
            sleep(2); continue
        if paused: log("Galaxy's relay is gone: taking jobs again"); paused = False
        jobs = sorted(INBOX.glob("*.json"))   # one job a pass: a job queued with a lower number goes next, not after the whole queue
        if jobs:
            process(jobs[0]); continue
        sleep(2)


if __name__ == "__main__":
    while True:
        try: main(); break
        except KeyboardInterrupt: log("relay stopped"); break
        except SystemExit: raise
        except Exception as e:
            log(f"relay error {type(e).__name__}: {e}; restarting in 5 s"); time.sleep(5)
