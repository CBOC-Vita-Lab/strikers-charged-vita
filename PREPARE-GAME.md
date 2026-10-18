# Prepare Your Game

Use your own **Wii USA Rev 1 (`R4QE01`)** dump. This creates the `sys/` and
`files/` folders the port reads; it does not compile or modify the game.
Allow about 3 GB of extra computer space, keeping your original dump intact.

## Easiest Method

Install [Python 3](https://www.python.org/downloads/) on your computer if it
is not already available. Extract the whole package, open Terminal or
PowerShell in that folder, and run one command with your dump's filename:

```powershell
# Windows
py -3 PREPARE-GAME.py "C:\Games\game.rvz"
```

```sh
# Mac or Linux
python3 PREPARE-GAME.py "/path/to/game.rvz"
```

ISO files work too. The helper downloads the official extractor for your
computer, verifies its checksum, checks the disc revision, extracts with
validation, and checks `main.dol` automatically. No Python packages are needed.
Internet access is needed only for the first extractor download.

When it says **READY**, copy `prepared-game/sys/` and `prepared-game/files/`
into **`ux0:data/mscharged/game/`**, then return to [Install](INSTALL.md).
It never connects to your Vita, uploads game data or replaces existing
folders. To retry after an interruption, choose a new folder with
`--output "prepared-game-2"`. Keep your original dump.

## Manual Method

Use this only if you prefer to run the extractor yourself.

### 1. Get The Extractor

Download the matching `nodtool` file from the official
[v2.0.0-alpha.12 release](https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.12):

| Computer | Download |
| --- | --- |
| Windows, Intel/AMD | `nodtool-windows-x86_64.exe` |
| Windows, ARM | `nodtool-windows-arm64.exe` |
| Mac, Apple silicon | `nodtool-macos-arm64` |
| Mac, Intel | `nodtool-macos-x86_64` |
| Linux | `nodtool-linux-x86_64` or `nodtool-linux-aarch64` |

Put the tool beside your dump. Rename the tool to `nodtool.exe` on Windows
or `nodtool` on Mac/Linux. Open PowerShell or Terminal in that folder.
In the commands below, replace `game.rvz` with your dump's actual filename,
keeping the quotes. ISO and RVZ are supported; no conversion is needed.

### 2. Check And Extract

Windows PowerShell:

```powershell
.\nodtool.exe info "game.rvz"
```

Mac/Linux Terminal:

```sh
chmod +x ./nodtool
./nodtool info "game.rvz"
```

At the top of the output, check **Game ID: R4QE01** and **Disc 1, Revision 1**.
The later data-partition header may say Revision 0; use the top-level disc
revision and the file check below. Stop for a different game or revision.

Extract into a new folder named `game`; do not reuse a partial extraction.

Windows:

```powershell
.\nodtool.exe extract --validate --partition data "game.rvz" "game"
```

Mac/Linux:

```sh
./nodtool extract --validate --partition data "game.rvz" "game"
```

Wait for a successful finish. A validation error means the dump needs
checking; do not disable validation to force it through.

### 3. Check The Result

Calculate the SHA-256 of `game/sys/main.dol`:

```powershell
# Windows PowerShell
Get-FileHash -Algorithm SHA256 ".\game\sys\main.dol"
```

```sh
# macOS (use sha256sum instead of shasum -a 256 on Linux)
shasum -a 256 "game/sys/main.dol"
```

It must match, ignoring letter case:

```text
8f811628ee6646c60620a9c599ef08ed929d2eccc98a779c211847f1500f81fd
```

If it differs, stop and check your dump. Do not obtain a replacement
`main.dol` separately.

Copy **only `game/sys/` and `game/files/`** into `ux0:data/mscharged/game/`
using VitaShell's USB or FTP connection. This must leave
`ux0:data/mscharged/game/sys/fst.bin` and
`ux0:data/mscharged/game/files/Art/`. Do not add another `game/` or `DATA/`
level. The other extracted disc/partition metadata is not needed here.

Return to [Install](INSTALL.md#installation) to install the VPK.

## Verification

On 2026-10-18, this nodtool version extracted a Rev 1 RVZ on Apple silicon
with Wii data-hash validation enabled. Every output file under `sys/` and
`files/` matched the retained extraction byte-for-byte; the only layout
difference was an empty `files/GameLog/` directory. Windows/Linux execution
and a fresh Vita installation have not been tested here.

The extractor is an external tool, not bundled game content. Its
[source and licenses](https://github.com/encounter/nod) are maintained upstream.
