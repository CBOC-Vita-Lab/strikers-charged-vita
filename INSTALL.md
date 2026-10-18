# Install

Use the matching `a8d4ad6` / `MSCN00003` private-preview package.
[Start Here](START-HERE.txt) is the short version.

## You Need

- A homebrew-enabled PS Vita and VitaShell.
- Your own Mario Strikers Charged **Wii USA Rev 1 (`R4QE01`)** dump.
- The matching ready-made native player VPK, application ID **`MSCN00003`**.
- Your legitimately obtained `libshacccg.suprx` in `ur0:data/`.
- About 3 GB free on the Vita for the extracted game, app and saves.

## Installation

1. [Prepare your own game](PREPARE-GAME.md) with the included one-command helper, then copy its
   `sys/` and `files/` folders into `ux0:data/mscharged/game/`. The preparation
   helper checks the revision and extracted files for you.
2. Extract the install ZIP on your computer and [check the download](CHECK-DOWNLOAD.md).
   Leave `Strikers-a8d4ad6-private-candidate.vpk` intact. Copy it through
   VitaShell USB or FTP to `ux0:VPK/` (create that folder if needed), then
   end the transfer. In VitaShell, highlight the VPK, use the confirm button
   (usually Cross), and confirm installation.
3. Open the Mario Strikers Charged bubble. Use [Controls](CONTROLS.md) for
   the default layout and closer camera options.

Game data and the shader compiler module are not included.

## Folder Check

```text
ux0:data/mscharged/
  game/
    sys/
      fst.bin
      ...                 other system files from your dump
    files/
      Art/
      audio/
      ...                 other files from your dump
  np-data/
    NAND/                 native game saves
  np-player.txt           optional player-build settings
```

Startup shaders are already inside the VPK. Do not download or replace them
separately.

## Update And Back Up

An ordinary app update replaces the **VPK only**, not your data folder.

| Item | What To Do |
| --- | --- |
| Matching `MSCN00003` VPK | Install with VitaShell; do not unpack it over `ux0:app/`. |
| `game/` | Keep. Copy your own extraction only for a first install or a verified repair. |
| `np-data/NAND/` | Keep and back up. Never replace with bundled/test saves. |
| `np-player.txt` | Keep your settings; no replacement config is bundled. |
| Boot image and shader cache | Already inside the matching VPK; no manual replacement. |

Exit the game and back up `ux0:data/mscharged/`, especially `np-data/NAND/`
and `np-player.txt`. Install the updated **player** VPK with the same
`MSCN00003` ID, then check your settings and a saved game before discarding
the backup. Do not delete data, import someone else's NAND or overwrite
your saves as an update step.

Keep `np-player.txt`; an old lab `env.txt` is not this player's settings file.

## If It Does Not Start

Check the application ID, game revision and folder nesting first. In
particular, `game/sys/fst.bin` must not be under `game/game/` or `game/DATA/`.
For a missing shader-compiler error, check the `ur0:data/` requirement above.
No kernel-plugin change is part of this install.
Do not replace the app's shader cache with one from a different build.
Keep any log private until filenames, addresses and save details have been
reviewed. Never attach the original disc or save to an issue by default.
See [Known Issues](KNOWN-ISSUES.md) for build-specific notes.
