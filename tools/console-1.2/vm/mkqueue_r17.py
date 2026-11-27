#!/usr/bin/env python3
"""Turn the r17 sitting (r17-sitting.txt) into what the Mac-VM slice tools (q.sh, slice.sh, swap.sh) run.

    python3 tools/console-1.2/vm/mkqueue_r17.py [--sitting FILE] [--out-dir DIR] [--versions-dir versions] [--check]

Writes, next to this script (or into --out-dir):
  queue-r17.txt            q.sh queue: `<id> <id>` per run, the sitting's order and block comments, `!eswap versions/<name>/eboot.bin`,
                           `!restore`, `!stop <text>` (the sitting's natural stop point)
  kit-r17q/env/<id>.txt    the env file each run uses ($KIT/env/<id>.txt for q.sh/slice.sh): env-fb/<id>.txt while the sitting's
                           `fastboot 1` is in force and the run has one, else env/<id>.txt (copied byte for byte)
  kit-r17q/routes/*.txt    the route files those env files name (fastboot/routes/ or routes/), byte for byte
  kit-r17q/routes.sha256   sha-256 of every route, and kit-r17q/ROUTES-NEEDED.txt (route -> the runs that use it)
Nothing is sent anywhere. --check regenerates in memory and says whether the files on disk are up to date (exit 1 if not).
"""
import argparse, hashlib, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CON = HERE.parent
ROUTE_RE = re.compile(r"^MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/([A-Za-z0-9_-]+\.txt)\s*$", re.M)
STOP_RE = re.compile(r"NATURAL STOP POINT", re.I)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def build(sitting, versions):
    lines = sitting.read_text().splitlines()
    q = ["# r17 queue for the Mac-VM slice tools (made by mkqueue_r17.py from %s; do not edit, regenerate)" % sitting.name,
         "# q.sh format: <id> <base env id>; !eswap <eboot file>; !restore; !stop <text>. Env files: kit-r17q/env/<id>.txt"]
    env_files, routes, uses = {}, {}, {}
    fast, nruns, minutes, eswaps, stops = False, 0, 0.0, [], 0
    for ln in lines:
        if not ln.strip():
            continue
        f = ln.split("\t")
        k = f[0]
        if k == "fastboot":
            fast = f[1].strip() == "1"
            q.append("# (fast boot %s from here: env files are %s)" % ("ON" if fast else "off", "env-fb/" if fast else "env/"))
        elif k == "run":
            rid = f[1]
            src = CON / "env-fb" / (rid + ".txt")
            if not (fast and src.is_file()):
                src = CON / "env" / (rid + ".txt")
            if not src.is_file():
                raise SystemExit("no env file for run %s" % rid)
            if rid in env_files:
                raise SystemExit("run id %s appears twice (q.sh keys results and env files by id)" % rid)
            data = src.read_bytes()
            env_files[rid] = (src.relative_to(CON).as_posix(), data)
            m = ROUTE_RE.findall(data.decode())
            if len(m) != 1:
                raise SystemExit("%s: expected exactly one MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/<name>.txt, found %d" % (src, len(m)))
            r = m[0]
            rsrc = next((p for p in (CON / "fastboot" / "routes" / r, CON / "routes" / r) if p.is_file()), None)
            if rsrc is None:
                raise SystemExit("%s names route %s, which is in neither fastboot/routes/ nor routes/" % (src, r))
            routes[r] = (rsrc.relative_to(CON).as_posix(), rsrc.read_bytes())
            uses.setdefault(r, []).append(rid)
            q.append("%s %s" % (rid, rid))
            nruns += 1
            minutes += float(f[3]) if len(f) > 3 else 0.0
        elif k == "eswap":
            q.append("!eswap %s/%s/eboot.bin" % (versions, f[1]))
            eswaps.append(f[1])
        elif k == "restore":
            q.append("!restore")
        elif k in ("swap", "swapfull"):
            raise SystemExit("the sitting has a full `swap` line (%s): the slice tools only swap the executable" % ln)
        elif ln.startswith("#"):
            if STOP_RE.search(ln):
                q.append("!stop " + ln.lstrip("# -").strip())
                stops += 1
            else:
                q.append(ln)
        else:
            raise SystemExit("unknown sitting line: %r" % ln)
    return q, env_files, routes, uses, dict(runs=nruns, minutes=round(minutes, 1), eswaps=eswaps, stops=stops)


def outputs(sitting, versions):
    q, env_files, routes, uses, st = build(sitting, versions)
    out = {"queue-r17.txt": ("\n".join(q) + "\n").encode()}
    for rid, (_, data) in env_files.items():
        out["kit-r17q/env/%s.txt" % rid] = data
    for r, (_, data) in routes.items():
        out["kit-r17q/routes/%s" % r] = data
    out["kit-r17q/routes.sha256"] = "".join("%s  %s\n" % (sha(d), r) for r, (_, d) in sorted(routes.items())).encode()
    need = ["# route files the r17 queue needs on the console (ux0:data/mscharged-perf-test/), uploaded once by `slice.sh routes`",
            "# route | kit source | runs"]
    for r, (srcp, _) in sorted(routes.items()):
        need.append("%s | %s | %s" % (r, srcp, " ".join(uses[r])))
    out["kit-r17q/ROUTES-NEEDED.txt"] = ("\n".join(need) + "\n").encode()
    return out, st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sitting", default=str(CON / "r17-sitting.txt"))
    ap.add_argument("--out-dir", default=str(HERE))
    ap.add_argument("--versions-dir", default="versions", help="where the eboots sit on the Mac, relative to q.sh's folder (versions/<name>/eboot.bin)")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    out, st = outputs(Path(a.sitting), a.versions_dir)
    root = Path(a.out_dir)
    if a.check:
        bad = [n for n, d in out.items() if not (root / n).is_file() or (root / n).read_bytes() != d]
        stale = [p.relative_to(root).as_posix() for p in (root / "kit-r17q").rglob("*") if p.is_file() and p.relative_to(root).as_posix() not in out] if (root / "kit-r17q").is_dir() else []
        if bad or stale:
            print("out of date: %s %s" % (bad[:5], stale[:5])); return 1
        print("up to date: %d files" % len(out)); return 0
    import shutil
    if (root / "kit-r17q").is_dir():
        shutil.rmtree(root / "kit-r17q")
    for n, d in out.items():
        p = root / n; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(d)
    print("%d runs, %.1f min of runs (sitting estimates), eswaps %s, stop points %d, %d route files, %d env files -> %s"
          % (st["runs"], st["minutes"], st["eswaps"], st["stops"], sum(1 for n in out if "/routes/" in n), st["runs"], root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
