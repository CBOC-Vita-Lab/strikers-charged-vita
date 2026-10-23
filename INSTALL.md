# Manual Install: Windows, Mac Or Linux

**You do not need CBOC Vita Installer to install or play this game.**
The ready-made VPK runs on the Vita. Any of the computer platforms above can
prepare and copy the required files. Once installed, no computer is needed.
This route needs no Python, compiling or Mac-only software.

Use `Strikers-settings-20261019.vpk`, application ID `MSCN00003`.
[Start Here](START-HERE.txt) is the short version.

## You Need

- A homebrew-enabled PS Vita and VitaShell.
- Your own Mario Strikers Charged **Wii USA Rev 1 (`R4QE01`)** dump.
- The matching ready-made native player VPK, application ID **`MSCN00003`**.
- Your legitimately obtained `libshacccg.suprx` in `ur0:data/`.
- About 3 GB free on the Vita for the extracted game, app and saves.

## Installation

1. Download [the VPK](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/download/strikers-settings-20261019/Strikers-settings-20261019.vpk), or extract the [complete install ZIP](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/download/strikers-settings-20261019/Strikers-settings-20261019-install.zip) for the VPK plus guides and source. **Do not unzip the VPK.** [Download checks](CHECK-DOWNLOAD.md) are optional.
2. Prepare your own game's complete `sys/` and `files/` folders. If you only have an ISO/RVZ, follow [Dolphin's extraction steps](PREPARE-GAME.md#dolphin-no-python-needed). It is used only for extraction, not gameplay. Already extracted? Skip this step.
3. Use one of the transfer methods below. Copy both folders into `ux0:data/mscharged/game/`, and copy `Strikers-settings-20261019.vpk` into `ux0:VPK/`. Create missing folders. Do not copy the ISO/RVZ itself or the whole repository into `ux0:app/`.
4. Check for `ur0:data/libshacccg.suprx` in VitaShell. If it is missing, copy your legitimately obtained file there. **USB/card drives expose `ux0:`, not `ur0:`:** stage the file in `ux0:VPK/`, end the transfer, then use VitaShell's **Triangle > Copy**, open `ur0:data/`, and **Triangle > Paste**. Do not replace an existing working copy.
5. End the transfer. In VitaShell, open `ux0:VPK/Strikers-settings-20261019.vpk`, press the confirm button (usually Cross) and confirm installation. Launch the Mario Strikers Charged bubble. See [Controls](CONTROLS.md) for the layout.

Game data and the shader compiler module are not included.
Already have Strikers installed? Use [Update and Back Up](#update-and-back-up)
instead of replacing its data folder. Both installation routes use the same VPK.

## Choose A Transfer Method

**USB:** In VitaShell, press Start, set **SELECT button** to **USB**, and
choose the **USB device** that is already your `ux0:` storage (for example,
SD2Vita). Close settings, connect a data-capable USB cable and press Select.
Open the mounted drive in your computer's file manager. Its root represents
`ux0:`: use `data/mscharged/game/` and `VPK/`, not a new folder named `ux0`.
Safely eject the drive before ending USB mode.

**Wi-Fi / FTP:** Put both devices on the same trusted network. In VitaShell's
Start menu, set **SELECT button** to **FTP**, then press Select. In an FTP client
on your computer, connect to the exact address and port shown on the Vita;
choose FTP, not SFTP. Open `ux0:` and copy to the folders above. Keep VitaShell
open until every transfer finishes, then disconnect and end FTP mode.

**Memory card reader:** Only use an already configured, computer-readable card
that your Vita mounts as `ux0:` (such as its SD2Vita microSD). Fully power off
the Vita before removing it. Copy to `data/mscharged/game/` and `VPK/` at the
card's root, safely eject it, return it to the Vita and start VitaShell.
Do not format the card or change your storage configuration for this install.
If the computer cannot read it, use USB or FTP instead.

These transfers use [VitaShell](https://github.com/TheOfficialFloW/VitaShell),
not the optional CBOC desktop app. A copied VPK still needs the VitaShell
installation in step 5.

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
| App artwork and shader cache | Handled by the matching VPK; no manual replacement. |

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

## Optional Automated Setup

[CBOC Vita Installer](DESKTOP-INSTALL.md) does extraction, checks, transfer and
installation for you on Mac, Windows or Linux. No additional movie/image
conversion is required by this Strikers package; its current playback fixes
are in the VPK.
