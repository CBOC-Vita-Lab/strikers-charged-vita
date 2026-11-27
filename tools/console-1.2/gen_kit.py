#!/usr/bin/env python3
"""Write the kit's env files (env/<run>.txt, lan/*.txt) and plan.json from matrix.py.

    python3 tools/console-1.2/gen_kit.py             # (re)write env/, lan/ and plan.json
    python3 tools/console-1.2/gen_kit.py --check     # exit 1 if the committed files differ from what matrix.py gives
    python3 tools/console-1.2/gen_kit.py --plan      # print the session as a table with the minute estimate (markdown)
    python3 tools/console-1.2/gen_kit.py --plan --measured session-times.csv    # the same with measured run times
    python3 tools/console-1.2/gen_kit.py --list [--tier 2]    # the session as tab-separated lines for session.sh: swap<TAB>VERSION / run<TAB>ID<TAB>ROUTE<TAB>MINUTES<TAB>VERSION
    python3 tools/console-1.2/gen_kit.py --list --tier 0      # the core session alone (session.sh core)
    python3 tools/console-1.2/gen_kit.py --list --tier 3 --staged    # everything, tier by tier: core, tier 1, tier 2, add-ons (session.sh full)
    python3 tools/console-1.2/gen_kit.py --plan --tier 0      # the core session's blocks with their minutes

Every env file is the np-player.txt of one run. Paths in them are the isolated test app's private root (ux0:data/mscharged-perf-test/),
the only place the isolated build accepts a script from.
"""
import argparse
import csv
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import matrix as M  # noqa: E402
import night as N  # noqa: E402
import sitting as SIT  # noqa: E402

PRIVATE = "ux0:data/mscharged-perf-test"
ROUTE_FILE = {"worstgpu": "worstgpu.txt", "tour": "tour.txt", "mega": "mega.txt", "look": "look.txt", "pipe": "pipe.txt", "ctrl": "ctrl.txt", "replay": "replay.txt",
              "ctour": "ctour.txt", "cpipe": "cpipe.txt", "clava": "clava.txt", "crep": "crep.txt",
              "ntour": "ntour.txt", "npipe": "npipe.txt", "nlava": "nlava.txt", "cstab": "cstab.txt", "cintro": "cintro.txt", "cend": "cend.txt",
              "settings": "look.txt",
              "gsvice": "fb-vice-goal.txt", "gsdump": "fb-dump-goal.txt", "gslava": "fb-lava-goal.txt", "megafull": "fb-mega-full.txt",
              "sand": "fb-sand.txt", "pause": "fb-pause.txt", "live19": "fb-live19.txt", "live19c": "fb-live19-crowd.txt", "live19r": "fb-live19-rear.txt", "live19l": "fb-live19-leave.txt",
              "onhub": "on-hub.txt", "onconn": "on-conn.txt", "onquick": "on-quick.txt", "onranked": "on-ranked.txt", "onlists": "on-lists.txt",
              "onhostwait": "on-hostwait.txt", "lanwifi": "lan-wifi-wait.txt", "lanadhoc": "lan-adhoc-wait.txt"}   # (the goal routes are fast-boot only: env-fb/ names the same file)     # (the Settings run quits from the Settings screen before the game starts; the script is never reached)


# The 1.3 stale-settings guard (native/platform/vita/src/np_settings_guard.cpp) runs in every player build, the isolated test app included, on the np-player.txt it finds. A file
# without this stamp is "an older file": the guard comments out MSC_DYNRES=0, MSC_VGX_MSAA=0, MSC_VITA_RENDER=..., MSC_DYNRES_MAX, MSC_DYNRES_FG=0 (41 of these env files name one)
# and the run would silently measure the defaults. The stamp is read from the first eight lines, so it goes on the second (test_kit.py reads the run id from the first).
STAMP = "# np-player.txt written by 1.3.0 build console-kit (the kit's files are not old settings files: the stale-settings guard leaves them as they are)"


def env_text(entry):
    lines = ["# console-1.2 run %s: %s" % (entry["id"], entry["title"]),
             STAMP,
             "# tier %d, group %s, route %s, version payload %s, role %s" % (entry["tier"], entry["group"], entry["route"], entry["version"], entry["role"]),
             "# %s" % entry["why"],
             "# ---- route and logging: the same in every run ----",
             "MSC_INPUT_SCRIPT=%s/%s" % (PRIVATE, ROUTE_FILE[entry["route"]])]
    named = {k for k, _ in (entry["env"] or [])}
    lines += ["%s=%s" % kv for kv in M.COMMON if kv[0] not in named]   # (a run that names a common key, e.g. its clock policy, says it once)
    if (entry["tier"] in (0, 3) and entry["role"] in ("cref", "core", "ab", "manual")) or entry["role"] in ("nref", "n60", "n11", "nab"):
        lines += ["%s=%s" % kv for kv in M.CORE_COMMON if kv[0] not in named]   # (sweepbg0 names MSC_HITCH_MS=45: said once)
    if entry.get("settings"):
        lines.append("# ---- session.sh puts np-settings-open (the Settings screen's test hook) next to this file: %s" % entry["settings"])
    lines.append("# ---- under test: nothing here means the default ----")
    if entry["env"]:
        lines += ["%s=%s" % kv for kv in entry["env"]]
    else:
        lines.append("# (no switch)")
    return "\n".join(lines) + "\n"


def env_fb_text(entry):
    """env-fb/<id>.txt: the same run on the fast-boot route (the first line of the route, `bootmatch <spec>`, starts the match at the title screen; saves are not written)."""
    fb = M.fastboot_route(entry)
    old = "MSC_INPUT_SCRIPT=%s/%s" % (PRIVATE, ROUTE_FILE[entry["route"]])
    new = "MSC_INPUT_SCRIPT=%s/%s.txt" % (PRIVATE, fb)
    text = env_text(entry)
    assert old in text, entry["id"]
    text = text.replace(old, "# fast boot (test only): the route starts the match at the title screen, so the menu, intro and stadium-select time is not measured or spent; the old route was %s\n%s" % (ROUTE_FILE[entry["route"]], new))
    return text


def lan_text(leg, role):
    lines = ["# console-1.2 %s, %s console: %s" % (leg["id"], role, leg["title"]),
             STAMP,
             "# ---- route and logging ----",
             "MSC_INPUT_SCRIPT=%s/%s" % (PRIVATE, M.LAN_SCRIPTS[role])]
    lines += ["%s=%s" % kv for kv in M.LAN_COMMON]
    lines.append("# ---- the link: Settings > Multiplayer Link SAME WI-FI writes this line (both Vitas on the same access point) ----")
    lines += ["%s=%s" % kv for kv in M.LAN_ROLE_ENV[role]]
    lines.append("# ---- under test (the host decides the delay) ----")
    extra = leg["host_env"] if role == "host" else leg["join_env"]
    lines += ["%s=%s" % kv for kv in extra] or ["# (no switch)"]
    return "\n".join(lines) + "\n"


def outputs():
    files = {}
    for r in M.RUNS + N.RUNS:
        files["env/%s.txt" % r["id"]] = env_text(r)
    for r in M.RUNS:
        if M.fastboot_route(r):
            files["env-fb/%s.txt" % r["id"]] = env_fb_text(r)
    for leg in M.LAN_LEGS:
        for role in ("host", "join"):
            files["lan/%s-%s.txt" % (leg["id"], role)] = lan_text(leg, role)
    files["r17-sitting.txt"] = SIT.sitting_text()
    files["r17-sitting-extra.txt"] = SIT.sitting_text(extra=True)
    files["r17-lan.txt"] = SIT.lan_text()
    plan = {
        "private_root": PRIVATE,
        "title_id": "CDXMP0001",
        "budget_min": M.BUDGET_MIN,
        "route_min": M.ROUTE_MIN, "swap_min": M.SWAP_MIN, "setup_min": M.SETUP_MIN, "lan_leg_min": M.LAN_LEG_MIN,
        "runs": [{k: r[k] for k in ("id", "title", "route", "version", "tier", "role", "group", "block", "settings")} | ({"night": N.meta(r)} if "night" in r else {}) | {"env": dict(r["env"]), "file": "env/%s.txt" % r["id"], "minutes": round(M.minutes(r), 2)} | ({"fb": {"route": M.fastboot_route(r), "file": "fastboot/routes/%s.txt" % M.fastboot_route(r), "env": "env-fb/%s.txt" % r["id"], "minutes": round(M.minutes(r, True), 2)}} if "night" not in r and M.fastboot_route(r) else {}) for r in M.RUNS + N.RUNS],
        "fastboot_route_min": M.FB_ROUTE_MIN,
        "night": {"budget_min": N.BUDGET_MIN, "phases": [{"id": p, "title": t, "until_min": u} for p, t, u in N.PHASES], "warm_records": N.WARM_RECORDS, "worth_fps": N.WORTH_FPS,
                 "baseline_refs": {"b": [r["id"] for r in N.RUNS if r["role"] == "nref"], "l": [r["id"] for r in N.RUNS if r["role"] == "n60"], "o": [r["id"] for r in N.RUNS if r["role"] == "n11"]}},
        "session": [{"step": "swap", "version": i} if k == "swap" else {"step": "run", "id": i["id"]} for k, i in M.session_order(1)],
        "session_tier2": [{"step": "swap", "version": i} if k == "swap" else {"step": "run", "id": i["id"]} for k, i in M.session_order(2)],
        "session_core": [{"step": "swap", "version": i} if k == "swap" else {"step": "run", "id": i["id"]} for k, i in M.session_order(0)],
        "session_full": [{"step": "swap", "version": i} if k == "swap" else {"step": "run", "id": i["id"]} for k, i in M.session_order(3, staged=True)],
        "refs": [r["id"] for r in M.RUNS if r["role"] == "ref"],
        "core_refs": [r["id"] for r in M.RUNS if r["role"] == "cref"],
        "lan": [{"id": leg["id"], "host": "lan/%s-host.txt" % leg["id"], "join": "lan/%s-join.txt" % leg["id"]} for leg in M.LAN_LEGS],
    }
    files["plan.json"] = json.dumps(plan, indent=1, sort_keys=False) + "\n"
    return files


def read_measured(path):
    out = {}
    if path:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                try:
                    out[row["id"]] = float(row["minutes"])
                except (KeyError, ValueError):
                    pass
    return out


def core_text(measured):
    out = ["Core session (`session.sh core`; tier 0, the first part of `all` and `full`), by block:", "",
           "| block | minutes | runs |", "| --- | --- | --- |"]
    blocks = M.core_blocks(measured)
    for name, minutes, ids in blocks:
        out.append("| %s | %.1f | %s |" % (name, minutes, ", ".join(ids) or "check, stage, first launch"))
    out.append("")
    out.append("Core total %.1f min (budget %.0f)%s" % (sum(b[1] for b in blocks), M.CORE_BUDGET_MIN, " - measured times used where known" if measured else " - estimates, see matrix.py"))
    return "\n".join(out)


def plan_text(measured):
    total, rows = M.estimate(1, True, measured)
    by_id = {r["id"]: r for r in M.RUNS}
    out = [core_text(measured), "", "The session of `all` (the core session, then tier 1):", "",
           "| # | step | what | route | minutes | cumulative |", "| --- | --- | --- | --- | --- | --- |"]
    cum, n = 0.0, 0
    for label, minutes in rows:
        cum += minutes
        r = by_id.get(label)
        what = r["title"] if r else ("LAN match, both Vitas" if label.startswith("lan-") else ("first launch and checks" if label == "setup" else "push eboot.bin and shader pack, read back"))
        route = r["route"] if r else ""
        n += 1
        out.append("| %d | %s | %s | %s | %.1f | %.1f |" % (n, label, what, route, minutes, cum))
    out.append("")
    out.append("Total %.1f min (budget %.0f)%s" % (total, M.BUDGET_MIN, " - measured times used where known" if measured else " - estimates, see matrix.py"))
    out.append("")
    out.append("Tier 2 (not in the session; env files are ready):")
    for r in M.RUNS:
        if r["tier"] == 2:
            out.append("- %s (%s, %.1f min): %s" % (r["id"], r["route"], M.minutes(r), r["title"]))
    out.append("")
    out.append("Add-ons (tier 3, the end of `full`; `session.sh addon` also runs the ones marked by hand):")
    for r in M.RUNS:
        if r["tier"] == 3:
            out.append("- %s (%s, %.1f min%s): %s" % (r["id"], r["route"], M.minutes(r), ", needs a plug-in set by hand" if r["role"] == "manual" else "", r["title"]))
    full, _ = M.estimate(3, True, measured, staged=True)
    out.append("")
    out.append("`full` (core, tier 1, tier 2, the automatic add-ons, the two LAN legs): %.0f min, about %.1f hours" % (full, full / 60))
    return "\n".join(out)


def print_plan(measured):
    print(plan_text(measured))


def night_text(measured=None, keys=None):
    """The night's plan as a table: phase, run, route, minutes, cumulative against the phase's deadline."""
    total, rows, dropped = N.estimate(keys, measured)
    by_id = {r["id"]: r for r in N.RUNS}
    titles = dict(N.PHASES and [(p, t) for p, t, _ in N.PHASES])
    until = {p: u for p, _, u in N.PHASES}
    out = ["The night (`session.sh night`, about 8 hours; each phase is left when its deadline has passed, so a slow night loses the end):", "",
           "| phase | what | deadline (min) | planned end (min) | runs | minutes |", "| --- | --- | --- | --- | --- | --- |"]
    ends, count, mins = N.phase_ends(rows), {}, {}
    for label, ph, m, cum in rows:
        count[ph] = count.get(ph, 0) + (1 if label in by_id else 0)
        mins[ph] = mins.get(ph, 0.0) + m
    for p, t, u in N.PHASES:
        if p in ends:
            out.append("| %s | %s | %.0f | %.0f | %d | %.0f |" % (p, t, u, ends[p], count.get(p, 0), mins.get(p, 0.0)))
    out += ["", "Total %.0f min planned (budget %.0f)%s." % (total, N.BUDGET_MIN, " - measured times used where known" if measured else ", estimates"), "",
            "| # | phase | run | route | version | minutes | cumulative |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for n, (label, ph, m, cum) in enumerate(rows, 1):
        r = by_id.get(label)
        out.append("| %d | %s | %s | %s | %s | %.1f | %.1f |" % (n, ph, label, r["route"] if r else "", r["version"] if r else "", m, cum))
    if dropped:
        out += ["", "Left out of this tree's list because no source reads their switch yet (the branch is named; they join the list by themselves once it is merged):", ""]
        out += ["- %s: %s" % (rid, ", ".join("%s (%s)" % kb for kb in miss)) for rid, miss in dropped]
    second = [r for r in N.RUNS if N.phase_of(r) == "p5"]
    if second:
        out += ["", "Second night (env files ready, `session.sh list` with your own list): " + ", ".join("%s (%s)" % (r["id"], r["route"]) for r in second)]
    return "\n".join(out)


README_BEGIN, README_END = "<!-- plan:begin (generated by gen_kit.py) -->", "<!-- plan:end -->"
NIGHT_BEGIN, NIGHT_END = "<!-- night:begin (generated by gen_kit.py) -->", "<!-- night:end -->"


def readme_with_plan(text):
    a, b = text.find(README_BEGIN), text.find(README_END)
    if a < 0 or b < a:
        return None
    text = text[:a] + README_BEGIN + "\n" + plan_text({}) + "\n" + text[b:]
    a, b = text.find(NIGHT_BEGIN), text.find(NIGHT_END)
    if a >= 0 and b > a:
        text = text[:a] + NIGHT_BEGIN + "\n" + night_text(None, N.source_keys()) + "\n" + text[b:]
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--plan", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--tier", type=int, default=1)
    ap.add_argument("--staged", action="store_true", help="tier by tier (core, 1, 2, 3) instead of the RUNS order")
    ap.add_argument("--manual", action="store_true", help="include the runs that need a plug-in set by hand")
    ap.add_argument("--night", action="store_true", help="the overnight session (night.py): with --list the lines session.sh night runs, with --plan its table")
    ap.add_argument("--with-pending", action="store_true", help="--night --list: keep the runs whose switch this tree's source does not read yet")
    ap.add_argument("--only-tier", type=int, help="list only the runs of this tier (session.sh addon: 3)")
    ap.add_argument("--sitting", action="store_true", help="print the r15 consolidated sitting list (r17-sitting.txt) and its minutes table (sitting.py)")
    ap.add_argument("--measured", help="CSV with columns id,minutes (session-times.csv from session.sh)")
    args = ap.parse_args()
    if args.sitting:
        print(SIT.sitting_text(), end="")
        print(SIT.table_text(), file=sys.stderr)
        return 0
    if args.night and args.list:
        steps, dropped = N.schedule(with_pending=args.with_pending)
        until = {p: u for p, _, u in N.PHASES}
        print("night\t%d\t%d" % (N.BUDGET_MIN, N.DEFAULT_END_MIN))
        for rid, miss in dropped:
            print("# dropped\t%s\t%s" % (rid, " ".join("%s(%s)" % kb for kb in miss)))
        phase = None
        for kind, item in steps:
            if kind == "swap":
                print("swap\t%s" % item)
                continue
            if N.phase_of(item) != phase:
                phase = N.phase_of(item)
                print("phase\t%s\t%d" % (phase, until[phase]))
            print("run\t%s\t%s\t%s\t%s\t%s" % (item["id"], item["route"], round(M.minutes(item), 2), item["version"], phase))
        print("phase\tend\t%d" % N.DEFAULT_END_MIN)
        return 0
    if args.night and args.plan:
        print(night_text(read_measured(args.measured), N.source_keys()))
        return 0
    if args.list:
        for kind, item in M.session_order(args.tier, args.staged, args.manual):
            if args.only_tier is not None and (kind == "swap" or item["tier"] != args.only_tier):
                continue
            if kind == "swap":
                print("swap\t%s" % item)
            else:
                print("run\t%s\t%s\t%s\t%s" % (item["id"], item["route"], round(M.minutes(item), 2), item["version"]))
        return 0
    if args.plan:
        if args.tier == 0:
            print(core_text(read_measured(args.measured)))
        else:
            print_plan(read_measured(args.measured))
        return 0
    files = outputs()
    bad = 0
    readme = HERE / "README.md"
    if readme.exists():
        updated = readme_with_plan(readme.read_text())
        if updated is not None:
            if args.check:
                if updated != readme.read_text():
                    print("DIFFERS: README.md (plan table)")
                    bad = 1
            else:
                readme.write_text(updated)
    for name, text in files.items():
        path = HERE / name
        if args.check:
            if not path.exists() or path.read_text() != text:
                print("DIFFERS: " + name)
                bad = 1
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
    if args.check:
        known = set(files)
        for sub in ("env", "env-fb", "lan"):
            for path in (HERE / sub).glob("*.txt"):
                if "%s/%s" % (sub, path.name) not in known:
                    print("STALE: %s/%s" % (sub, path.name))
                    bad = 1
    else:
        print("wrote %d files (%d runs, %d night runs, %d LAN files, plan.json)" % (len(files), len(M.RUNS), len(N.RUNS), 2 * len(M.LAN_LEGS)))
    return bad


if __name__ == "__main__":
    sys.exit(main())
