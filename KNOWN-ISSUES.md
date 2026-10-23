# Known Issues And Test Limits

## This release (2026-10-23)

- **Not yet played on a physical Vita.** It was built from source, played in
  the Vita3K emulator (boot, menus, the Strikers 101 Mega Saves lesson and
  Options > Visual), and passes the host tests. Emulator timing says nothing
  about console speed. A console check of Mega Saves, a full match, the title
  tap and the zoom slider is the next step.
- **Mega Saves speed is an estimate.** The previous build measured about 15 FPS
  in Mega Saves on a console. The glow and haze are now drawn at a quarter of
  the pixels, which by estimate removes about three quarters of that scene's
  blending work, with the scene itself at full size. If that is not enough the
  picture size still drops (down to 50 % of each side). `MSC_FX_LOWRES=0` and
  `MSC_MEGA_RENDER=off` in `ux0:data/mscharged/np-player.txt` turn the two off.
  The cloud emulator could not run this release (its shader compiler failed on
  the machine available that day, for the previous release too).
- **AUTO 60 fps and the dynamic resolution are new and untested on a
  console.** They were tested on the desktop harness, with a simulated GPU
  time and a stress test that changes the picture size every 7 frames through
  menus, a match and Mega Saves. How often a match reaches 60 depends on the
  CPU as well as the GPU; the controller drops to 30 when either cannot keep
  up. If anything looks wrong, `MSC_FPS=30` and `MSC_DYNRES=off` in
  `np-player.txt` restore the earlier behaviour.
- **The AI plays differently at zoom levels 4-7.** It now judges which players
  are "on screen" from about the TV-default view. `MSC_CAMERA_VIS=screen`
  brings back the real screen test.
- **The desktop installer may not list this release yet.** CBOC Vita Installer
  trusts the releases in its catalog. If **Get** offers an older version, use
  the manual VitaShell steps until the catalog lists this one.

## Still open from earlier releases

- Main and stadium-selection menus can run below 30 FPS (graphics-bound).
- Saved port settings apply at the next launch. Use **Save and Restart** to
  apply them now.
- Two-Vita LAN, Classic controls, rear touch and longer progression need wider
  testing.
- Full linked-library provenance and combined-package licensing review remain
  open. The runtime needs the owner's shader compiler module, which is not
  included.

The performance description is what testing has reached so far, not a promise
that every scene runs at the same frame rate.
