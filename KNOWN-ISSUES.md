# Known Issues And Test Limits

- Main and stadium-selection menus can run below 30 FPS. Older console
  measurements of those screens are not a performance guarantee for the
  final package.
- Loading-screen flicker and stadium-preview audio timing have code fixes;
  confirmation on the final Vita package is still pending.
- Camera level 7 completed an emulator match. Level 6 gameplay and cycling
  levels 5/6/7 through Options were not tested in that handoff.
- Two-Vita LAN, Classic controls, rear touch, save/update behavior and wider
  game progression need testing on the release candidate.
- The current native source, renderer and patched vitaGL files are included.
  The dependency recipe and pinned source setup are also included. Full
  linked-library provenance, combined-package licensing and fresh
  installation remain open requirements.
- The retained executable contains original Linux build-workspace paths.
  Binary privacy review remains open; repackaging art does not remove them.
- The runtime requires the owner's shader compiler module even when the
  startup cache is included. It is not part of the package.

No whole-game, universal compatibility or locked-framerate claim is made.
