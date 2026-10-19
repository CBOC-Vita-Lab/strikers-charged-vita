# Building

For installation, use [Install](INSTALL.md). This page is for developers.

Build the `a8d4ad6` native player plus the accepted movie update. Dependency
setup uses fixed revisions rather than whichever upstream versions are newest.

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
  VDEPS="$VDEPS" LTO=thin NP_PLAYER=1 \
  'OPT=-O2 -ffunction-sections -fdata-sections' DEBUG=-g0 vpk
```

`BUILD-DEPS.py` fetches the pinned source, rebuilds five libraries locally and
writes `build/dependencies/build-receipt.json`. It does not install anything
into VitaSDK or reset existing checkouts. To download source without
compiling, use `python3 BUILD-DEPS.py --fetch-only` on any supported host.
Rerunning is safe after an interrupted download. Each build recompiles the
libraries so changed headers or compiler options cannot leave stale objects.

The vitaGL source already has the exported patches. Do not apply them again.
The recipe builds an isolated copy with the recorded GLB options and an
8 MiB uniform ring. Source pins were recovered from reference checkouts;
they are not proof of byte-identical libraries in the installed executable.

Generic package images are included at the paths in
`native/platform/vita/Makefile.vpk`; [ARTWORK.json](ARTWORK.json) records them.
Personal installed artwork and shader seeds are not committed.
Seeds must match the renderer and vitaGL options.
The normal output is `build/native/vita/pkg/np/mscharged-np.vpk`, title ID
`MSCN00003`. This source-build path must pass the verification below before
its output could be considered for installation. The selected movie-update
package uses the approved production rebuild, retaining the existing shader
seeds and generic app graphics. See [Source Record](SOURCE.md) for its hashes;
a separate build is not automatically byte-identical to that executable.

## Verification

On 2026-10-18, the five dependency libraries built successfully with this
helper. That initial baseline rebuild was not selected. On 2026-10-19 the
approved movie source was built for `MSCN00003`, installed through the guarded
executable-update procedure and verified by readback. Its exact executable is
now selected. This does not establish full VPK or desktop-installer acceptance.

Run `python3 -m unittest discover -s tests -v` for the file checker,
game-preparation and dependency-setup tests. These are host tests, not
gameplay tests. A release still needs an exact source/dependency
receipt, license review, fresh install and save-preserving upgrade tests.
Do not run the old `native/platform/vita/session/` deployment scripts on a
working console as part of a source build.

[Movie tests](tests/movie/README.md) cover the scheduler and real decoder,
including audio-preserving skips, retries and mixer exclusion.
