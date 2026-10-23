# Known Issues And Test Limits

## This release (2026-10-23)

- **Not yet played on a physical Vita.** It was built from source, played in
  the Vita3K emulator (boot, menus, the Strikers 101 Mega Saves lesson and
  Options > Visual), and passes the host tests. Emulator timing says nothing
  about console speed. A console check of Mega Saves, a full match, the title
  tap and the zoom slider is the next step.
- **Mega Saves speed is an estimate.** The previous build measured about 15 FPS
  in Mega Saves on a console. The new effect drawing should remove roughly half
  of that scene's blending work. If the haze looks wrong, set `MSC_FX_THIN=0`
  in `ux0:data/mscharged/np-player.txt`.
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
