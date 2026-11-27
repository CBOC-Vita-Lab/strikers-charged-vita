# Known Issues

A few things we're still working on. If you hit something not listed here,
please [open an issue](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/issues).

## Performance

- **60 fps depends on the scene.** **SMOOTH 60** (default) runs matches up to
  60 fps at native 960×544 and steps smoothly to 45 or 30 in the heaviest
  moments (crowded stadiums, heavy weather or cut-scenes). If you prefer a
  constant frame rate everywhere, set **Frame Rate** to **30 LOCKED** in
  VITA SETTINGS.
- **Goal replays and heavy cut-scenes run at 30 fps or below.** Close-up
  cameras draw far more detail than open play. Gameplay itself is not affected.
- **Rare unseen shader combinations.** The 2.0 package ships **4,281**
  precompiled Vita shaders covering all 17 stadiums, 12 captains, 11 power-ups,
  menus, character select, replays and Mega Strikes. Any uncommon combination
  not in the pack compiles once via `libshacccg.suprx` and is saved to your
  memory card cache for future sessions.
- **First launch after updating from 1.0 or 1.1.** Existing compiled shaders on
  your memory card are migrated into the new layout once at first start-up.
  Nothing needs deleting.

## Gameplay

- **The CPU judges distance a little differently at zoom levels 4 to 7.** It
  now plays as if it could see the standard TV view, so zooming in no longer
  makes it weaker.
- **Spanish and French are menus and text only.** The USA disc carries the North
  American Spanish and French texts, so the game can run in either language
  (Settings > Language, or `MSC_LANGUAGE=es` / `fr` in `np-player.txt`; English
  is the default; it applies at next launch). Since 2.0 the port's own screens
  follow it too: Settings, the online and local multiplayer menus, the online
  banner, the loading screen and the error messages. The disc has no other
  voices or movies, so the sound is the same in every language. Two things stay
  English or Wii-worded for now: the game's own tutorial and help texts still
  name the Wii's buttons in Spanish and French (the button renaming works on the
  English texts only), and the controller picture of the pause menu is English.
  German and Italian texts are also on the disc, but the USA game does not offer
  them and the port does not either.
- **Some settings apply at next launch.** Use **Save and Restart** in Settings
  to apply them straight away.
- **An old LiveArea Settings tile just starts the game.** The tile is gone from
  2.0's bubble; a console that still shows it opens the game normally. The
  settings are in the game: pause, or OPTIONS, then VITA SETTINGS.
- **Settings saved with a 1.1 preset show CUSTOM.** The 1.1 presets fixed the
  picture size, which switches the dynamic resolution off, so the newer picture
  modes no longer do that. Choose Quality (Auto), Performance (60) or Locked 30
  again to get the current version. A hand-written core plan line and the 1.1 Anti-aliasing line also
  show CUSTOM until you change that row.
- **No Mega Strikes does not apply everywhere.** Strikers 101 and the
  challenges keep their Mega Strikes. In local play both Vitas must use the same
  setting, and the same goes for Original Game Fixes: ALL.
- **Wider testing needed** for two-Vita local play, the Classic layout, rear
  touch and long career runs. Reports are very welcome.

## Local Play And Online

- **2.0 cannot play 1.0 or 1.1 over the local link.** The versions use
  different link versions, so each ignores the other: the host's game never
  appears in the other Vita's list. Update both Vitas. Two 2.0 Vitas with
  different match rules (No Mega Strikes, Original Game Fixes: ALL) also do not
  find each other. The link carries the game's version, so both Vitas need the
  same release to play each other.
- **Online play (Beta) needs Wi-Fi and a correct console date and time.** The
  service is live; MULTIPLAYER offers hosting and joining with a code, Quick
  Play and Ranked Match. Some networks need a relay, and a service outage or
  exhausted relay allowance can prevent a new connection. Two-Vita field
  testing is ongoing — use **REPORT A PROBLEM** in the Multiplayer menu if you
  hit a disconnect or desync.

## Install

- **The first launch after updating from 1.0 or 1.1 can take a little longer.**
  The graphics library changed, so the compiled shaders already on your Vita are
  moved into the new layout once. A very large cache can finish over the next
  launch. Nothing needs deleting.
- **A missing or cut-short game file stops the game with a message.** It names
  the file and returns to the LiveArea. Copy that file again from your disc. The
  preparation helper checks the file table, sizes and positions, not every byte of
  every file, so a copy that was damaged in transit can still get through to
  this message.
- **Movie decoding on the Vita's JPEG hardware is new.** The game compares the
  first frames of each movie with the software decoder and uses software for the
  rest of the session if they differ. If a movie ever looks wrong or stutters
  anyway, please report which one. `MSC_THP_HWJPEG=0` in `np-player.txt` turns the
  hardware decoder off.

- **The desktop installer may take a little while to list a new version.** If
  **Get** offers an older one, use the manual steps in the
  [install guide](MANUAL-INSTALL.md), or try again later.
- You need your own copy of the game and the Vita's shader compiler module
  (`libshacccg.suprx`). Neither is included.

## If Something Goes Wrong

Open `ux0:data/mscharged/np-player.txt` and add either line to go back to the
previous behaviour:

```
MSC_FPS=30
MSC_DYNRES=off
```

More switches are listed in [Technical Notes](docs/TECHNICAL.md#runtime-switches).

**Two failed starts after a Picture or Language change (startcheck19).** If the game does not start twice in a row after you change a Picture row (resolution, frame rate, anti-aliasing,
sharpness, depth of field, HD characters, picture format, shadows) or the Language row, the next start uses the standard ones for those rows and the loading screen says
"2 FAILED STARTS: STANDARD PICTURE AND LANGUAGE" (also in Spanish and French). Your `np-player.txt` is not changed; the standard ones stay in use until you change those rows again.
The count lives in `ux0:data/mscharged/np-change.txt` (three numbers), apart from the core plan's `np-plan.txt`, which steps down on the same failed starts by its own rule. Leaving the
game within its first 10 seconds twice after a change counts as two failed starts. `MSC_CHANGE_GUARD=0` in `np-player.txt` turns the check off.
