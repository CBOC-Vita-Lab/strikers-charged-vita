# Before the night: what must be true (owner's checklist)

Tick every line. A wrong line costs hours of the night; the script cannot fix most of them while you sleep.

## On the Vita
- [ ] **Charger plugged in**, battery above 50%.
- [ ] **Auto-Standby off** (Settings > Power Save Settings > Auto-Standby: Do not standby). Also screen-off timer: Do not turn off. Sleep is not scriptable; a standby ends the night.
- [ ] **vitacompanion running** (FTP on port 1337, command port 1338). `session.sh check` must answer "installed".
- [ ] **Clock: ARM 500 / GPU 222 (owner-approved, as Galaxy)**: the player build asks for it itself 1 s after the start; a PSVshellPlus 500 profile is fine, except during clock444 / clock444d where the title's profile must not lock 500. Keep the Vita on the charger. The fourth core (x-core3 runs, tier 4) needs the CapUnlocker plugin; without it those two runs are skipped.
- [ ] **Test app CDXMP0001 installed.** The installed game MSCN00003 is never touched. The script only writes in `ux0:data/mscharged-perf-test`, `app/CDXMP0001/eboot.bin` and its `shadercache`.
- [ ] **Game data in place** at `ux0:data/mscharged/game`.
- [ ] **Free space:** at least 2 GB on ux0 (logs, screenshots, up to 3 crash dumps of up to 96 MB each).
- [ ] **No other app open**, no other plugin that draws on screen or sleeps the console. Wi-Fi in range of the router, router not set to sleep.
- [ ] Leave the Vita on the **LiveArea or the game** (not in a menu); do not touch it afterward.

## On the computer
- [ ] `versions/1.1` and `versions/1.3` prepared with `prepare-versions.py` (1.1 built from commit 5421c34 as the isolated build). `session.sh swap 1.1` and back must work.
- [ ] **The build and the source tree match** the tree `gen_kit.py` ran in (the plan drops runs whose switch the source does not read: look at `out/not-in-this-build.txt` at the start).
- [ ] `python3 tools/console-1.2/test_kit.py` passes and `gen_kit.py --check` is clean.
- [ ] Laptop does not sleep; run in `tmux`/`screen`.

## Start and stop
- `VITA_IP=<ip> bash tools/console-1.2/session.sh night` (a minute of checks, then about 8 hours).
- Stop at any time with Ctrl-C: the reports are still written. Run the same command to resume within 14 hours (the night clock is kept; finished runs are skipped).
- Morning: read `out/MORNING.md` (one page, plain words), then `out/ISSUES.md`, `out/looks-night.html` (pictures).

## Things to know
- Crash dumps (`psp2core-*.psp2dmp`) are **copied read-only** into `out/<run>/dumps/` and never deleted on the console (up to 3 per run, 96 MB each; `DUMPS=0` turns this off). This replaces the old statement that the script never opens dump files.
- Sleep/resume is a manual test: close the lid for a minute in the morning, wake, play one minute.
- Numbers are measured at 960x544 with a 600-frame warm-up and 300-frame windows; differences smaller than the same-night noise (printed in MORNING.md) are not differences.
