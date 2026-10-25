# Prepare Your Game

> **Most people should use [CBOC Vita Installer](EASY-INSTALL.md) instead.** It does every step below for you: pick Mario Strikers Charged, choose your disc, press Install. Use this guide only if you can't or don't want to use it.

**This is the manual route; CBOC Vita Installer is not required.**
It can be done on Windows, macOS (Intel or Apple Silicon), or Linux. The game
runs on Vita, not in Dolphin. No computer is needed after installation.

Use your own **Wii USA Rev 1 (`R4QE01`)** dump. If you already have its
complete `sys/` and `files/` folders, skip extraction and [copy them to the
Vita](MANUAL-INSTALL.md#installation). Do not copy the ISO or RVZ itself.

## Dolphin: No Python Needed

1. Install [Dolphin](https://dolphin-emu.org/download/) on your computer and
   add the folder containing your own ISO or RVZ to its game list. Choose
   the Windows or macOS download, or the official Linux Flatpak on that page.
2. Right-click the game and open **Properties**. Under **Info**, check
   **Game ID: R4QE01** and **Revision 1** in the game name.
3. Open **Filesystem**. Right-click **Data Partition (0) - R4QE01** and
   choose **Extract Entire Partition...**. Select a new, empty folder;
   allow about 3 GB extra space and wait for extraction to finish.
4. Copy the resulting `sys/` and `files/` folders into
   **`ux0:data/mscharged/game/`** using [USB, FTP or your configured memory card](MANUAL-INSTALL.md#choose-a-transfer-method).

Choose the entire **data partition**, not the update partition or just
**Extract Files...**, which leaves out the system files. Keep your original
dump. If extraction reports an error or is cancelled, retry into a new folder.

The final paths must include:

```text
ux0:data/mscharged/game/sys/fst.bin
ux0:data/mscharged/game/sys/main.dol
ux0:data/mscharged/game/files/Art/
```

Do not add another `game/` or `DATA/` level. Other extracted partition
metadata is not needed. Return to [Install](MANUAL-INSTALL.md).

That is all the game-data preparation this Strikers package needs. The VPK
contains its playback fixes and startup shaders; you do not need to run the
desktop installer for an extra movie or image conversion step.

## Optional: Automatic Extraction And Checks

Prefer an automatic revision/file check? The included helper uses
[Python 3](https://www.python.org/downloads/) on your computer, not the Vita.
Extract the whole install ZIP, open Terminal or PowerShell in that folder,
and run:

```powershell
# Windows
py -3 PREPARE-GAME.py "C:\Games\game.rvz"
```

```sh
# Mac or Linux
python3 PREPARE-GAME.py "/path/to/game.rvz"
```

ISO works too. The helper downloads checksum-pinned
[nodtool 2.0.0-alpha.12](https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.12),
validates the data while extracting, and checks the disc revision and
`main.dol`. No Python packages are needed; internet is needed only for the
first extractor download.

When it says **READY**, copy `prepared-game/sys/` and `prepared-game/files/`
into **`ux0:data/mscharged/game/`**, then return to [Install](MANUAL-INSTALL.md).
It never connects to your Vita, uploads game data or replaces existing
folders. To retry after an interruption, choose a new folder with
`--output "prepared-game-2"`. Keep your original dump.

## Optional: Check An Existing Extraction

The SHA-256 of `sys/main.dol` should be:

```text
8f811628ee6646c60620a9c599ef08ed929d2eccc98a779c211847f1500f81fd
```

From the extracted folder, run `Get-FileHash -Algorithm SHA256 sys/main.dol`
in Windows PowerShell, `shasum -a 256 sys/main.dol` on macOS, or
`sha256sum sys/main.dol` on Linux. A mismatch means you should check your
dump/revision, not download a replacement `main.dol`.

## Verification Notes

The Dolphin menu labels and `sys/` plus `files/` output were checked against
its [extraction code](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/DolphinQt/Config/FilesystemWidget.cpp).
This GUI route has not yet had an end-to-end test here.

On 2026-10-18, this nodtool version extracted a Rev 1 RVZ on Apple silicon
with Wii data-hash validation enabled. Every output file under `sys/` and
`files/` matched the retained extraction byte-for-byte; the only layout
difference was an empty `files/GameLog/` directory. Windows/Linux execution
and a fresh Vita installation have not been tested here.

The extractor is an external tool, not bundled game content. Its
[source and licenses](https://github.com/encounter/nod) are maintained upstream.
