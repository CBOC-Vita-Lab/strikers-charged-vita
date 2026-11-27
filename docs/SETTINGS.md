# The Settings Screen

For maintainers. The player's guide is [CONTROLS.md](../CONTROLS.md); every key is in
[TECHNICAL.md](TECHNICAL.md).

## Layout (set18: the game's own menu is the one place)

`kTable` in `native/platform/vita/src/np_settings.cpp` is the list of rows and the order of the lines in `np-player.txt`. The player changes them in the game's
own menu, OPTIONS > VITA SETTINGS or the pause menu's VITA SETTINGS: `kRows` in `native/platform/sdk/net/np_native_settings.cpp` names which row stands where
(five rows a page, most used first). No key, value or choice depends on a page.

| Page | Rows (the menu's short name) |
|---|---|
| MAIN | All Characters, All Stadiums, Frame Rate, Controls, Language |
| PICTURE | Crowd, Sharpness, Blur (Depth Of Field), HD Characters |
| TOUCH | Rear Touch, Mega Save (aim), Save Speed, Touch Strike, Touch Shakedown |
| MATCH | Less Shake, Mega Strikes, Game Fixes, Pointer Speed |
| ONLINE | Connection, ONLINE ACCOUNT, RESET TO DEFAULTS |

Each row is "NAME: VALUE" (at most 24 characters in English, Spanish and French) and has one line of help (`NpNativeSettings::Help`: the row's `help[0]`, translated),
shown on the panel's steel foot under DONE while the cursor is on the row, in the bars' own lettering (`np_settings_help.h`; the pause panel package has no description
text of its own, so it is drawn the way the online banner is, through the game's text box and font).
The first page opens with the two unlock rows (owner, 15 Nov). RESET TO DEFAULTS puts every row at its default; leaving then saves it (and drops the retired lines).

The port's pixel screen (`kPages` in `np_settings.cpp`, drawn by `Draw`) has two pages only:

| Page | Rows | Opened by |
|---|---|---|
| SAFE START | Core Plan, Version, Build, Settings File, Game Files (then Reset To Defaults, Save And Start, Save And Exit) | holding L + R while the game starts: the way out when a setting stops the game from starting (the fatal screen says so). Not a settings menu. |
| ONLINE | Online Name, Send A Report, Save A Report, Delete My Online Account | VITA SETTINGS > ONLINE ACCOUNT, over the paused game |

The LiveArea Settings tile is gone (1.3, `template.xml` content-rev 5). A console that still shows the old tile starts the game with `-settings`: the game starts normally and
`log.txt` says where the settings are; a press of the old tile while the game runs is logged and dropped.

**Problem reports and your online account (ONLINE page).** Three action rows, each run with Cross:
- **SAVE A REPORT** writes `ux0:data/mscharged/mp-report.txt` on the card (nothing leaves the Vita).
- **SEND A REPORT** opens a screen that lists exactly what would be sent. CANCEL is lit first; SEND sends one report to the online service, and the line under
  the list says what happened (sent, the limit of 3 a day, no connection, or the service not taking reports). Circle at any time stops it, also while it waits.
  It needs an online account (play online once, so the game has made your online name); without one it says so and shows no screen. The service
  keeps a report for 30 days. See "What a report contains" in the README.
- **DELETE MY ONLINE ACCOUNT** asks first (NO is lit first). YES removes your name, friends, invitations and reports from the online service and makes this Vita
  forget its online name; the next online game asks for a name again. Your save and your settings are not touched. If the service cannot be reached the Vita
  keeps its identity and says so, so you can try again. It cannot be undone.

Nothing talks to the service until you press SEND or YES. Neither row is available during a multiplayer game (the screen is not offered then).

### Rows that went (set18, owner 23 Nov: "Remove any settings that aren't needed or are just pure negative to the experience. Simplify where able")

| Row or choice | Why | Its keys now |
|---|---|---|
| RESOLUTION (STANDARD / SMALLER 864X488) | purely worse: a fixed size never adapts and turns the dynamic resolution, its smoothing and SMOOTH 60 off | `MSC_VITA_RENDER` retired; in the screen's block: obsolete (skipped) |
| ANTI-ALIASING (ON / OFF) | OFF purely worse: the picture already gives the samples up first under load | `MSC_VGX_MSAA` retired; `=0` in the block: obsolete |
| SMOOTH 60 MODE (OFF / ON) | redundant with FRAME RATE (two rows for the frame rate) | `MSC_LOCK60` owned by FRAME RATE (SMOOTH 60) |
| FRAME RATE AUTO (UP TO 60) | SMOOTH 60 is the frame-rate mode (23 Nov console A/B); the 45 mode was rejected on 18 Nov | `MSC_FPS=auto`, `MSC_FPS_BETWEEN=pace` in the block: obsolete |
| INPUT DELAY | a netcode tuning knob: the game's own delay adapts to the link | `MSC_LAN_INPUT_DELAY` retired |
| TOUCH POINTER | not needed (the pointer only moves in menus that use one) | `NP_TOUCH_POINTER` retired |
| TOUCH MEGA SAVE | redundant with MEGA SAVE AIM (its OFF made the other row do nothing) | `MSC_TOUCH_SAVE=0` is MEGA SAVE AIM: STICK ONLY |
| MULTIPLAYER LINK | redundant: the game's menu asks ONLINE / LOCAL and NEARBY / SAME WI-FI every time; the HOST / JOIN choice (NEARBY PAIRING) went too, below | `MSC_LAN_MODE` retired |
| NEARBY PAIRING (AUTO / NEARBY HOST / NEARBY JOIN) | mp24, owner: "if they don't help anything": no console run showed it fixing a pairing; AUTO is the system dialog's own mode, the one the retail game uses | `MSC_LAN_DIALOG` retired (the radio still reads a hand line `create` / `join` outside the screen's block) |
| ORIGINAL GAME FIXES: OFF (AS RETAIL) | purely worse: the retail drawing bugs back | `MSC_GAME_FIXES=none` in the block: obsolete |
| MEGA SAVE SPEED: ORIGINAL, 2x | purely worse: the glove did not reach the far side in time | `MSC_MEGA_POINTER_SPEED=1\|2` in the block: obsolete |
| LIVE SHADOWS (FAST / FULL) | owner 20:36 ("if they don't help anything"): FULL helps nobody (a pass more in live play; replays and cut-scenes keep the full shadows) | `MSC_SHADOW_LIVE` retired |
| PICTURE FORMAT (WIDESCREEN / 4:3 BOX) | owner 20:36: 4:3 only puts bars on the Vita's wide screen | `NP_WIDESCREEN` retired |
| ALL SETTINGS (the menu row) and the full pixel list | the game's menu holds every row | the row is ONLINE ACCOUNT now |
| SETTINGS AT START (info line) | L + R is SAFE START now, not a way into the settings | |

"Retired" (`kRetiredKeys`): Load ignores the key, a line the screen wrote (inside its block) goes at the next save, a line outside the block stays byte for byte, RESET TO DEFAULTS drops it
anywhere. "Obsolete" (`kObsolete`): a purely worse value inside the screen's block is skipped by the loader at start-up with a log line (`NpSettings::WithoutObsolete`), the row reads its
default and the next save drops the line; outside the block (the kit, a player's own line) it is applied as written. Every key keeps its reader.

### Rows that went (1.3 prune, 15 Nov)

The owner asked for every setting that no longer makes sense, or is not needed, to be removed: 22 rows, the PICTURE MODE row (the presets with it), and a few choices. The table
of their keys is `kRetiredKeys` in `np_settings.cpp`, and `RETIRED` in `tests/audit_settings_keys.py` names the row each one belonged to. Nothing was removed from the game: every key still
has its reader, and the console kit and a hand-written file set it as before. Gone from the screen: PICTURE MODE (and its presets and the PICTURE (AUTO) note), Replays & Cut-scenes, Full Picture,
Dynamic Resolution, Smallest Picture At 60, Sharper When Possible, Prioritize Players, Effects Quality, Glow, Replay Shadows, Replay Far Players, Distant Textures, Frame Pacing, Gxm Direct,
Fourth Core, Texture Detail, Picture Shape, Native Hud, Still Backdrops, Effects Under Load, Match Shadows, Texture Memory and Menu Stick. Choices that went: UP TO 60 of Frame Rate (what AUTO does by
itself), 4X of Anti-aliasing, ON (AS GAME) of Depth Of Field, CRISP+, SMOOTH NO EDGE AA, FAST and SMOOTH FULL of Sharpness. `MSC_SETTINGS_AUTO` is deleted with its migration: the rows it hid are gone for everyone.
This list and `kRetiredKeys` are the only places the dead titles stay, besides CHANGELOG.md.

## How the pixel screen looks (style18)

The pixel screen is SAFE START (hold L + R at start-up) and the ONLINE page (VITA SETTINGS > ONLINE ACCOUNT). Every page (however many `kPages` names; the tabs share the width, nothing depends on a count) and both dialogs are drawn by the `Draw*` functions of
`np_settings.cpp` in the look of the game's pause panel, from hand-chosen colour constants (`Rgb(...)`, the block above `kVisibleRows`) and our own bitmap font: a steel frame with rivets, a blue-glow panel,
grey-blue striped row bars with a lighter bar and a blue glow for the selected row, white heavy italic lettering with a black outline, an orange title band with slanted streaks, slanted page tabs
(the open one orange). No game art is copied or committed; over the player's own game art (read at run time, as before) the title band is dark steel so the game's heading lettering stays readable.
Only the drawing changed: no row, key, page or dialog logic. A dialog's box is one flat colour (`kPanel`) inside a 32 px margin of the same colour: `test_pause_settings` checks that no text reaches the 32 px margins
and that the screen outside the box is dimmed, so a new dialog decoration must keep to x 112..848 inside the box.

## Adding a row

1. Append it to `kTable`, at `NEW ROWS GO HERE`. Never insert: that order is the order of the file's lines.
2. The first choice is the build's own default and writes nothing. If the build sets the default in `vita_plan.cpp`, a row whose OFF is
   not the first choice follows `DEPTH OF FIELD` (until ship26 it was ON first; the build's default is now OFF, so it is first and ON writes `lazy`): the first choice writes nothing and OFF writes the off value. If a file may name the
   default's value, add the pair to `IsBuildDefault` (now `MSC_DOF=0`, and `MSC_LOCK60=1` while `VitaPlanDefaults::kLock60` is true).
3. Give it a place in the game's menu: `kRows` in `np_native_settings.cpp` (a page has five bars; add a page if none is free), with a name that fits "NAME: VALUE" in 24
   characters in all three languages (`kShortValues` for a long value), and one line of help (`help[0]`, 76 characters, with its Spanish and French in `np_text_strings.tsv`).
4. If the row can have no effect because of another row, add that to `NoEffect`.
5. Add the key to `tests/audit_settings_keys.py`, the row to `docs/TECHNICAL.md`, and run the tests below.
6. To retire a row or a choice: delete it, add its keys to `kRetiredKeys` and to `RETIRED` in the audit, keep every reader; if a value an earlier 1.3 screen wrote is only worse,
   add it to `kObsolete`; add a guard rule only if a file written by 1.0-1.2 must stop pinning the old value (`np_settings_guard.cpp`, files stamped before 1.3.0 only).
   `tests/run_settings_prune_tests.py` has the mutants of this rule.

`kMaxOptions` is 56, with 20 rows after set18 and mp24; `test_settings` checks each help line at 76 characters and that `help[1]` is empty (one line per row).

## Older files and changed defaults

There is no version line, on purpose. A key is in the file only when the player moved its row or wrote the line by hand, so a new switch or a
default changed by a later build reaches every older file by itself. Nothing is migrated and no file is rewritten until the player saves.

What an older file can still do, and how it is handled:

- **A line naming the old default's value.** It would pin that value. `IsBuildDefault` reads it as the row's default and the next save drops it;
  `test_settings` holds that table equal to `vita_plan.cpp`. A default changed there must keep its old value listed too.
- **A retired line (the prune).** A key whose row went (`kRetiredKeys`) is ignored by Load and kept by Serialize: a line the screen itself wrote (inside its marked block) is
  dropped at the next save, a line outside the block (a kit file, a hand test) stays byte for byte, and RESET TO DEFAULTS drops them wherever they stand. SHARPNESS keeps the
  line of a variant that went (CRISP+, SMOOTH NO EDGE AA) while the row reads CUSTOM. A file with a preset selector (`MSC_PICTURE=performance`) still expands in the game; the screen only rewrites the
  selector when its rows can show it.
- **A file stamped before 1.3.0** (`np_settings_guard.cpp`, once, with `np-player.before-1.3.txt` as the backup) also loses: `MSC_DOF=1`, `MSC_FPS_REPLAY`, `MSC_FX_LOWRES=0|off|4`, `MSC_FX_LOWRES_MINPX`, `MSC_FX_LOWRES_SCENES`,
  `MSC_VITA_ANISO` and `NP_MENU_NAV=0`: each pinned an older picture or turned a control off that 1.3 now decides. A file stamped 1.3.0 or later (the console kit's) is never touched.
- **A fixed picture size (`MSC_VITA_RENDER`) or `MSC_VITA_MSAA` outside the screen's block** (a hand line). These do turn the adaptive picture off. No row owns them
  (set18): they are kept as written until RESET TO DEFAULTS. Inside the screen's block a fixed size is obsolete and skipped at start-up.
- **A line kept from a hand test** (any key without a row, such as `MSC_VGX_SOFTXFORM`). It wins over the build's default until removed. Remove such lines
  before a measured run.

## The LiveArea button (1.3: the tile is gone; set18: an OLD tile's press opens nothing)

front20: template.xml has no Settings frame any more (content-rev 5): every setting is reachable in the game (OPTIONS > VITA SETTINGS by D-pad, stick pointer and touch, host route w8/front20-runs/reach.txt; the pause item; ALL SETTINGS for the technical pages; L + R at the start). The parameter handling below is kept for a console whose bubble still has the old tile.

set18: `-settings` at start-up starts the game normally (a log note says where the settings are); a press while the game runs is logged and dropped. `NpSettings::OpenRequest` treats a press as the parameter appearing. Whether `sceAppMgrGetAppParam` consumes the parameter is not known for this
build: 1.2 assumed it does, and the Galaxy port's screen reopened every 250 ms, which means it did not there. Both cases are safe: the screen opens once. If the
parameter stays, a second press in one run cannot be seen. A try that cannot open the screen is repeated at the next polls, three tries in all.

## New switch

| Key | Values | Default | Effect |
|---|---|---|---|
| `MSC_BOOT_HINT` | seconds; `0` off | `20` | Seconds before the loading screen's slow-start line. `0`: no line under the bar at all, the screen as in 1.2 |

Log: `[boot] milestone: NAME  [+N ms]` for each step, and a `slow start` note in `log.txt`.

## Tests

From `native/platform/vita`:

```
g++ -std=c++17 -O1 -pthread -Iinclude -Isrc -I../../include tests/test_settings.cpp src/np_settings.cpp src/vita_plan.cpp -o /tmp/ts && /tmp/ts [picture dir]
g++ -std=gnu++17 -O2 -pthread -Isrc -Iinclude -I../../include tests/test_bootscreen.cpp src/vita_bootscreen.cpp -o /tmp/tb && /tmp/tb - <picture dir>
python3 tests/run_settings_display_tests.py
python3 tests/run_settings_runtime_tests.py
python3 tests/audit_settings_keys.py
python3 tests/run_settings_prune_tests.py
python3 tests/run_native_settings_tests.py
```

The screen's test script (`np-settings-open`) has two new words, `ltrigger` and `rtrigger`, for the pages.

## Console checks

0a. (front19) Main menu, OPTIONS: the corner button that says DONE on the other screens says VITA SETTINGS (D-pad, stick pointer, finger; hover and press sounds as the other bars). It opens the same panel as the pause item, on the front end's scene stack (`SHVitaSettings.h` `sFrontEndNext`; popups through `NpMenuUiShow`); leaving goes back to OPTIONS, and the next screen's corner button says DONE again. The leave popup is SAVE / DON'T SAVE / KEEP EDITING, or SAVE AND START AGAIN / DON'T SAVE / KEEP EDITING when a row that waits for the next start changed (`np_native_settings.h` `Apply`: the game starts again with the saved file). Offered only when the pause item would be (`NpPauseSettings::DecideOptions`: a settings screen, `MSC_PAUSE_SETTINGS` not 0, no LAN or online session).
0. Pause a match, pick VITA SETTINGS: five pages (MORE), every row changes with Cross; ONLINE ACCOUNT on the last page opens the ONLINE page over the paused game; Circle (or Square
   with CLASSIC controls) leaves it. RESET TO DEFAULTS, then DONE: the popup asks; SAVE writes the file.
1. Start the game from an old LiveArea Settings tile (a console that was not refreshed): the game starts normally, `log.txt` has the note. Hold L + R at start-up: SAFE START opens.
2. Change a row, press Circle: the dialog appears. Try all three choices.
3. On a start that takes more than 20 s (the first start after an update, for example) the hint appears and `log.txt` holds one `slow start` note. `MSC_BOOT_HINT=1` shows it on any start.
