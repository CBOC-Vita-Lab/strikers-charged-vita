# What's New

## Version 2.0 — November 27, 2026

**Our biggest update yet — bringing smooth 60 fps gameplay with crisp dynamic resolution, the first-ever local and online multiplayer for Mario Strikers Charged on PS Vita, a complete stutter-free shader pack, automatic on-device artwork, Spanish & French support, and redesigned in-game settings.** *(Version 1.2 was shared briefly and withdrawn, so 2.0 includes every improvement from 1.2 along with months of new speed, visual, and stability work.)*

### Smooth 60 FPS & Crisp Dynamic Resolution
- **Smooth 60 fps with dynamic resolution up to native `960×544`.** **SMOOTH 60** is now the default: matches run cleanly at **60 fps** using a dynamic 3D resolution ladder that climbs up to full native **`960×544`** whenever GPU headroom allows while keeping the **HUD, menus, Title screen, and Mega Saves locked at full native `960×544`**. In the heaviest chaos it steps smoothly **60 → 45 → 30** and climbs straight back to 60 fps once the action clears. Prefer max sharpness everywhere? Pick **30 LOCKED** anytime in **VITA SETTINGS** for locked 30 fps at **full native `960×544` with `2× MSAA`**.
- **Complete full-game shader pack (4,281 shaders).** Prebuilt on a real PS Vita across all 17 stadiums, all 12 captains, all 11 power-ups, menus, character select, tackles, goals, replays, Mega Strikes, and Mega Saves — virtually eliminating mid-match shader compile freezes while keeping startup warm-up quick (~1.2 s).
- **Razor-sharp Mega Strikes, Mega Saves & Title screens.** Mega Strikes run smoothly without distracting mid-scene resolution jumps, while Mega Save gloves, balls, and the Title / Game Start screen stay crisp at full native **960×544**.
- **Pipeline Central & The Dump overhauled.** Pipeline Central runs much smoother with a tuned reflective floor and mist profile, while The Dump's rain, shadow flicker, kickoff hitch, and 30-fps recovery edge cases are fixed.
- **Faster from top to bottom.** Rebuilt with profile-guided Vita optimizations for extra 60 fps headroom, automatic high-performance Vita clock tuning, and keep-awake protection so the screen never dims mid-match or during loading.
- **Recommended optional plug-ins for maximum 60 fps.** Works great with zero plug-ins, and **automatically supports [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker)** (unlocks the Vita's 4th CPU core for extra headroom with zero configuration) and **[PSVshell](https://github.com/Electry/PSVshell) / [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus)** (`500 MHz` CPU / `222 MHz` GPU & bus overclock — automatically detected and preserved). See [Optional Plug-ins](docs/PLUGINS.md) for details.

### First-Ever Online & Local Multiplayer on PS Vita *(Beta)*
- **Play online with friends or rivals.** Create a private room and share a short invite code, jump straight into unranked **PLAY WITH ANYONE**, or climb the season and daily leaderboards in **RANKED MATCH** (live web rankings at **[strikers.psvita.dev/leaderboard](https://strikers.psvita.dev/leaderboard)**).
- **Local multiplayer anywhere.** Play another Vita head-to-head over **Same Wi-Fi** or **Nearby** local play.
- **Smoother connections & clearer status.** Improved Vita network compatibility, clearer online name feedback, a live **"Waiting for the other player..."** timer during hiccups, instant cleanup when a partner disconnects, and built-in **REPORT A PROBLEM** and **ONLINE ACCOUNT** management.
- *Note: Multiplayer is marked **Beta** while two-Vita field testing continues. Version 2.0 uses an updated link protocol — update both Vitas to connect.*

### One App, Automatic Artwork & Redesigned In-Game Settings
- **Clean in-game VITA SETTINGS.** Pause any match and choose **VITA SETTINGS**, or open **OPTIONS > VITA SETTINGS** from the main menu — five tidy pages with plain-English help, **RESET TO DEFAULTS**, and **ONLINE ACCOUNT**. No separate Settings download or extra LiveArea tile needed; hold **L + R** at launch for **SAFE START** if a setting ever prevents boot.
- **Automatic first-boot loading screen, LiveArea & Home bubble.** On first launch, the game automatically builds its own *Mario Strikers Charged* loading screen, Mario vs. Bowser LiveArea background, fiery START gate, launch splash, and circular Home screen bubble straight from your own disc files on your Vita — with zero copyrighted art bundled in the download and without ever overwriting custom artwork. *(LiveArea and loading screen appear immediately; on manual VPK installs, reboot your Vita once to refresh the Home bubble, or use CBOC Vita Installer to brand before install. Offline `tools/brand-from-disc.py` is still included too.)*
- **Natural game progression.** Unlock All now defaults to OFF so you can earn trophies and stadiums normally (or toggle it back on anytime in VITA SETTINGS), and all locked stadiums work cleanly.
- **Clean `2.0` version reporting.** Displays `02.00` (`2.0`) properly on the Vita system and VitaDB.

### Play in English, Español & Français
- Switch to **Spanish** or **French** in **VITA SETTINGS** and everything switches together: the North American disc's own game text plus every Vita Settings page, control guide, loading screen, error screen, and online menu.

### Stability, Saves & Under-the-Hood Polish
- **Crash-safe saves & cleaner I/O.** Saves, settings, and online account files now write safely with automatic backup and damaged-save recovery (`Strikers2.bad`), alongside full multi-thread file protection.
- **More resilient matches, movies & sleep/resume.** Prevents out-of-memory crashes (`C2-12828-1`) during heavy stadium intros, automatically falls back to software decoding if the Vita's hardware video decoder stalls during a cutscene, and recovers audio cleanly after long frames or suspend/resume.
- **Drop-in update.** Install 2.0 right over 1.0 or 1.1 — your disc files, saves, and settings stay intact, and older shader caches migrate automatically on first boot.

For developer notes and build provenance, see [Technical Notes](docs/TECHNICAL.md) and [Source Record](docs/SOURCE.md).

## Version 1.1 — October 24, 2026

The performance update.

**Smoother play**
- Matches run at up to 60 fps. The Frame Rate setting is now **AUTO** by
  default: 60 when the Vita can keep up, a smooth 30 when it can't. Prefer a
  locked 30? Choose **30 FPS** in Settings.
- Mega Strikes run at a steady 30 fps at full resolution (about 15 fps
  before). Their glowing haze is drawn at a lower resolution and blended over
  the sharp, full-size scene.
- Snappier controls: a third less delay from button to screen, about a
  tenth of a second in normal play. Stick and touch respond faster in Mega
  Saves and other heavy scenes too.
- The picture size adapts to the load: effects soften first and the scene
  only slightly, so the frame rate holds without the game looking rough.
- Fewer hitches from those size changes: they now happen at the cuts into
  and out of a Mega Strike, not a few seconds into play, and a size used
  recently is switched back to quickly.

**Better picture**
- A light sharpening filter is on by default (Settings > Sharpness: SMOOTH).
  OFF, CRISP and CRISP+ are still there.
- SMOOTH also softens jagged character outlines, at almost no cost.
- Optional HD characters and ball: sharper faces, kits and ball in close-ups
  (Mega Strikes, celebrations, menus), made on your computer from your own
  game files with `tools/hdtex` and copied to `ux0:data/mscharged/hdtex.pack`.
  In normal play the game draws the same detail as before, so it costs no
  frame rate.
- When there's time to spare, the game is drawn above native resolution for
  cleaner edges.
- New optional **Distant Textures** setting for sharper ground and walls at an
  angle (experimental, off by default).

**Controls and gameplay**
- Tap anywhere on the title screen to start.
- All seven camera zoom levels are on the Options > Visual slider.
- The CPU plays just as well when your camera is zoomed in close.
- Quick screen taps are never missed, and button presses are never lost or
  repeated after a system pop-up.
- The stick moves the glove three times faster in Mega Saves by default, so
  you can reach the far side of the goal in time (Settings > Mega Save Speed:
  3x, 4x, 2x or ORIGINAL). Optional rear-touch aiming is in Settings too.

**Under the hood**
- Fixed a crash when starting a match after the picture size had changed a
  few times: unused graphics buffers are now freed, and the game only draws a
  larger picture when there is graphics memory to spare.
- Mega Strikes no longer drop to a low resolution when that would not make
  them any faster.
- More of the game's graphics come prebuilt, so a goal replay no longer stops
  for a loading bar the first time.
- Fixed new graphics effects never being built on the console (a fault in
  the shader cache code), which kept the sharpening and the lighter Mega
  Strike glow from switching on.
- More reliable shader cache and graphics memory recovery.
- Faster texture uploads and less background CPU use.
- The game's 3D models are rebuilt less often after loading screens and
  pauses (improvements shared with the Super Mario Galaxy port).
- Players' faces in celebrations, replays and cut-scenes are skinned on the
  graphics chip like the rest of the body, so those scenes give the CPU less
  to do.

Technical details for developers are in
[Technical Notes](docs/TECHNICAL.md#version-11-changes).

## Version 1.0 — October 2026

The first public release of the native PS Vita port, and the updates that followed it in its first week.

**Launch**
- First public release of the native PS Vita port.

**Easier setup (October 18)**
- Simple copy-and-install steps, with Dolphin extraction instructions.
- Optional one-command game preparation helper.
- Full source, build instructions and license notices.

**Movies and a quieter start (October 19)**
- Smoother intro and stadium-preview movies with their audio intact.
- Faster start-up with no debug output.

**Settings in the LiveArea (October 19)**
- Open Settings from the LiveArea while the game is running, then jump back
  into your match.
- Save changes for next launch, or apply them now with Save and Restart.
- The game pauses while Settings is open.
