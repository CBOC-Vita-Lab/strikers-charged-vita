# Diagnostics And Logging

Release builds (`NP_LOGGING=0`) write no log and record nothing unless asked. Everything below
is for development builds and console measurements.

## Runtime log (`NP_LOGGING=1` builds)

- The runtime log is `ux0:data/mscharged/log.txt`. At 2 MiB it rotates to `log.txt.old`.
- stdout and stderr (C stdio and iostreams) are routed into it, and the last lines are kept for
  the on-screen console (hold SELECT+START, then D-pad down).
- A logging thread only copies into a 64 KiB buffer. A writer thread at the lowest user
  priority writes it to the card every 250 ms, or at once when 8 KiB are waiting. A memory-card
  write takes milliseconds, so logging no longer stalls the game or render thread, and it no
  longer changes the timings it reports. Fatal paths, `Flush()`, start-up (synchronous until
  the first frame) and the lab's file switches still write on the caller's thread, in order.
- If the writer thread cannot be created, the log falls back to writing on the caller's
  thread, as before.

## Performance recorder (`MSC_TOUCH_DIAG=1`, any build)

The recorder is an in-memory event recorder for frame-pipeline timing. It records:

- input polls and gestures;
- simulation steps;
- the game-to-render handover;
- the renderer back end;
- display callbacks and display gates;
- task timings, shader-cache activity and system samples;
- Mega Strike phases.

| Line in `np-player.txt` | Effect |
| --- | --- |
| `MSC_TOUCH_DIAG=1` | Turn it on. The 4.4 MiB of banks are allocated only then |
| `MSC_PERF_DETAIL=summary` | Keep only pipeline events, and spend Mega banks on the active catch |

How it records:

- It fills up to four banks: two of ordinary play (up to 30 s each) and two of Mega Strikes
  (up to 60 s each).
- A bank closes when it is full, when its time is up, or at a scene change.
- It is written to the card only at a safe point, such as the pause menu or a menu screen. It
  is never written during play.

It writes:

- `perf-<session>-<bank>-<mode>.bin`, the events with a CRC;
- `perf-status-<session>-<n>.txt`, the bank states and phase counters.

To time the GPU calls themselves (`sceGxmFinish`, `NotificationWait`, `DisplayQueueAddEntry`,
`BeginScene` and `EndScene`), build with `NP_DIAG_GPU=1`. That adds the matching `--wrap`
options. Use a clean `BUILD_ROOT` when you switch it on or off, because make does not track
compiler flags.

## Reading the recordings

```sh
python3 tools/diag/perf-report.py perf-*.bin          # report per bank
python3 tools/diag/perf-report.py --json perf-*.bin   # the same as JSON
```

For each bank, the report shows:

- mode, length and clocks;
- display cadence, in Hz and frame interval;
- frame handover to display gate;
- renderer back-end time and draws per frame;
- GPU-call times;
- per-task times;
- event counts.

Warnings mark dropped events, a closing race, or a damaged file (CRC). Banks are read one at a
time, and frames are never joined across files. All times are CPU wall clock on the console,
not GPU timestamps, scanout or finger-to-screen latency.

`tests/test_diag_tools.py` records a bank on the host with the real recorder and checks the
report against known timings.

## Other switches

| Line | Effect |
| --- | --- |
| `MSC_VITA_DISPLAY_BUFFERS=2` | Double instead of triple buffering (latency A/B; default 3) |
| `MSC_VITA_RENDER=WxH` | Internal picture size (diagnostic, e.g. `320x224` for a fill-rate A/B) |
| `MSC_FX_THIN=0/2`, `MSC_FX_ZERO=0` | Mega Strike particle drawing (see [Technical notes](TECHNICAL.md)) |

Emulator timing (Vita3K) is not console performance evidence. Use it for correctness only.

The A/B tests planned for the next optimisations are in [Console tests](CONSOLE-TESTS.md).
