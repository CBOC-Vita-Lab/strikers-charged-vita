# VitaDB Submission Text

Plain text for the VitaDB listing of Mario Strikers Charged. Copy the fields below into the submission form; nothing else is needed.
It was prepared for the 2.0 submission, after the earlier one was declined for its date-style version and its separate settings
package. Both are fixed: the version is a plain number and there is one VPK.

## Fields

**Name:** Mario Strikers Charged

**Version:** 2.0

**Title ID:** MSCN00003

**Type:** Port (native)

**Download:** https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/latest/download/Strikers-Charged-Vita.vpk

**Source:** https://github.com/CBOC-Vita-Lab/strikers-charged-vita

**Data files:** none included. The game needs the player's own Wii disc (USA, Rev 1, R4QE01), extracted with Dolphin and copied to
`ux0:data/mscharged/game/`. Instructions: https://github.com/CBOC-Vita-Lab/strikers-charged-vita/blob/main/MANUAL-INSTALL.md

**Requirements:** `libshacccg.suprx` in `ur0:data/` (the system shader compiler, as for other vitaGL ports); about 3 GB free.

## Short description

A native PS Vita port of the Wii football game Mario Strikers Charged: the game's own code rebuilt for the Vita, with a new renderer
for its graphics chip. Not an emulator. Requires your own copy of the game.

## Long description

Mario Strikers Charged runs natively on the PS Vita: the five-a-side football, the stadiums, the Mega Strikes and the captains, rebuilt
from the game's code for the Vita's processor and drawn by a renderer written for its graphics chip.

- Matches at up to 60 fps, adaptive picture quality, optional HD characters.
- Online play with friends by invite code, ranked matches and leaderboards, local play over ad hoc or Wi-Fi.
- English, Spanish and French.
- Settings inside the game: pause a match and pick VITA SETTINGS (or OPTIONS > VITA SETTINGS in the main menu), then return to your match.
- Modern and Classic control layouts, touch and rear-touch options.

You need your own Mario Strikers Charged disc (Wii, USA, Rev 1). No game data is included.

Unofficial fan project, not affiliated with Nintendo or Sony.

## Changelog for this version

Version 2.0: Smooth 60 fps up to native 960x544 by default, full-game precompiled shader pack (4,281 Vita shaders), crisp Mega Strikes and Mega Saves, Pipeline Central and Dump optimizations, online play and ranked matches (Beta), Spanish and French for the port's own menus, and a cleaner five-page in-game VITA SETTINGS menu (one VPK, no LiveArea Settings tile). Full notes: https://github.com/CBOC-Vita-Lab/strikers-charged-vita/blob/main/CHANGELOG.md

## Checklist before sending

- The release `v2.0` exists, its `Strikers-Charged-Vita.vpk` installs over 1.1, and `sce_sys/param.sfo` reads `APP_VER 02.00`.
- The release has one VPK and the install ZIP; no settings or test package is attached.
- The CHANGELOG's top heading reads `## Version 2.0 — <date>`.
