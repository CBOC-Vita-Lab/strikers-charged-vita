# Strikers console relay

Lets the coordinating session run the Vita test title (CDXMP0001) in short rounds: queue a run, read its log, decide the next run, swap in
a newer executable. It replaces a long fixed `session.sh` sitting with many small jobs. It is the Strikers counterpart of Galaxy's relay.

## Owner: starting it (once per sitting)
1. Vita on its charger, awake, VitaCompanion running (the plug-in that gives FTP 1337 and commands 1338), no game open.
2. Close Galaxy's relay (its window) if it is open. Both drive the same Vita: this one refuses to start while `vita_relay.py` runs
   and stops taking jobs if it appears later.
3. First time only: put the Vita's address on the first line of `vita-address.txt` next to `strikers_relay.py` (the file is git-ignored,
   never committed), or start with `VITA=<address>`.
4. Double-click `Start Strikers Relay.command`. Leave the window open. Ctrl+C stops it. It restarts itself when the script changes.

Folders next to the script (`relay-r17/`, one folder per sitting: `STRIKERS_SITTING=<name>` picks it, a new sitting starts with a new name so no old owner copy or finished job is reused; not committed): `inbox/` (jobs in), `outbox/<id>/` (results: `result.json` + files), `staging/`
(files to put), `done/` (finished jobs), `state/` (the owner's np-player.txt kept once, executable name), `relay.log`.

## Session: using it
    python3 mkjobs.py jobs r17-sitting.txt --ids d1,lock60tour [--end-with r17] [--hold]
    python3 mkjobs.py results            # analyse.py on the pulled logs + one "id: pass" / "id: INVALID (why)" line per run
    python3 mkjobs.py ab d1 lock60tour   # A/B table per scene (fps, p50/p95/p99 ms, deltas against the first)
    python3 mkjobs.py release [NUM]      # take parked jobs (<num>-<id>.json.hold)
`jobs` stages the kit's env/ (env-fb/ for fast-boot runs), routes and `versions/<name>/eboot.bin` into `staging/`, writes one batch
job per run (an eboot put first when the list's `eswap` calls for another executable, then the run), numbered from 5000 up, and a
`restore` job last in every batch (`"always": true`: it runs even if an earlier step failed). `--unit A` takes the runs of a
`# ==== A` block; `--assume-eboot NAME` says the Vita already has it; `--end-with NAME` puts the plain executable back after the runs.
A job file named `*.json.hold` is parked and ignored until renamed to `*.json`.

Actions (a job is `{"action": ..., ...}`): `status`, `ls`, `get`, `put`, `launch`, `close`, `run`, `restore`, `pull_logs` (lean; `"all": true`
adds env.txt.* and log.txt.prev; closes the game first), `eswap` (an executable alone, read back, skipped when the Vita has it),
`shot` (not for judging pictures), `vc`, `batch`, `sleep`. Every result carries per-step times.

### The run job = session.sh's `run` unit
owner np-player.txt kept once -> route put and read back by hash -> env put as `np-player.txt` and read back -> old `log.txt` removed
(verified gone) -> old shots removed -> `np-settings-open` if the run has settings -> crash dump names -> quit, launch (one relaunch
if no log appears in 75 s) -> wait for `script exit` / settings done, a stall (no new log bytes for `stall_s`, 90 s; 150 s for the
stability routes) or the time cap (`cap_s`, default 2 x estimate + 120 s) -> **close the game** -> pull `log.txt` (sliced from the last
build header), shots, new crash dump names, `gpucrash.sgx` -> `status.txt` (done / stalled / timeout / nolaunch). `est_s`,
`first_poll_s`, `poll_s`, `cap_s` can be set in the job to make a round shorter.

## Hard limits (enforced in code, tested)
Only `ux0:data/mscharged-perf-test/` and `ux0:app/CDXMP0001/`: reads there (plus `ux0:data/psp2core-*.psp2dmp` crash dumps of this title,
and VitaCompanion's own `plane0.bmp` for `shot`); writes only `<name>.txt` files directly in the data root (and `np-settings-open`),
and `eboot.bin` (+ its one backup `eboot.before-claude-relay.bin`) of the test title. No other app (MSCN00003), save, `ux0:tai`, `ur0:`.
Commands to the Vita: `version`, `help`, `quit CDXMP0001`, `launch CDXMP0001`, and `vc` verbs press / release / wait / screen / unlock /
nosleep / capture. Never destroy, quit all, reboot, a package install. Deletes only what session.sh replaces: `log.txt`, `np-player.txt`
(restore, when there was none), `np-settings-open`, `gpucrash.sgx`, files of `shots/`. An eboot put reads the old one first (a failed
read stops before anything is written), backs it up once with readback, puts, reads back, retries, and puts the old one back if all fail.
Retries on 426 / timeouts. Galaxy's relay running = refuse.

## Cannot do (yet)
Shader-pack swaps (`swap` lines of the lists; `shadercache/`), the two-Vita LAN legs, copying big crash dumps (get them by name, one job),
FASTBOOT is chosen by mkjobs (from the list's `fb-` routes), not by the relay; `stage` of every route at once (routes go with each run).
Screenshots through the relay are torn: never judge a picture from them; the owner looks at the screen.

## Tests
    python3 -m unittest test_relay test_mkjobs      # fake VitaCompanion (fake_vita.py), every action and every limit
    python3 -m unittest test_mutants                # 15 broken copies of the relay, each must fail test_relay (several minutes)
