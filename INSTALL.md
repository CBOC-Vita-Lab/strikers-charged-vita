# Install

This is a preparation draft, not a released installer. The current app was
recovered read-only from a Vita, but the complete source and final release
package are not ready. Do not use an older lab or Codex VPK in its place.

## You Need

- A homebrew-enabled PS Vita and VitaShell.
- Your own Mario Strikers Charged **Wii USA Rev 1 (`R4QE01`)** dump.
- The matching native player VPK, application ID **`MSCN00003`**, once released.

## Installation

1. Prepare your own game's data on the computer. The result must contain
   `sys/` and `files/`, including `sys/fst.bin`. Copy those two folders into
   `ux0:data/mscharged/game/`, not into another nested game folder.
2. Check the release VPK against its supplied checksum, then install it with
   VitaShell.
3. Open the Mario Strikers Charged bubble. Use [Controls](CONTROLS.md) for
   the default layout and closer camera options.

The exact supported extraction procedure still needs to be checked against
the current source export. These steps are not yet a tested fresh-install
guide. Do not download a pre-extracted game pack to fill that gap.

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

The startup shader pack belongs inside the app's VPK at
`shadercache/bc447993.pack`. It is not a separate game-data download or a
manual file-copy step for players. The final package must include its
reviewed matching resources.

## Update And Back Up

Exit the game and back up `ux0:data/mscharged/`, especially `np-data/NAND/`
and `np-player.txt`. Install the updated **player** VPK with the same
`MSCN00003` ID, then check your settings and a saved game before discarding
the backup. Do not delete data, import someone else's NAND or overwrite
your saves as an update step.

The lab's `env.txt` is not the player's settings file. An old lab title or
recompiled build is not interchangeable with this native player release.

## If It Does Not Start

Check the application ID, game revision and folder nesting first. In
particular, `game/sys/fst.bin` must not be under `game/game/` or `game/DATA/`.
Do not replace the app's shader cache with one from a different build.
Keep any log private until filenames, addresses and save details have been
reviewed. Never attach the original disc or save to an issue by default.
