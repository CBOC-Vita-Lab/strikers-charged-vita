import re, sys, hashlib
from pathlib import Path
fake, qleft, qorig, kit, sit = (Path(x) for x in sys.argv[1:6])
W = fake / "home/x/w9"; con = fake / "console/ux0"; fails = []
def ok(c, m):
    print(("PASS " if c else "FAIL ") + m)
    if not c: fails.append(m)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()[:12]
q = [l for l in qorig.read_text().splitlines() if l.strip() and not l.startswith("#")]
runs = [l.split()[0] for l in q if not l.startswith("!")]
# 1. every env exists, every route an env names is in the upload list and on the (fake) console
need = {l.split("|")[0].strip() for l in (kit / "ROUTES-NEEDED.txt").read_text().splitlines() if "|" in l and not l.startswith("#")}
perf = con / "data/mscharged-perf-test"
miss = [r for r in runs if not (kit / "env" / (r + ".txt")).is_file()]
ok(not miss, "every run has kit-r17q/env/<id>.txt (%d runs)" % len(runs))
named = {re.search(r"^MSC_INPUT_SCRIPT=\S*/([^/\s]+)$", (kit / "env" / (r + ".txt")).read_text(), re.M).group(1) for r in runs}
ok(named <= need, "routes named by the envs %s are all in the upload list %s" % (sorted(named), sorted(need)))
ok(all((perf / r).is_file() and sha(perf / r) == sha(kit / "routes" / r) for r in need), "all %d route files are on the console, hash equal" % len(need))
# 2. launches in the sitting's order, each with its env hash, route present, and the right executable
la = [l.split(" ", 1)[1] for l in (fake / "launches.log").read_text().splitlines()]
ok(len(la) == len(runs), "launches %d == runs %d" % (len(la), len(runs)))
if len(la) != len(runs):   # name the run ids that have no launch (the i-th launch belongs to the i-th run; the first launch that differs from its run's env hash is where one went missing)
    ehash = [sha(kit / "env" / (r + ".txt")) for r in runs]; got = [dict(x.split("=") for x in l.split()).get("np") for l in la]
    k = 0
    while k < len(la) and k < len(runs) and got[k] == ehash[k]: k += 1
    print("FAIL no launch recorded for run %s (launch %d of %d); runs without a launch if only that one is missing: %s" % (runs[k] if k < len(runs) else "?", k + 1, len(runs), [r for r in runs if sha(kit / "env" / (r + ".txt")) not in got]))
    la += [""] * (len(runs) - len(la))
eb = {"r17": sha(W / "versions/r17/eboot.bin"), "r17pgo": sha(W / "versions/r17pgo/eboot.bin")}
cur = eb["r17"]; i = 0; bad = []; swaps = []
for l in q:
    if l.startswith("!eswap"): cur = eb[l.split("/")[1]]; swaps.append(l.split("/")[1])
    elif not l.startswith("!"):
        d = dict(x.split("=") for x in la[i].split()) if i < len(la) and la[i] else {"np": "-", "eboot": "-", "routefile": "-"}; e = kit / "env" / (l.split()[0] + ".txt")
        if d["np"] != sha(e) or d["eboot"] != cur or d["routefile"] != "True": bad.append((l, d, cur))
        i += 1
ok(not bad, "each launch used its own env file, the route was on the console, and the executable was the one the last !eswap put (%s)" % (bad[:1],))
ok(swaps == [l.split("/")[1] for l in q if l.startswith("!eswap")], "eswap points as in the sitting: %s" % " ".join(swaps))
# 3. restore, stop, empty
ok(sha(perf / "np-player.txt") == sha(W / "out/owner/np-player.txt"), "the owner's np-player.txt is back at the end")
ok(sha(con / "app/CDXMP0001/eboot.bin") == eb["r17"], "the r17 executable is on the console at the end")
calls = (fake / "calls.log").read_text()
ok("QUEUE EMPTY" in calls, "QUEUE EMPTY at the end")
ok(len(re.findall(r"^STOP POINT", calls, re.M)) == 1, "one STOP POINT, after the 7th eswap/restore: " + " | ".join(re.findall(r"^STOP POINT: (.{30})", calls, re.M)))
ops = (fake / "ops.log").read_text().splitlines()
# restore before each eswap block ends: the owner's file PUT must come right before each eswap's eboot PUT and at the stop point
seq = [("E" if "eboot.bin" in o else "R" if "np-player.txt" in o and sha(W/'out/owner/np-player.txt')[:12] in o else "") for o in ops]; seq = "".join(x for x in seq if x)
n_eswap = sum(1 for l in (Path(__file__).resolve().parent.parent / "queue-r17.txt").read_text().splitlines() if l.startswith("!eswap "))
# the first eswap may come before any run (the owner's file is still on the console then: nothing to restore)
ok(seq.count("E") == n_eswap and re.fullmatch(r"R?E?(RE)+R+", seq) is not None, "owner file restored before every eswap after the first run and after the last block (%d eswaps; ops: %s)" % (n_eswap, seq))
res = (W / "outq/results.txt").read_text().splitlines(); ids = [l.split()[0] for l in res if re.match(r"^\S+\s+(done|CUT )", l)]
ok(ids == runs, "results.txt has one summ line per run, in order (%d)" % len(ids))
ok(all(" done " in l for l in res if re.match(r"^\S+\s+(done|CUT )", l)), "every run ended with its script exit line (no STALLED)")
ok(not (fake / "violations.txt").exists(), "no operation outside np-player.txt, log.txt, route .txt files and app/CDXMP0001/eboot.bin" + ("" if not (fake / "violations.txt").exists() else ": " + (fake / "violations.txt").read_text()[:300]))
ok(not [l for l in qleft.read_text().splitlines() if l.strip() and not l.startswith("#")], "queue.txt has no entries left")
print("RESULT:", "FAILED %d" % len(fails) if fails else "ALL PASS"); sys.exit(1 if fails else 0)
