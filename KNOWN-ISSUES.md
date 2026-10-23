# Known Issues And Test Limits

## This release (2026-10-23)

- **Not yet played on a physical Vita.** It was built from source, played in
  the Vita3K emulator (boot, menus, the Strikers 101 Mega Saves lesson and
  Options > Visual), and passes the host tests. Emulator timing says nothing
  about console speed. A console check of Mega Saves, a full match, the title
  tap and the zoom slider is the next step.
- **Mega Saves speed is an estimate.** The previous build measured about 15 FPS
  in Mega Saves on a console. Two changes target 30: the effect drawing removes
  roughly half of that scene's blending work, and the scene renders at a lower
  internal size (49 % of the pixels, down to 25 % if it is still below 30 fps).
  Mega Strike scenes are therefore a little softer than the rest of the match.
  `MSC_MEGA_RENDER=off` restores full size and `MSC_FX_THIN=0` the original
  haze, both in `ux0:data/mscharged/np-player.txt`. The resolution change has
  not been run in the emulator: the cloud emulator's shader compiler failed on
  the host available that day, for this and the previous release alike.
- **The AI plays differently at zoom levels 4-7.** It now judges which players
  are "on screen" from about the TV-default view. `MSC_CAMERA_VIS=screen`
  brings back the real screen test.
- **The desktop installer (0.1.11) does not know this release yet.** It trusts
  releases it has listed. Use the manual VitaShell steps until an installer
  update lists this release.

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
