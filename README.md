# Mario Strikers Charged Vita

A native PS Vita port of Mario Strikers Charged, based on the reconstructed
Wii source. It runs the game as ARM code, with a Vita graphics, audio and
controller layer.

## In This Build

- Modern Vita controls, a Classic option and Vita button prompts.
- Seven camera zoom levels, with Auto Zoom and two extra close-up views.
- GPU skinning, renderer optimizations and a packaged startup shader cache.
- Recent fixes for startup delays, loading-screen flicker and stadium-preview
  audio timing.

## Install

New to the package? Open [Start Here](START-HERE.txt).

You need a homebrew-enabled Vita, VitaShell and your own supported game dump:
**Wii USA Rev 1 (`R4QE01`)**. Game files are not included.

Installation uses a ready-made VPK plus your extracted `sys/` and `files/` folders
under `ux0:data/mscharged/game/`. Follow [Install](INSTALL.md) for the layout
and backup steps, with [game extraction](PREPARE-GAME.md) in a separate guide.
You should not need to replace `eboot.bin` or run lab tools.

## Release Status

This is private release preparation for Claude's native `a8d4ad6` build.
The installed executable and app resources have been packaged as a private
VPK candidate without changing them. A fresh console read confirms the
executable is that exact build. You will not need to compile the port.
Matching source/dependencies, public-content clearance and fresh-install
testing are still needed before publication. No public download is available.

Level 7 has an emulator match test. Level 6, the Options cycle and the final
package still need Vita acceptance. Menus can run below 30 FPS; no locked
30 FPS or whole-game completion claim is made. See [Known Issues](KNOWN-ISSUES.md).

[Controls](CONTROLS.md) | [Included Files](ASSETS.md) | [Credits](CREDITS.md)

CBOC, Claude and ChatGPT contributed to the port and release preparation;
upstream contributors retain their credit and rights.
