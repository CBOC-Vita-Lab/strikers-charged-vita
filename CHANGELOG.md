# What's New

## Version 1.1 — October 23, 2026

The performance update.

**Smoother play**
- Matches run at up to 60 fps. The Frame Rate setting is now **AUTO** by
  default: 60 when the Vita can keep up, a smooth 30 when it can't. Prefer a
  locked 30? Choose **30 FPS** in Settings.
- Mega Strikes run at a steady 30 fps at full resolution (about 15 fps
  before). Their glowing haze is drawn at a lower resolution and blended over
  the sharp, full-size scene.
- Stick and touch respond faster in Mega Saves and other heavy scenes.
- The picture size adapts to the load: effects soften first and the scene
  only slightly, so the frame rate holds without the game looking rough.

**Better picture**
- A light sharpening filter is on by default (Settings > Sharpness: SMOOTH).
  OFF, CRISP and CRISP+ are still there.
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
- Optional rear-touch aiming and faster Mega Save stick speed in Settings.

**Under the hood**
- Fixed a crash when starting a match after the picture size had changed a
  few times: unused graphics buffers are now freed, and the game only draws a
  larger picture when there is graphics memory to spare.
- Fixed new graphics effects never being built on the console (a fault in
  the shader cache code), which kept the sharpening and the lighter Mega
  Strike glow from switching on.
- More reliable shader cache and graphics memory recovery.
- Faster texture uploads and less background CPU use.

Technical details for developers are in
[Technical Notes](docs/TECHNICAL.md#version-11-changes).

## October 19, 2026 — Settings Update

- Open Settings from the LiveArea while the game is running, then jump back
  into your match.
- Save changes for next launch, or apply them now with Save and Restart.
- The game pauses while Settings is open.

## October 19, 2026 — Movie and Quiet Updates

- Smoother intro and stadium-preview movies with their audio intact.
- Faster start-up with no debug output.

## October 18, 2026 — Easier Setup

- Simple copy-and-install steps, with Dolphin extraction instructions.
- Optional one-command game preparation helper.
- Full source, build instructions and license notices.

## Version 1.0

- First public release of the native PS Vita port.
