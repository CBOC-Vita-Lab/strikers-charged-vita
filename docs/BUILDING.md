# Building

For installation, use [Install](../INSTALL.md). This page is for developers.

Build the native player (title `MSCN00003`) from this tree. Dependency setup uses
fixed revisions rather than whichever upstream versions are newest.

## Source Layout

| Folder | Contents |
| --- | --- |
| `native/` | Game source, platform replacements, build rules and tests |
| `renderer/` | Native VGX renderer, Vita backend and third-party notices |
| `dependencies/vitaGL/source/` | Patched vitaGL implementation used by the build |
| `dependencies/lock.json` | Pinned dependency sources |
| `dependencies/Makefile.vitadeps` | Recovered library-build recipe |

The recorded toolchain is Linux, VitaSDK GCC 15.2.0 and Clang 18.1.3.
Game objects use `-O2` ThinLTO, linked through GNU ld and LLVM's gold plugin.
The platform layer uses VitaSDK GCC. The original recipe is
`native/platform/vita/tools/job_vita_final.sh`; it also makes diagnostic
packages, so it is not an installer.

## Build

Use Linux with Git, Python 3, Make, Clang 18.1.3, the LLVM 18 gold plugin and
VitaSDK GCC 15.2.0. Keep the repository and SDK paths free of spaces.
From the repository root:

```sh
export VITASDK=/opt/vitasdk
export PATH="$VITASDK/bin:$PATH"
python3 BUILD-DEPS.py
export VDEPS="$PWD/build/dependencies/prefix"
export KUBRIDGE="$PWD/build/dependencies/sources/bythos14_kubridge"
make -C native -j2 TARGET=vita WITH_SDK=1 WITH_GX=1 \
  BUILD_ROOT="$PWD/build/native" GX_VGX="$PWD/renderer" \
  VDEPS="$VDEPS" LTO=thin NP_PLAYER=1 NP_LOGGING=0 \
  "OPT=-O2 -ffunction-sections -fdata-sections -ffile-prefix-map=$PWD=. -ffile-prefix-map=$PWD/build=build" \
  DEBUG=-g0 vpk
```

The `-ffile-prefix-map` options keep your local folder names out of the executable.

`BUILD-DEPS.py` fetches the pinned source, rebuilds five libraries locally and
writes `build/dependencies/build-receipt.json`. It does not install anything
into VitaSDK or reset existing checkouts. To download source without
compiling, use `python3 BUILD-DEPS.py --fetch-only` on any supported host.
Rerunning is safe after an interrupted download. Each build recompiles the
libraries so changed headers or compiler options cannot leave stale objects.

`NP_LOGGING=0` selects the quiet player build. It disables runtime diagnostics
without suppressing ordinary game/save file writes.

The vitaGL source already has the exported patches. Do not apply them again.
The recipe builds an isolated copy with the recorded GLB options and an
8 MiB uniform ring. Source pins were recovered from reference checkouts;
they are not proof of byte-identical libraries in the installed executable.

Generic package images are included at the paths in
`native/platform/vita/Makefile.vpk`; [ARTWORK.json](../ARTWORK.json) records them.
Personal installed artwork and shader seeds are not committed.
The build output is `build/native/vita/pkg/np/mscharged-np.vpk`, title ID
`MSCN00003`. It has no startup shader cache.

## Packaging A Release

A release VPK is the previous release's package with the new executable and
notices:

```sh
python3 tools/repack-release.py Strikers-Charged-Vita-previous.vpk \
  build/native/vita/pkg/np/mscharged-np.vpk Strikers-Charged-Vita.vpk
```

This keeps the startup shader cache (`shadercache/`), LiveArea images and
`param.sfo` byte for byte. The shader cache matches the renderer and vitaGL
options; rebuild it only when those change. The same inputs give the same
VPK bytes. Then update `cboc-release.json` and `RELEASE.json` (package and
runtime hashes, plus the old manifest hash in `appUpdateFrom`), and
regenerate `SHA256SUMS` and `SOURCE-MANIFEST.json`. `tests/test_release_metadata.py`
checks that they agree.

## Optional Build Switches

| Make variable | Effect |
| --- | --- |
| `NP_LOGGING=1` | Runtime log in `ux0:data/mscharged/log.txt` (development; see [Diagnostics](DIAGNOSTICS.md)) |
| `NP_DIAG_GPU=1` | GPU-call timing for the `MSC_TOUCH_DIAG=1` recorder (adds `--wrap=sceGxm*`; use a clean `BUILD_ROOT`) |
| `LAB=1` | Lab server and scripted-run support (development only) |

## Verification

Run `python3 -m unittest discover -s tests -v` for the file checker,
game-preparation, dependency-setup and release-metadata tests, and
`make -C native/platform/sdk T=host32 check` for the SDK unit tests. GitHub
Actions runs both on every push. These are host tests, not gameplay tests.
A release still needs console play, a fresh install and a save-preserving
upgrade test. Do not run the old `native/platform/vita/session/` deployment
scripts on a working console as part of a source build.

[Movie tests](../tests/movie/README.md) cover the scheduler and real decoder,
including audio-preserving skips, retries and mixer exclusion.
