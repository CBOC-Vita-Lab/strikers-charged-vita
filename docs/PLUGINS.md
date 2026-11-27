# Optional Performance Plug-ins

Nothing on this page is required to play. **The game works without any plug-in, never installs or downloads one, and no plug-in file is part of the VPK or this repository.** Everything below is optional and installed at your own discretion.

A kernel plug-in runs inside the Vita's system software (`ur0:tai/config.txt`). Always keep a backup of `config.txt` and remember you can hold **L** at boot to start without plug-ins if anything ever goes wrong.

## Recommended Plug-ins for 2.0

| Plug-in | What it does for Mario Strikers Charged 2.0 | Verdict |
| --- | --- | --- |
| **[PSVshell](https://github.com/Electry/PSVshell)** / **[PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus)** | Unlocks **500 MHz** CPU (with 222 MHz GPU / 222 MHz BUS / 166 MHz XBAR) | **Recommended** for best 60 fps headroom |
| **[CapUnlocker](https://github.com/GrapheneCt/CapUnlocker)** (GrapheneCt) | Unlocks the Vita's 4th CPU core for games | **Recommended** — 2.0 detects and uses it automatically with built-in safety guards |
| Storage-speed plug-ins (e.g., Turbo Storage) | Faster bulk SD2Vita copies | **Not recommended** for this game (see below) |

### PSVshell / PSVshellPlus: 500 MHz CPU Overclock

Without a clock plug-in, VitaOS caps games at **444 MHz** CPU / **222 MHz** GPU & bus (which Mario Strikers Charged 2.0 requests automatically). Installing **PSVshell** or **PSVshellPlus** lets you raise the CPU clock to **500 MHz** (alongside **222 MHz GPU**, **222 MHz BUS**, and **166 MHz XBAR**) for extra 60 fps headroom in demanding stadiums.

Version 2.0 measures the live CPU clock at startup and automatically leaves your custom **PSVshell** / **PSVshellPlus** profile untouched.

### CapUnlocker: Automatic 4th CPU Core Support

A PS Vita has four CPU cores: three for games and one reserved for system tasks unless unlocked by **CapUnlocker** (`CapUnlocker.skprx`, MIT license, https://github.com/GrapheneCt/CapUnlocker).

In Version 2.0, **4th-core support is enabled automatically** (`MSC_VITA_CORE3=1`, `MSC_VITA_CORE3_WORK=3` — no config file editing required):
- At startup, the game runs a quick probe on Core 3. Without CapUnlocker (or if the core responds slowly), **nothing moves** and the game runs normally on three cores.
- When Core 3 is available, the game offloads audio output, background jobs, and front-end decoding to Core 3 (keeping file/save I/O off Core 3, monitoring Core 3 availability continuously, and automatically stepping back to 3 cores during online/LAN matches).
- Need to disable 4th-core usage while keeping CapUnlocker installed for other games? Add `MSC_VITA_CORE3=0` to `ux0:data/mscharged/np-player.txt`.

**Install** (Ensō or HENkaku with taiHEN):

1. Download `CapUnlocker.skprx` from the *Releases* page of the project above. Check that it comes from that page; this repository does not carry a copy.
2. Copy it to `ur0:tai/CapUnlocker.skprx`.
3. Make a copy of your config first (`ur0:tai/config.txt` to `config.txt.bak`). If `ux0:tai/config.txt` exists, taiHEN reads that one instead and ignores the `ur0:` one:
   edit the file your console really uses.
4. Under the `*KERNEL` heading add one line:

   ```
   *KERNEL
   ur0:tai/CapUnlocker.skprx
   ```
5. Save, reboot the console.

**Safety notes**

- CapUnlocker patches thread/app manager checks in system memory; use only on supported firmwares listed on its Releases page.
- Keep a backup of `config.txt` and remember holding **L** while booting disables plug-ins.
- **Remove:** delete the line from `config.txt` (or restore your backup) and reboot.

### Storage-speed plug-ins: why this draft does not recommend one

One such plug-in is Turbo Storage by Juan Garcia Alvite (MIT licence, https://github.com/juanjsebgarcia/vita-turbo-storage, first published 26 October 2026, firmware 3.60 to 3.74). It makes SD2Vita and memory card transfers faster by handing the system's storage driver a buffer that needs no cache maintenance and copying the data afterwards. Its gains are
reported for large copies (installing VPKs, copying folders). This game reads its data in a different way and the plug-in author's own figures say a game launch spent
about 4% of its time waiting for storage. It also touches the part of the system that writes your saves.

- Early release, tested by its author on one console and one firmware.
- It reads files into a cached buffer and leaves the last data in the processor's cache; a program that hands a freshly loaded texture to the graphics chip without
  flushing could draw stale data. The author's own default is to switch itself off while a retail game is in front. This game's title id is not a retail one, so it would
  not be paused.
- A previous plug-in of this kind was withdrawn by its author over suspected data corruption.

If you still want it for copying files, install it from its author's page, keep a backup of your memory card, and turn its "Pause in Vita games" option on (the default)
or leave it off for play. The game never asks for it.

## Faster loading without any plug-in

These are in the game itself (1.3, `docs/TECHNICAL.md`): disc files are read with large positional reads instead of through a 1 KiB buffer (`MSC_DVD_DIRECT`), a failed
read is retried on a fresh handle (`MSC_DVD_RETRY`), and the next files in line can be opened ahead (`MSC_DVD_PREOPEN`, off).

## Reporting a problem

If you test a plug-in with this game, send `ux0:data/mscharged/log.txt` with the report and say which plug-ins and firmware version you run.
