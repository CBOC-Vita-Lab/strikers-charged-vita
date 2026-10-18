# Building

For installation, use [Install](INSTALL.md). This page is for developers.

**A clean rebuild is not verified yet.** The recovered native source is the
`a8d4ad6` player build, but its export did not include the dependency-build
Makefile or the complete dependency checkout. Do not replace those pieces
with current upstream versions and call the result the same build.

## Source Layout

| Folder | Contents |
| --- | --- |
| `native/` | Game source, platform replacements, build rules and tests |
| `renderer/` | Native VGX renderer, Vita backend and third-party notices |
| `dependencies/vitaGL/source/` | Patched vitaGL implementation used by the build |

The recorded toolchain is Linux, VitaSDK GCC 15.2.0 and Clang 18.1.3.
Game objects use `-O2` ThinLTO, linked through GNU ld and LLVM's gold plugin.
The platform layer uses VitaSDK GCC. The original recipe is
`native/platform/vita/tools/job_vita_final.sh`; it also makes diagnostic
packages, so it is not an installer.

## Required Before Building

1. Recover and check the original `one/vita/Makefile.vitadeps` and pinned
   dependency recipes for vitaShaRK, SceShaccCgExt, math-neon and taiHEN.
   `native/platform/vita/tools/vitadeps_glb.sh` references these missing inputs.
   The `KUBRIDGE` header/export-description input referenced by `vita.mk`
   and the weak-stub generator also needs a pinned, licensed source copy.
2. Restore the complete vitaGL build context at the recorded `cdbba42` base.
   The recovered implementation already has the local patches applied;
   do not apply `native/platform/vita/vitagl/*.patch` to it again.
3. Build a dependency prefix containing `lib/` and `include/`. Match the
   original flags before comparing executables. Optional shader seeds must
   match the same renderer and vitaGL options.
4. Supply rights-cleared package artwork. The exported PNGs, fan art and
   loading image are deliberately not committed. `Makefile.vpk` still
   expects package art; its presence is a separate packaging prerequisite.

Once those inputs are verified, the native Makefile accepts this player
build shape, from the repository root:

```sh
export VITASDK=/opt/vitasdk
export PATH="$VITASDK/bin:$PATH"
export VDEPS=/absolute/path/to/verified-dependency-prefix
make -C native -j2 TARGET=vita WITH_SDK=1 WITH_GX=1 \
  BUILD_ROOT="$PWD/build/native" GX_VGX="$PWD/renderer" \
  VDEPS="$VDEPS" LTO=thin NP_PLAYER=1 \
  'OPT=-O2 -ffunction-sections -fdata-sections' DEBUG=-g0 vpk
```

This is a documented entry point, **not a tested end-to-end recipe**. The
normal output is `build/native/vita/pkg/np/mscharged-np.vpk`; check the
generated SFO for player title ID `MSCN00003`. Never install a lab package
over a working player installation.

## Verification

Run the small install checker tests with
`python3 -m unittest discover -s tests -v`. They validate the file checker,
not the game. A release still needs a clean build, exact source/dependency
receipt, license review, fresh install and save-preserving upgrade tests.
Do not run the old `native/platform/vita/session/` deployment scripts on a
working console as part of a source build.
