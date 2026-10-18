# Source Record

Snapshot imported on 2026-10-18 from the completed native player build,
not the older recompiled port or a later experiment.

| Item | Recorded value |
| --- | --- |
| Native revision | `a8d4ad6` |
| Renderer export | `W5INT-vgx`, recorded revision `47e8568` |
| Build time | 2026-10-18 03:49:32 UTC |
| Build label | `np-a8d4ad6-1017.234731` |
| Title ID | `MSCN00003` |
| Source export SHA-256 | `e4ab1ca7fcafc3f7fc11423412d63d4c21f4c1c1bd62d07f3f932597f786365a` |
| Recovered player eboot SHA-256 | `4d238f2af1fc1e02d8d42e2cbebfd2eef15b32aa524398ab5e988d6114331005` |
| Private candidate VPK SHA-256 | `a1eb4cfba10021af9af782bef47c00d9e1be1f5bc81507a2f1ce38dcae145f4c` |

The build manifest and recovered installed executable agree. That is
provenance evidence, not a clean-rebuild or gameplay test. The export has no
Git history; the new repository commit identifies the reviewed snapshot,
not the original build commit. [SOURCE-MANIFEST.json](SOURCE-MANIFEST.json)
records the imported source files and their current hashes.

Original upstream notices remain. Owner-specific names and paths were
normalized; private logs, session data, original Git metadata, screenshots,
package artwork, shader binaries and game data were left out. Current
guides replace the old lab notebooks. No game logic was deliberately changed.

The recovered dependency recipe is now included with fixed source revisions
in [dependencies/lock.json](dependencies/lock.json). It uses the exported
patched vitaGL tree and recorded GLB options, without changing game code.
The pins identify the recovered reference checkouts, not the exact archives
linked into the original executable.

See [Building](BUILDING.md) for build steps and
[Third-party Notices](THIRD_PARTY_NOTICES.md) for licensing boundaries.
The repository stays private until the remaining checks pass.
