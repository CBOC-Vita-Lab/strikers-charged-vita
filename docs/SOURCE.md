# Source Record

Versions are plain numbers: the game's `APP_VER` is `02.01` for 2.0.1, `cboc-release.json` says `"version": "2.0.1"`,
and the release is tagged `v2.0.1`. Every release is one VPK, `Strikers-Charged-Vita.vpk`, with Settings inside the
game (the in-game Settings screen, opened from the pause menu's VITA SETTINGS or OPTIONS > VITA SETTINGS, title ID `MSCN00003`).

## Version 2.0.1 (`strikers-v2.0.1`)

| Item | Value |
| --- | --- |
| Package | `Strikers-Charged-Vita.vpk` |
| Title ID | `MSCN00003` (same app as every earlier release; installs as an update) |
| `APP_VER` | `02.01` |
| Executable SHA-256 | `830a8cf90e2463ed28d562825b6e38a1e9754d66480d08d0887f4622d4303ccd` |
| VPK SHA-256 | `ba5c6874c164cb9d34950f8ad0c855cc048b50514a7e42c7dd2ae011a7cfe128` |
| Shader pack | `shadercache/874a13e0.pack` (4,281 compiled Vita shaders) + `vgx_shaderlist.bin` (`core33`: 830 marked boot programs, 238 `pal60` keys) |
| Player build flags | `NP_PLAYER=1 NP_LOGGING=0 NP_APP_VER=02.01 NP_BUILD_ID=d3eabe5f5305c345 NP_PGO_ALL=1`, ThinLTO `-O2` + ARMv7 NEON PGO |
| Toolchain | VitaSDK GCC 15.2.0, Clang 18.1.3 |

2.0.1 adds 2–4 player same-console local multiplayer on PlayStation TV and handheld Vita with extra paired Bluetooth controllers (e.g. `MiniVitaTV`), DualShock 3/4 `L2`/`R2` trigger bindings, per-player rumble (`sceCtrlSetActuator`), live controller hot-plugging/reconnection, and full CI test suite fixes.
It preserves `2.0`'s exact simulation and `NP_BUILD_ID=d3eabe5f5305c345` link fingerprint so `2.0` and `2.0.1` consoles remain 100% cross-compatible over Online and Local Wi-Fi / Nearby play.
The package was made with `tools/repack-release.py --app-version 02.01 --shadercache <pack33>`.
`appUpdateFrom` includes the 2.0 (`d7bcec2f...`) and 1.1 (`8d58eea7...`) executables so updating preserves game files, saves, and settings.

Verification for this release: full host & 32-bit CI suites (`tests/`, `tests/test_multipad_cpp.py`, `native/platform/sdk` `host32` + `PLATFAKE=1`, `capture_lifecycle`, `shader_forms`, settings & runtime suites, `verify_vpk.py`, `public_scan.py`), ELF symbol/hook verification (`check_elf_hooks.py`), and physical PS Vita installation, read-back SHA-256 verification, and clean boot/settings testing.

## Version 2.0 (`strikers-v2.0`)

| Item | Value |
| --- | --- |
| Package | `Strikers-Charged-Vita.vpk` |
| Title ID | `MSCN00003` (same app as every earlier release; installs as an update) |
| `APP_VER` | `02.00` |
| Executable SHA-256 | `d7bcec2fbab91f232586e453c45462bce27bf1b24c8cdc19562f6d138fe280f7` |
| VPK SHA-256 | `4d7793aa583f38a30c09c4278e0f48c1a5a393c669add18239e257c1fbfc743f` |
| Shader pack | `shadercache/874a13e0.pack` (4,281 compiled Vita shaders) + `vgx_shaderlist.bin` (`core33`: 830 marked boot programs, 238 `pal60` keys) |
| Player build flags | `NP_PLAYER=1 NP_LOGGING=0 NP_APP_VER=02.00 NP_PGO_ALL=1`, ThinLTO `-O2` + fresh ARMv7 NEON PGO |
| Toolchain | VitaSDK GCC 15.2.0, Clang 18.1.3 |

2.0 is the major release that follows 1.1. 1.2 was distributed briefly and then withdrawn, so 2.0 carries both:
the 1.2 changes (the Settings pages, online play beta, the adaptive picture work, the third-party notices) and the
2.0 smoothness work, full-game shader pack (`pack33`), fresh ARMv7 NEON PGO profile, Mega Strike fixes, and Spanish and French for the port's own menus and messages.
The package was made with `tools/repack-release.py --app-version 02.00 --shadercache <pack33>` with LiveArea `template.xml` upgraded to `content-rev="5"` (no Settings tile).
`appUpdateFrom` lists the 1.1 executable (`8d58eea7...`), so 1.1 updates in place while preserving game files, saves and settings.

Verification for this release: host tests (`tests/`, including `test_livearea_art_cpp.py` and `test_bootart_cpp.py`, SDK unit tests, native settings & runtime suites, `verify_vpk.py`, `public_scan.py`),
Vita3K runs, and physical PS Vita installation, read-back SHA-256 verification, first-boot on-device loading screen (`vita_bootart.cpp`) and LiveArea/bubble (`vita_livearea_art.cpp`) generation, full-game shader compilation & warm-up, Mega Strike/Save A/B testing, and live playtesting.

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
the October 19, 2026 update of 1.0. Only `eboot.bin` and `licenses/` come from the
new build.

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
the rendering and frame-rate changes. It had **not** been run on a physical Vita
before release.

## Version 1.0 and its October updates

| Item | Value |
| --- | --- |
| Package | One VPK of the same app, `MSCN00003` (published then under a dated file name; from 1.1 on it is `Strikers-Charged-Vita.vpk`). The Settings update added the LiveArea Settings button to the game itself |
| Executable SHA-256 (October 19 update) | `0243f69693d1fc72b0629558c68d6eac990ca7a39d29f2c84c6f3d56ce7143a4` |
| VPK SHA-256 (October 19 update) | `43543e5bce2f162f6678bc2baacbdcbeb03a4593493c0e401a5ad6534623400a` |

That executable was installed on a physical Vita through a guarded
executable-only update and read back. Saves and settings were preserved, and
LiveArea Settings opened, returned and reopened. Its source is the commit history
up to October 19, 2026.
