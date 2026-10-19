# Known Issues And Test Limits

- The movie, quiet-logging and LiveArea settings updates are included. The
  executable was installed and read back on a physical Vita, preserving saves,
  settings and artwork. Opening, returning from and reopening Settings were
  checked on that normal-title build. Full VPK/desktop installation and a
  complete-match playthrough of this release remain open. No new tests were
  run when packaging this update.
- Saved port settings take effect on the next launch. Use **Save and Restart**
  to apply them now. The experimental independent interpolation presenter is
  not supported by the settings overlay and remains disabled by default.
- Main and stadium-selection menus can run below 30 FPS. Older console
  measurements of those screens are not a performance guarantee for the
  final package.
- Movie improvements have been checked by the tester. Wider playback and
  progression testing is still needed; this is not a claim of flawless audio
  or a guaranteed frame rate in every scene.
- Camera level 7 completed an emulator match. Level 6 gameplay and cycling
  levels 5/6/7 through Options were not tested in that handoff.
- Two-Vita LAN, Classic controls, rear touch, save/update behavior and wider
  game progression need testing on the release candidate.
- The current native source, renderer and patched vitaGL files are included.
  The dependency recipe and pinned source setup are also included. Full
  linked-library provenance, combined-package licensing and fresh
  installation remain open requirements.
- The executable retains generic Linux build-workspace paths. Their exact
  bytes were reviewed separately from the private artwork, which is excluded.
- The runtime requires the owner's shader compiler module even when the
  startup cache is included. It is not part of the package.

The performance description is what testing has reached so far, not a promise
that every scene runs at the same frame rate.
