# Known Issues

A few things we're still working on. If you hit something not listed here,
please [open an issue](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/issues).

## Performance

- **Some menus run below 30 fps.** The main menu and stadium select are the
  slowest scenes in the game right now. Matches are not affected.
- **60 fps depends on the scene.** AUTO runs matches at 60 whenever the Vita
  can keep up and switches to 30 in the busiest moments. If you'd rather have
  a constant frame rate, set **Frame Rate** to **30 FPS** in Settings.
- **Sharpening and the lighter Mega Strike glow need the Vita's shader
  compiler while the game runs.** On some consoles it refuses to build new
  shaders; the game then shows the plain picture and draws the glow at full
  size, as version 1.0 did. Everything else works the same.
- **Distant Textures is experimental.** It is off by default. If ground or
  wall textures look wrong with it on, turn it back off.

## Gameplay

- **The CPU judges distance a little differently at zoom levels 4 to 7.** It
  now plays as if it could see the standard TV view, so zooming in no longer
  makes it weaker.
- **Some settings apply at next launch.** Use **Save and Restart** in Settings
  to apply them straight away.
- **Wider testing needed** for two-Vita local play, the Classic layout, rear
  touch and long career runs. Reports are very welcome.

## Install

- **The desktop installer may take a little while to list a new version.** If
  **Get** offers an older one, use the manual steps in the
  [install guide](INSTALL.md), or try again later.
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
