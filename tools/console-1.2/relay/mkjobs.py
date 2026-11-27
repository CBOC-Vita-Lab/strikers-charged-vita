#!/usr/bin/env python3
"""Turn entries of the kit's sitting lists (r17-sitting.txt, r17-sitting-extra.txt) into relay job files, and read the results back.

  python3 mkjobs.py jobs r17-sitting.txt --ids d1,lock60tour          queue those runs (in list order) + one restore job at the end
  python3 mkjobs.py jobs r17-sitting.txt --unit A                     every run of the block "# ==== A: ..." of the list
  python3 mkjobs.py jobs r17-sitting.txt --ids d1 --dry               print what would be queued, write nothing
  python3 mkjobs.py results [--ids d1,...]                            analyse.py on the pulled logs, one pass/INVALID line per run
  python3 mkjobs.py ab d1 lock60tour [more ids]                       A/B table per scene (p50/p95/p99 ms, fps) of the first run against the others, from the pulled logs
  python3 mkjobs.py release [NUM ...]                                 rename parked jobs (<num>-<id>.json.hold) to .json so the relay takes them (no NUM: all)
  jobs ... --hold                                                     write the jobs parked (.json.hold); --end-with NAME puts the plain executable NAME back (eswap) before the restore job

Options: --start 5000 (first job number; default: after the highest number in inbox/outbox/done), --relay DIR (default: next to this script),
--assume-eboot NAME (the console already has that executable: no put of it), --no-eboot (never put an eboot), --est-scale X.
The kit's env/ (or env-fb/ for fast-boot runs), routes/ and the versions/ folder (prepare-versions.py; default <kit>/versions or $VERSIONS) are what is staged.
`swap` lines (shader pack changes) are NOT supported by the relay: a run that needs one is refused here.
Job files are written to <relay>/inbox as <num>-<id>.json (a batch of: eboot put if needed, run), and <num>-restore.json last."""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KIT = HERE.parent
STAB = ("cstab", "cintro", "cend")
DATA = "ux0:data/mscharged-perf-test"


def parse_list(path):
    """[(kind, fields, block)]: block is the letter/word after '# ==== ' in force at that line"""
    out, block = [], ""
    for line in Path(path).read_text().splitlines():
        m = re.match(r"#\s*====\s*([A-Za-z0-9_-]+)", line)
        if m: block = m.group(1).rstrip(":")
        if not line or line.startswith("#"): continue
        f = line.split("\t")
        out.append((f[0], f[1:], block))
    return out


def next_number(relay, start):
    if start: return start
    nums = [int(m.group(1)) for d in ("inbox", "outbox", "done") for p in (relay / d).glob("*") if (m := re.match(r"(\d+)", p.name))]
    return max([n for n in nums if n >= 5000] + [4999]) + 1


def select(entries, ids, unit):
    """walk the list keeping the state (fastboot flag, current eswap/swap name) and return [(run fields, fastboot, eswap, swap)] of the chosen runs"""
    fb, esw, swp, chosen = 0, None, None, []
    for kind, f, block in entries:
        if kind == "fastboot": fb = int(f[0])
        elif kind == "eswap": esw = f[0]
        elif kind == "swap": swp = f[0]
        elif kind == "run":
            if (ids and f[0] in ids) or (unit and block == unit):
                chosen.append((f, fb, esw, swp, block))
    return chosen


def build(entries, ids, unit, relay, versions, start, assume, no_eboot, scale, plan, end_with=None):
    sys.path.insert(0, str(KIT))
    import matrix
    by_id = {r["id"]: r for r in plan["runs"]}
    chosen = select(entries, ids, unit)
    known = {f[0] for k, f, _ in entries if k == "run"}
    missing = [i for i in (ids or []) if i not in known]
    if missing: raise SystemExit(f"not in the list: {', '.join(missing)}")
    if not chosen: raise SystemExit("nothing selected (--ids or --unit matched no run)")
    jobs, files, have = [], {}, assume
    num = next_number(relay, start)
    for f, fb, esw, swp, block in chosen:
        rid, route = f[0], f[1]
        r = by_id.get(rid)
        if r is None: raise SystemExit(f"{rid}: not in plan.json (run gen_kit.py)")
        if swp and swp != r["version"] and swp not in ("1.3",) :
            raise SystemExit(f"{rid}: the list swaps the shader pack/version to {swp}: the relay cannot do a swap (eboot.bin alone only)")
        env_src = KIT / ("env-fb" if route.startswith("fb-") else "env") / f"{rid}.txt"
        if not env_src.is_file(): raise SystemExit(f"{rid}: no env file {env_src} (python3 gen_kit.py)")
        text = env_src.read_text()
        m = re.search(r"^MSC_INPUT_SCRIPT=.*/(.+)$", text, re.M)
        rname = m.group(1) if m else None
        rsrc = None
        if rname:
            for c in (KIT / "routes" / rname, KIT / "fastboot" / "routes" / rname):
                if c.is_file(): rsrc = c; break
            if rsrc is None: raise SystemExit(f"{rid}: the route {rname} is not in the kit")
        files[f"env-{rid}.txt"] = env_src
        steps = []
        if esw and esw != have and not no_eboot:
            eb = versions / esw / "eboot.bin"
            if not eb.is_file(): raise SystemExit(f"{rid}: no {eb} (prepare-versions.py --eboot {esw}=<vpk>)")
            files[f"eboot-{esw}.bin"] = eb
            steps.append({"action": "put", "src": f"eboot-{esw}.bin", "path": "ux0:app/CDXMP0001/eboot.bin"}); have = esw
        est = (matrix.ROUTE_MIN.get(route) or matrix.FB_ROUTE_MIN[route]) * 60 * 1.15 * scale
        run = {"action": "run", "id": rid, "env": f"env-{rid}.txt", "est_s": int(est), "stab": route in STAB,
               "settings": r.get("settings") or "", "shots": bool(route == "look" or r.get("settings") or "MSC_VITA_SHOT=" in text)}
        if rname:
            files[f"route-{rname}"] = rsrc
            run.update(route=f"route-{rname}", route_name=rname)
        if route in STAB:   # session.sh: a stability route is polled early and often, patient stall time
            run.update(poll_s=30)
        steps.append(run)
        jobs.append((f"{num}-{rid}", {"action": "batch", "steps": steps, "_note": f"{rid}: {route}, block {block}, eboot {esw or 'installed'}"}))
        num += 1
    if end_with and not no_eboot:
        eb = versions / end_with / "eboot.bin"
        if not eb.is_file(): raise SystemExit(f"--end-with: no {eb}")
        files[f"eboot-{end_with}.bin"] = eb
        jobs.append((f"{num}-eswap-{end_with}", {"action": "eswap", "name": end_with, "src": f"eboot-{end_with}.bin", "_note": f"put the executable {end_with} back"}))
        num += 1
    jobs.append((f"{num}-restore", {"action": "batch", "steps": [{"action": "restore", "always": True}]}))
    return jobs, files


def results(relay, ids):
    sys.path.insert(0, str(KIT))
    sess = relay / "session"
    rows = []
    for rf in sorted((relay / "outbox").glob("*/result.json")):
        res = json.loads(rf.read_text())
        r = res.get("result")
        if not res.get("ok") and isinstance(res.get("error"), str) and res["error"].startswith("RuntimeError: ["):   # a failed batch keeps its step results in the error text
            try: r = json.loads(res["error"].split(": ", 1)[1])
            except ValueError: r = None
        if not isinstance(r, list): continue
        for i, s in enumerate(r):
            x = s.get("result") if isinstance(s, dict) else None
            if isinstance(x, dict) and "status" in x and "id" in x and (not ids or x["id"] in ids):
                rows.append((rf.parent.name, i, x))
    if not rows: print("no run results in", relay / "outbox"); return 1
    latest = {}
    for job, i, x in rows: latest[x["id"]] = (job, i, x)    # a later job of the same run id wins
    # (16 Nov) nothing is removed: on the owner's Mac the kit folder does not allow deletion (rmtree failed silently and the next
    # mkdir stopped the command); each run's files are written over in place, and runs of earlier calls stay beside them.
    verdicts = {}
    for rid, (job, i, x) in latest.items():
        d = sess / rid; d.mkdir(parents=True, exist_ok=True)
        src = relay / "outbox" / job / f"{i:02d}"
        for n in ("log.txt", "status.txt"):
            if (src / n).is_file(): shutil.copy(src / n, d / n)
        if (src / "shots").is_dir(): shutil.copytree(src / "shots", d / "shots", dirs_exist_ok=True)
        text = (d / "log.txt").read_text(errors="replace") if (d / "log.txt").is_file() else ""
        why = []
        if x["status"] != "done": why.append(f"status {x['status']}")
        if "[frame-timing]" not in text: why.append("no [frame-timing] lines")
        if "=====" not in text: why.append("no build header")
        verdicts[rid] = (not why, "; ".join(why))
    a = subprocess.run([sys.executable, str(KIT / "analyse.py"), str(sess)], capture_output=True, text=True)
    print(a.stdout.rstrip() if a.returncode == 0 else f"(analyse.py failed, exit {a.returncode}: {a.stderr.strip()[-300:]})")
    print()
    for rid, (job, i, x) in latest.items():
        ok, why = verdicts[rid]
        print(f"{rid}: {'pass' if ok else 'INVALID (' + why + ')'}  [{x['status']}, route wait {x.get('route_wait_s')} s, total {x.get('total_s')} s; job {job}]")
    return 0


def scene_stats(log):
    """{scene: (windows, fps, p50 ms, p95 ms, p99 ms)} from the [frame-timing] lines (medians over the windows of a scene)"""
    import statistics
    acc = {}
    for ln in Path(log).read_text(errors="replace").splitlines():
        if "[frame-timing]" not in ln: continue
        kv = dict(re.findall(r"(\w+)=(\S+)", ln))
        try: n, tot, p50, p95, p99 = (float(kv[k]) for k in ("n", "total_us", "p50_us", "p95_us", "p99_us"))
        except (KeyError, ValueError): continue
        if tot > 0: acc.setdefault(kv.get("scene", "?"), []).append((n / tot * 1e6, p50 / 1000, p95 / 1000, p99 / 1000))
    return {sc: (len(v),) + tuple(statistics.median(x[i] for x in v) for i in range(4)) for sc, v in acc.items()}


def ab(relay, ids):
    sess = relay / "session"
    if not sess.is_dir(): print("no session folder: run `mkjobs.py results` first"); return 1
    stats = {}
    for i in ids:
        lg = sess / i / "log.txt"
        if not lg.is_file(): print(f"{i}: no pulled log"); return 1
        stats[i] = scene_stats(lg)
    a = ids[0]
    print(f"A = {a}; per scene: windows, fps, p50 / p95 / p99 ms (delta against A for the others). Windows are medians; read the noise of d1 against d1 before a verdict.")
    for sc in sorted(set().union(*[set(s) for s in stats.values()])):
        print(f"== {sc}")
        for i in ids:
            v = stats[i].get(sc)
            if not v: print(f"  {i:<18} (no windows)"); continue
            d = ""
            if i != a and sc in stats[a]:
                b = stats[a][sc]; d = "   d: fps %+.1f  p50 %+.2f  p95 %+.2f  p99 %+.2f" % (v[1] - b[1], v[2] - b[2], v[3] - b[3], v[4] - b[4])
            print("  %-18s n=%-3d fps %5.1f  p50 %5.2f  p95 %5.2f  p99 %5.2f%s" % (i, v[0], v[1], v[2], v[3], v[4], d))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    j = sub.add_parser("jobs"); j.add_argument("list"); j.add_argument("--ids"); j.add_argument("--unit"); j.add_argument("--dry", action="store_true")
    j.add_argument("--start", type=int, default=0); j.add_argument("--relay"); j.add_argument("--versions"); j.add_argument("--assume-eboot")
    j.add_argument("--no-eboot", action="store_true"); j.add_argument("--est-scale", type=float, default=1.0)
    j.add_argument("--hold", action="store_true"); j.add_argument("--end-with")
    ab_ = sub.add_parser("ab"); ab_.add_argument("ids", nargs="+"); ab_.add_argument("--relay")
    rl = sub.add_parser("release"); rl.add_argument("nums", nargs="*"); rl.add_argument("--relay")
    r = sub.add_parser("results"); r.add_argument("--ids"); r.add_argument("--relay")
    a = ap.parse_args()
    relay = Path(a.relay) if a.relay else Path(os.environ["STRIKERS_RELAY_ROOT"]) if os.environ.get("STRIKERS_RELAY_ROOT") else HERE / ("relay-" + (re.sub(r"[^A-Za-z0-9._-]", "", os.environ.get("STRIKERS_SITTING") or "r17") or "r17"))
    if a.cmd == "ab": return ab(relay, a.ids)
    if a.cmd == "release":
        n = 0
        for p in sorted((relay / "inbox").glob("*.json.hold")):
            if not a.nums or any(p.name.startswith(x + "-") for x in a.nums): p.replace(p.with_suffix("")); n += 1; print("released", p.name[:-5])
        return 0 if n else 1
    if a.cmd == "results":
        return results(relay, a.ids.split(",") if a.ids else None)
    lst = Path(a.list)
    if not lst.is_file() and (KIT / a.list).is_file(): lst = KIT / a.list
    versions = Path(a.versions or os.environ.get("VERSIONS") or KIT / "versions")
    plan = json.loads((KIT / "plan.json").read_text())
    jobs, files = build(parse_list(lst), a.ids.split(",") if a.ids else None, a.unit, relay, versions, a.start, a.assume_eboot, a.no_eboot, a.est_scale, plan, a.end_with)
    for n, job in jobs: print(f"{n}.json: {job.get('_note', 'restore the owner np-player.txt')}")
    if a.dry: return 0
    for d in ("inbox", "staging"): (relay / d).mkdir(parents=True, exist_ok=True)
    for n, src in files.items():
        dst = relay / "staging" / n
        if not dst.exists() or dst.read_bytes() != src.read_bytes(): shutil.copy(src, dst)
    for n, job in jobs:   # written last and atomically, so the relay never reads half a file
        job = {k: v for k, v in job.items() if not k.startswith("_")}
        tmp = relay / "inbox" / f".{n}.tmp"; tmp.write_text(json.dumps(job)); tmp.replace(relay / "inbox" / (f"{n}.json.hold" if a.hold else f"{n}.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
