# Source Record

## Version 1.1 (`strikers-v1.1`)

| Item | Value |
| --- | --- |
| Package | `Strikers-Charged-Vita.vpk` |
| Title ID | `MSCN00003` (same app as earlier releases; installs as an update) |
| Executable SHA-256 | `8d58eea7fa849a1220902efe6732e300446c5c6b859e353c1f0c70fdc44d599d` |
| VPK SHA-256 | `1854ef4b163aace32b10b70a86a289c5e137802cf34b71569ac83a1b9595218f` |
| Player build flags | `NP_PLAYER=1 NP_LOGGING=0`, ThinLTO `-O2` |
| Toolchain | VitaSDK GCC 15.2.0, Clang 18.1.3 |

The executable was built from this tree with the command in
[Building](BUILDING.md). The package was made with
`tools/repack-release.py` from the previous release's VPK, so the startup
shader cache, LiveArea images and `param.sfo` are byte-identical to
`strikers-settings-20261019`. Only `eboot.bin` and `licenses/` come from the
new build. [RELEASE.json](RELEASE.json) lists the members that were carried
over.

The source is the previous release plus the performance-test line developed on
18-23 Oct (shader cache, allocation recovery, input, diagnostics), the Mega
Saves, title-touch, zoom-slider and AI-visibility changes, dynamic resolution,
low-resolution effects, the AUTO frame rate, the adaptive GPU queue and the
build-time switch for the GPU-call diagnostics. The commit history shows each step.
[SOURCE-MANIFEST.json](SOURCE-MANIFEST.json) lists every source file with
its hash.

Verification for this release: host tests (`tests/` and the SDK unit tests),
Vita3K runs of start-up, menus, the Strikers 101 Mega Saves lesson and
Options > Visual, and desktop-harness runs of menus, a match and Mega Saves for
the rendering and frame-rate changes. It has **not** been run on a physical Vita yet; see
[Known Issues](../KNOWN-ISSUES.md).

## Previous release `strikers-settings-20261019`

| Item | Value |
| --- | --- |
| Package | `Strikers-settings-20261019.vpk` |
| Executable SHA-256 | `0243f69693d1fc72b0629558c68d6eac990ca7a39d29f2c84c6f3d56ce7143a4` |
| VPK SHA-256 | `43543e5bce2f162f6678bc2baacbdcbeb03a4593493c0e401a5ad6534623400a` |

That executable was installed on a physical Vita through a guarded
executable-only update and read back. Saves and settings were preserved, and
LiveArea Settings opened, returned and reopened. The tree at tag
`strikers-settings-20261019` is its source.
