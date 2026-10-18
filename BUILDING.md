# Building

For installation, use [Install](INSTALL.md). This page is for developers.

Build the `a8d4ad6` native player from the source included here. Dependency
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

Packaging also needs rights-cleared artwork at the paths in
`native/platform/vita/Makefile.vpk`. Original package images and shader seeds
are not committed. Seeds must match the renderer and vitaGL options.
The normal output is `build/native/vita/pkg/np/mscharged-np.vpk`, title ID
`MSCN00003`. This source-build path must pass the verification below before
its output replaces a working installation.

## Verification

On 2026-10-18, the five dependency libraries built successfully with this
helper. The recovered recipe also compiled and linked the native player,
produced its Vita executable and generated the `MSCN00003` SFO. Packaging
then stopped at the excluded `icon0-test.png`. The rebuilt executable is
not byte-identical to the recovered one; the existing VPK was not replaced.

Run `python3 -m unittest discover -s tests -v` for the file checker,
game-preparation and dependency-setup tests. These are host tests, not
gameplay tests. A release still needs exact source/dependency
receipt, license review, fresh install and save-preserving upgrade tests.
Do not run the old `native/platform/vita/session/` deployment scripts on a
working console as part of a source build.
