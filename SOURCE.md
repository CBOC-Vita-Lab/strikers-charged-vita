# Source Record

The selected build adds LiveArea settings to the quiet-logging update, based on the completed
`a8d4ad6` native player. It is not the isolated movie-test application.

## Selected Settings Update

| Item | Recorded value |
| --- | --- |
| Package | `Strikers-settings-20261019.vpk` |
| Title ID | `MSCN00003` |
| Native base / renderer | `a8d4ad6` / `47e8568` |
| Executable SHA-256 | `0243f69693d1fc72b0629558c68d6eac990ca7a39d29f2c84c6f3d56ce7143a4` |
| VPK SHA-256 | `43543e5bce2f162f6678bc2baacbdcbeb03a4593493c0e401a5ad6534623400a` |
| Player build flags | `NP_PLAYER=1 NP_LOGGING=0` |

[RELEASE.json](RELEASE.json) binds the source changes to this executable and
records the unchanged package members. All 4,004 native/renderer files match
the production source. Fourteen existing files and one new clock header add
the settings fix to the quiet build. The five movie-fix files, silent logging,
normal title ID and data directory are preserved.

Previously recorded checks on 2026-10-19 covered settings during boot, intro,
menus and a live match in a separate diagnostic app. The normal-title quiet
executable was then installed and read back. All 16 protected art, metadata,
shader, settings and save files matched their pre-update hashes. Settings
opened, returned to the intro and reopened without changing the process or log.

That was an executable-only update, not a full VPK or desktop-installer
installation. Those checks, a full-match playthrough and wider controls
acceptance remain open. Earlier settings, timing, audio, movie and silent-I/O
regressions are recorded in the development handoff. No new tests or Vita
changes were made for this packaging update; the existing runtime was not rebuilt.

The shared package uses the same executable with the previously reviewed
generic art, notices and shader seeds. No personal fan art, game data,
proprietary shader compiler, device address or save is included.

## Original Import

The original native snapshot was imported on 2026-10-18. These hashes identify
the older baseline, not the selected movie update:

| Item | Recorded value |
| --- | --- |
| Native revision | `a8d4ad6` |
| Renderer export | `W5INT-vgx`, recorded revision `47e8568` |
| Build time | 2026-10-18 03:49:32 UTC |
| Build label | `np-a8d4ad6-1017.234731` |
| Title ID | `MSCN00003` |
| Source export SHA-256 | `e4ab1ca7fcafc3f7fc11423412d63d4c21f4c1c1bd62d07f3f932597f786365a` |
| Recovered player eboot SHA-256 | `4d238f2af1fc1e02d8d42e2cbebfd2eef15b32aa524398ab5e988d6114331005` |
| Private candidate VPK SHA-256 | `201c4bb5ec892ba18fd58ad2bd8053be32c0356ee5e3f207448d0e011c4d4298` |

The original build manifest and recovered installed executable agree. That is
provenance evidence, not a clean-rebuild or gameplay test. The export has no
Git history; the new repository commit identifies the reviewed snapshot,
not the original build commit. [SOURCE-MANIFEST.json](SOURCE-MANIFEST.json)
records the imported source files and their current hashes.

Original upstream notices remain. Owner-specific names and paths were
normalized; private logs, session data, original Git metadata, screenshots,
installed artwork, shader binaries and game data were left out. Current
guides replace the old lab notebooks. Later movie, quiet-logging and settings
changes are listed in `RELEASE.json`; gameplay control mappings are unchanged.
Generic project images replace personal
fan art, and the existing built-in loading screen is used. See [Included Files](ASSETS.md).

The recovered dependency recipe is now included with fixed source revisions
in [dependencies/lock.json](dependencies/lock.json). It uses the exported
patched vitaGL tree and recorded GLB options, without changing game code.
The pins identify the recovered reference checkouts, not the exact archives
linked into the original executable.

See [Building](BUILDING.md) for build steps and
[Third-party Notices](THIRD_PARTY_NOTICES.md) for licensing boundaries.
The repository and selected release are public at the publisher's request.
The outstanding licensing and installation checks remain documented, not waived.
