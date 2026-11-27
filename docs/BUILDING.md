# Building

For installation, use [Install](../MANUAL-INSTALL.md). This page is for developers.

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

Use Linux with Git, Python 3, CMake, Make, Clang 18.1.3, the LLVM 18 gold plugin and
VitaSDK GCC 15.2.0. Keep the repository and SDK paths free of spaces.
For a release, choose a new `BUILD_ROOT` that does not exist yet; never reuse an
incremental package root. From the repository root:

```sh
export VITASDK=/opt/vitasdk
export PATH="$VITASDK/bin:$PATH"
python3 tools/build-deps.py
python3 tools/build-mbedtls.py --target vita --work build/dependencies-tls13
export VDEPS="$PWD/build/dependencies/prefix"
export KUBRIDGE="$PWD/build/dependencies/sources/bythos14_kubridge"
export BUILD_ROOT="$PWD/build/release-1.3"
make -C native -j2 TARGET=vita WITH_SDK=1 WITH_GX=1 \
  BUILD_ROOT="$BUILD_ROOT" GX_VGX="$PWD/renderer" \
  VDEPS="$VDEPS" NP_MBEDTLS="$PWD/build/dependencies-tls13/mbedtls/vita" \
  LTO=thin NP_PLAYER=1 NP_LOGGING=0 NP_TITLE_ID=MSCN00003 NP_APP_VER=01.30 \
  "OPT=-O2 -ffunction-sections -fdata-sections -ffile-prefix-map=$PWD=. -ffile-prefix-map=$PWD/build=build" \
  DEBUG=-g0 vpk
```

The `-ffile-prefix-map` options keep your local folder names out of the executable.

`tools/build-deps.py` fetches the pinned source, rebuilds five libraries locally and
writes `build/dependencies/build-receipt.json`. It does not install anything
into VitaSDK or reset existing checkouts. To download source without
compiling, use `python3 tools/build-deps.py --fetch-only` on any supported host.
Rerunning is safe after an interrupted download. Each build recompiles the
libraries so changed headers or compiler options cannot leave stale objects.

`NP_LOGGING=0` selects the quiet player build. It disables runtime diagnostics
without suppressing ordinary game/save file writes.

The online-play HTTPS client (`native/platform/sdk/net/np_https*`) links Mbed TLS from a private prefix:
`python3 tools/build-mbedtls.py --target vita --work build/dependencies-tls13`, then add `NP_MBEDTLS="$PWD/build/dependencies-tls13/mbedtls/vita"` to the
make line. This is required for a release that uses the live HTTPS service; without it the client is built without TLS.
The stable 3.6.7 pin has one locked upstream TLS 1.3 allocation-failure cleanup overlay; its checkout remains unmodified. Changed TLS
provenance requires a new dependency work directory, preserving old prefixes/receipts. Use a new, empty `BUILD_ROOT` when changing TLS or
player-build options. See [ONLINE-TLS.md](ONLINE-TLS.md).

The vitaGL source is upstream vitaGL at the commit named by `vitagl_base` in
`dependencies/lock.json` with the patches in `native/platform/vita/vitagl/`
(0001 to 0036, in order, `patch -p1`) already applied. Do not apply them again.
The recipe builds an isolated copy with the recorded GLB options and an
8 MiB uniform ring. Source pins were recovered from reference checkouts;
they are not proof of byte-identical libraries in the installed executable.

Generic package images are included at the paths in
`native/platform/vita/Makefile.vpk`; [artwork.json](../store/artwork.json) records them.
Personal installed artwork and shader seeds are not committed.
The example build output is `build/release-1.3/vita/pkg/np/mscharged-np.vpk`, title ID
`MSCN00003`. It has no startup shader cache.

## Making The Shader Seed

The package carries compiled shaders (`shadercache/<key>.pack`) and a warm-up list (`vgx_shaderlist.bin`), so packaged variants can be loaded
without first-use compilation. This is not a guarantee that every possible variant is covered; the shader compiler remains a prerequisite.
The seed has to be made again whenever the renderer starts to make shaders the last
package does not hold (a new variant, a new shader, a changed generator or build option; 1.2 adds several). A shader that is missing from the
pack is compiled on first use and kept on the console, so a missing one is a hitch at best and, on a console without the compiler, an effect that
is not drawn (the failure paths fall back and keep the game running). Keys do not depend on the platform, so the shaders are found on the desktop and compiled in Vita3K, which writes
exactly the files vitaGL writes on the console. The tools are in `native/tools/seed/` (each script's first lines are its usage) and
`native/platform/vita/tools/make_shader_pack.py`; the seed scripts keep their work under the build root named at their top (`R=`), which is a
local path to change on another machine. `$R` below is this repository, `$W` an empty scratch folder with a few GiB free, `$PREV` the last
release package and `$BUILT` the new executable's package (`build/release-1.3/vita/pkg/np/mscharged-np.vpk` in the example above).

The scripts keep their work under `/work/msc/build/np/W5SEED` unless `SEED_ROOT=<folder>` is set (`rec.sh`, `batch.sh`, `v3k_warm.sh`, `v3k_proof.sh`,
`make_seed.sh`, `tiers.py`, `report.py`). The recordings run on the desktop, which does not read `vita_plan.cpp`: `. native/tools/seed/env_player.sh` before
`batch.sh` sets the switches the player build turns on by itself (the speed and quality lists, the multisampled rungs), because they change shader keys; keep that file
equal to the lists in `vita_plan.cpp`. The emulator's lab package reads `vita_plan.cpp` itself (a lab package is not a `MSC_LAB` build), so it needs no such line.

1. **What the last release already holds.** `mkdir -p $W/base && unzip -q -j $PREV 'shadercache/*' -d $W/base` gives `<key>.pack` and the old
   `vgx_shaderlist.bin`. The pack's file name is its cache key (`bc447993` for 1.0 and 1.1). A build with another vitaGL id string has another key (the move to
   vitaGL 9a14d17 made one; the line `[vgx-vita] compiled-shader cache: .../shadercache/<key>` of any log of the new build names it, and the
   next line names the older key of the same shaders), and then the new pack is filed under the new key; step 5 merges the old pack into it.
2. **The keys the new renderer makes.** Build the null renderer (`BUILD_ROOT=$W/h32 GX_VGX=$R/renderer native/tools/mp/build_host32.sh`, through
   `heavy.sh`), write the scenario scripts (`python3 native/tools/seed/gen_scripts.py --out $W/scripts`; matches in every stadium, captains,
   cheats, menus, cut-scenes), and record them: `SEED_OUT=$W/seed NP_DISC_DIR=<your extracted disc> native/tools/seed/batch.sh -X
   $W/h32/host32/np-host32-full-null $W/scripts/runs.txt` (repeat until it reports all runs done; a run is a few seconds to a minute). Every
   run leaves `$W/seed/rec/<name>.shl`; `python3 -I native/tools/seed/shl.py merge -o $W/catalogue.shl $W/seed/rec/*.shl` is the catalogue.
3. **The warm-up list.** `python3 -I native/tools/seed/tiers.py --rec $W/seed/rec --base $W/base/vgx_shaderlist.bin --share 0.10 --table --out-core
   $W/core.shl` writes the shaders used by one match in ten or more (what is linked at start-up); everything else is linked from the pack on
   first use.
4. **Compile what the pack lacks, in Vita3K.** The emulator runs the lab package (title `MSCN00001`, settings in `env.txt`, which the
   seed scripts write): the make line above with `NP_PLAYER=1` left out, `NP_LOGGING=1` and `BUILD_ROOT=$W/lab`, then
   `native/tools/seed/pack_vpk.sh <that .vpk> $W/base $W/lab-base.vpk` puts the old pack into it. Inside a `heavy.sh -t 1100` turn run
   `native/tools/seed/v3k_warm.sh -n 60 -s 900 -v $W/lab-base.vpk delta $W/catalogue.shl`: the game warms the whole catalogue at its first
   frame, finds what the old pack holds in the pack (`shader pack` lines) and compiles only the rest, into the emulator profile's cache. Its
   `log.txt` says `frame 1: N shader compiles`; N is the number of new shaders (the emulator needs Sony's shader compiler, `libshacccg.suprx`,
   in its profile; the scripts name where). If the list is too big for one turn, split it (`shl.py split --parts 3 -o $W/part
   $W/catalogue.shl`) and run the parts one after the other with `v3k_warm.sh -k` (keep the profile): the cache adds up.
   A list of about 450 programs links in one first frame in the emulator; one of 1,200 or more stalls it (the first frame takes a minute and the run can end in
   an emulator memory abort), so split any catalogue of that size, 3 parts for the 1.2 catalogue. `v3k_warm.sh -x "MSC_VITA_RENDER= ..."` leaves out the small
   fixed picture size, which the tool sets by default: the dynamic-resolution ladder and the multisampled rungs need none. The programs the back end makes by itself
   (picture, copy and clear programs: they are not in the lists) are compiled when the game first draws them, so play them once in the emulator with the warm-up list
   as in step 4 (`v3k_warm.sh -k -S <nav script> -x "MSC_VITA_RENDER= ..." <tag> core.shl`): `match60.txt` with `MSC_DYNRES_CYCLE=40` steps through every
   dynamic-resolution level, the 4x and 2x rungs among them; `native/tools/routes/mega_strike_route.txt` and `mega_route.txt` (with
   `MSC_WATCH=keepstate:s32:[[g_pTeams]+B4]+334`) the Mega Strike and Mega Save; a script that launches a friendly and waits for `score0 >= 1 || score1 >= 1` a goal with
   its replay and celebration. Each run leaves what it compiled in the profile; the count of `*.gxp` files after it says whether it found anything new.
5. **Assemble.** `native/tools/seed/assemble_seed.sh $W/v3k/pref $W/core.shl $W/pack --base $W/base --base-key bc447993` copies the profile's compiled shaders
   (`<profile>/pref/ux0/data/mscharged/shadercache/<key>/v3/{v,f}/<HH>/*.gxp`; the profile folder is `v3k` under the build root of `v3k_warm.sh`) and the warm-up list
   into a folder and runs `make_shader_pack.py` on it (`--base-key` is needed only when `<key>` is not the old pack's name: the 1.2 key is `874a13e0`, the old packs are
   `bc447993`). The line it prints says how many shaders are in the new pack and how many came from the old one (all of the old ones must be there; the count only
   grows).
6. **Package and prove.** `python3 tools/repack-release.py --app-version 01.20 --shadercache $W/pack $PREV $BUILT Strikers-Charged-Vita.vpk` (it replaces every
   `shadercache/` member of the old package, so no older key is left behind). To prove the pack, put it into the lab package
   (`native/tools/seed/pack_vpk.sh <lab .vpk> $W/pack $W/lab-final.vpk`) and install that fresh in Vita3K through a match:
   `native/tools/seed/v3k_proof.sh -v $W/lab-final.vpk -S native/platform/vita/session/nav/match60.txt proof` (inside `heavy.sh`). Its
   `cache.txt` must say that nothing was compiled by the shader compiler; if it names some, record that route with the step 2 scripts and
   repeat from step 4 with the profile kept (`-k`).

**1.3 notes.** There is no 32-bit host toolchain or `job_v3k.sh` in the 1.3 work sandbox, so steps 2 and 4 to 6 ran differently; the shape is the same. Step 2: the null renderer built as the armhf harness (`TARGET=armhf GX_BACKEND=null EXESUF=-full-null`, as in Profile-Guided Build) and run under `qemu-arm` through `rec.sh -X <wrapper>`, about 60 frames a second, after `. native/tools/seed/env_player.sh`, which now holds the 1.3 defaults that change keys. `MSC_VGX_VS_TRIM=1` puts light facts in the vertex key, so a list recorded with it on is not the 1.2 list; fragment forms stay plain in the list. Step 3 without `--base`: the 1.2 list's plain vertex keys are not what the defaults draw. Step 4: `shl.py sub --noprograms` gives a list of shader keys alone, which compiles without linking; boots to the title under the emulator's `run.sh`, repeated with the data kept while the log has the `MSC_VGX_WARM_HEAP_MIB` note, once per form (defaults, `MSC_VGX_TEX2D=1`, `MSC_VGX_FS_HALF=1`, `=2`). Picture programs: one boot per `MSC_VITA_UPSCALE` value, `smooth` and `cas` also with `MSC_VITA_EDGE_AA=0`. Step 5: the 1.2 pack (825) stays the `--base`, so a trim switched off still finds its form. Step 6: `native/tools/seed/gen_proof_route.py` writes the tour and then a match in each named stadium in one boot, and `native/tools/seed/pack_proof.sh` installs cold, plays it and writes `cache.txt`. The whole pack is read into the C heap at the start (`LoadPack`) and stays there, so its size comes off the same 64 MiB as everything else: that, not the package, is its budget.

**Pack 14 (1.3 top-up, Emulator-only workspace).** Same shape, with these tool facts: `b13x.sh` stages the isolated player build (title CDXMP0001, data root `ux0:data/mscharged-perf-test`, env through `np-player.txt`), so that build is also the "lab" package of step 4 (`pack_vpk.sh` it over the last pack). `v3k_warm.sh` and `pack_proof.sh` run through `v3k_emu.sh` (the headless emulator's `run.sh` with `V3K_PREF` / `V3K_XDG` / `V3K_RUNS` for a private profile; callers hold `emu.lock`), `assemble_seed.sh` reads `SEED_DATA` (default `mscharged-perf-test`). Warm the keys alone (`shl.py sub --noprograms`) with `-x "MSC_VGX_WARM_HEAP_MIB=0"`: the default 44 MiB stop is already reached at the first frame with the pack loaded, and a list of 180 new shaders compiles in two minutes in one boot; repeat with `-k` until the log's `frame 1` line shows no new GXP files. The warm-up list of a pack that serves `MSC_VGX_PALMERGE=2` must hold the 60-row palette keys: `shl.py palkeys -o core.shl --from catalogue.shl <tiers core>` adds those the recording found, as keys alone (their programs link from the pack on first use).

**Pack 15 (FS_TRIM keeps the final clamp).** Only fragment texts changed, not keys, so the warm-up list is pack 14's unchanged and nothing was recorded again: pack 14's catalogue keys (`catalogue-keys.shl`, current and proposed recordings merged) warmed over pack 14 in a fresh private profile (with the firmware folders `os0`, `sa0`, `vs0`, `ur0` copied into it: an empty profile aborts at boot) compiled 347 fragment programs and no vertex program; a second boot compiled none. `assemble_seed.sh <profile pref> pack14/vgx_shaderlist.bin pack15 --base pack14`: 2,425 shaders (2,078 kept), 2,652,208 bytes (+236 KiB of C heap at load).

**Top-up** after more renderer changes (a pack of this procedure is the next one's `--base`, filed under the key it already has, so no `--base-key`):
rebuild the null renderer and the lab package from the new head; `. native/tools/seed/env_player.sh`, then `SEED_ROOT=$W SEED_OUT=$W
native/tools/seed/batch.sh -X <null exe> $W/runs.txt` (the same runs) plus the two Mega routes, each one `rec.sh -X <null exe> -n mega_save -s native/tools/routes/mega_route.txt -f 8000 -S <save folder> -t 600 'MSC_WATCH=keepstate:s32:[[g_pTeams]+B4]+334'` and `... -n mega_strike -s native/tools/routes/mega_strike_route.txt -f 8000 -S <save folder> -t 600`, and `shl.py merge` of all `rec/*.shl` into the new catalogue;
put the last pack into the new lab package, `native/tools/seed/pack_vpk.sh <lab .vpk> $W/pack $W/lab-pack.vpk`; warm the catalogue's parts into an EMPTY profile
(`v3k_warm.sh -v $W/lab-pack.vpk ...` without `-k` for the first part, `-k` for the others; only what the last pack lacks is compiled); play the back end's programs
(step 4); then `assemble_seed.sh $W/v3k/pref $W/core.shl $W/pack2 --base $W/pack` and step 6 with `$W/pack2`.

A source build can use the same inputs without the release tools:
   `make ... NP_SHADER_SEED=$W/delta NP_SHADER_BASE=$W/base vpk` (an older key's pack in `$W/base` is then carried along beside the new one). Then update the hashes in `cboc-release.json`, `RELEASE.json` and
   `SHA256SUMS` as described below.

## Packaging A Release

A release is one VPK, `Strikers-Charged-Vita.vpk`. Settings are part of the game itself (the in-game
Settings screen, opened from the pause menu's VITA SETTINGS or OPTIONS > VITA SETTINGS; holding L + R at the start opens SAFE START, the recovery page); there is no separate
settings package. The VPK uses the previous release's generic artwork, the new executable
and notices, and a shader seed matching the graphics build. For 1.3:

```sh
python3 tools/repack-release.py --app-version 01.30 --shadercache <shader-seed-directory> \
  Strikers-Charged-Vita-previous.vpk \
  build/release-1.3/vita/pkg/np/mscharged-np.vpk Strikers-Charged-Vita.vpk
```

Versions are plain numbers everywhere: `APP_VER` 01.30, `"version": "1.3"` in `cboc-release.json`,
tag `v1.3`, the CHANGELOG heading `## Version 1.3 — <date>`. Always pass `--app-version`: without it
the donor's `APP_VER` (the previous release's) is copied.

The LiveArea images remain byte-identical to the donor. `--app-version` validates
the donor's SFO and changes only its five `APP_VER` payload bytes; title, content
identity and padding remain unchanged. Without that option the SFO is copied
unchanged. `--shadercache` replaces all donor shader-cache members, rather than
overlaying a new seed on old files; without it the donor cache is retained.
The shader cache must match the renderer and vitaGL
options; rebuild it only when those change or when the renderer makes shaders it does not hold (see Making The Shader Seed). The pack is filed under a key made
from the vitaGL id string (`options.txt`), the host's shader ABI and the shader
generator version (`ShaderCacheFolder`, `vgx_host_vita.cpp`). The move to vitaGL
9a14d17 changed the id string and so the key. The move on to 9bf3e3f and the draw
path options of the 1.2 A/B libraries do not: `options.txt` keeps the 9a14d17
identity and the base options on purpose (`Makefile.vitadeps`, `VGL_HASH`,
`VGL_BASE_OPTS`; the real commit and options go to `build-options.txt`), because
none of them changes what the shader compiler produces. The host also looks for the pack
(and for the compiled shaders already on a console) under the key of the 1.0 and
1.1 builds, as long as the build options, the ABI and the generator version are
unchanged (`LegacyCacheKey`), so an unchanged `shadercache/` is still found. Any
change to the options, the ABI or the generator makes the old key stop matching
and needs a new pack. A pack made with `tools/make_shader_pack.py` from a console
session reads both layouts (vitaGL 9a14d17 writes `v3/` with 16-digit names). The same inputs give the same
VPK bytes. Then update `cboc-release.json` and `RELEASE.json` (package and
runtime hashes, plus the old manifest hash in `appUpdateFrom`), and
regenerate `SHA256SUMS` and `docs/SOURCE-MANIFEST.json`. `tests/test_release_metadata.py`
checks that they agree.

## Profile-Guided Build

The game objects can be compiled with clang IR profile data on top of ThinLTO. The profile is recorded on the
armhf harness build under `qemu-arm`, because VitaSDK has no clang profile runtime and its GCC libgcov is
incomplete. armhf and the Vita share the ISA flags and almost all source (only `src/NL/nlMemory.cpp` and two headers
test `NP_TARGET_*`), so the profile applies to the Vita objects unchanged. `NP_PGO_ALL=1` (the setting of `record.sh` and of the
default Vita build below) instruments and profiles `native/src`, the renderer library (`renderer/src`, `renderer/native`, the front end
and the send path), the GX glue (`native/platform/gx`) and the platform layer (`native/platform/*`, the `pobj` objects). It is
`VGX_PGO=1 NP_PGO_PLAT=1`; either can be given alone. Not covered: vitaGL, which `dependencies/Makefile.vitadeps` builds with
VitaSDK's GCC (clang's profile format does not apply), and the Vita-only sources (`renderer/vita`, `platform/vita/src`), which the armhf
harness does not contain and which therefore compile without a profile.

```sh
# 1. the profile runtime for armhf (clang 18 ships none; compiler-rt's lib/profile, needs network once)
native/tools/pgo/build_runtime.sh build/pgo-runtime
# 2. instrumented armhf build, scripted routes under qemu-arm, merge (about 15 minutes on two cores)
NP_DISC_DIR=<game files> SAVE_DIR=<data folder holding a save> RT=$PWD/build/pgo-runtime/libclang_rt.profile-armhf.a \
  native/tools/pgo/record.sh build/pgo-armhf build/msc.profdata
# 3. the normal Vita build command above, plus
  NP_CLANG_PGO=$PWD/build/msc.profdata NP_PGO_ALL=1
#    names of internal-linkage functions in the renderer carry the path they were compiled from: map a profile recorded in one checkout onto
#    the tree a build compiles from first (game and platform objects use relative paths and need nothing):
  native/tools/pgo/rebase.sh build/msc.profdata <recorded checkout> <build tree> build/msc-rebased.profdata
```

`record.sh` runs the routes with the switches the Vita build turns on by itself (`native/tools/pgo/vita_env.py` reads `kSpeed` and `kQuality`
of `vita_plan.cpp`, so a new default is picked up by the next recording) and the 60 Hz thread layout of the console (`MSC_FPS=60`, front-end
and send threads), because the host harness does not run `vita_plan.cpp` and a profile recorded without them would not run the code paths the
defaults select. `PGO_ENV=<file>` overrides. The game units are compiled with `-Xclang -no-enable-noundef-analysis` (all builds): a profile
changes inlining, and the decompiled code has undefined behaviour (an uninitialised local passed by address) that an inlined copy can expose.
A Vita launch test of a profile-guided build is required before it is trusted.

**Sandbox Vita build script (opt-in).** `tools/build.sh <letter>` (`b13x.sh`) applies, when `PGO7=1` is set or the letter is `pg` (opt-in until a Vita3K launch test of a profile-guided build has passed), the profile in `/home/claude/s13/pgo/msc.profdata`
by itself: it rebases the renderer names from the checkout in `msc.profdata.root` onto the staged tree and adds `NP_CLANG_PGO=... NP_PGO_ALL=1`.
`NOPGO=1` in the environment builds without; `PGO_PROFILE=<file>` (with a `<file>.root`) uses another profile; naming `NP_CLANG_PGO=` in the
make variables overrides it. After merging source changes, re-record (`record.sh`, about an hour on the shared two cores) before a release.

The routes are `ROUTES` in `record.sh`: the 60 s open-play script, the survey, the console tour, a match of goals and replays, the Mega Save and Mega Strike routes of `native/tools/routes`, and `tools/pgo/megastrike.txt` (a harder CPU and long shot charges). Code that no route runs has a zero profile, which makes the compiler optimise it for size: online play, cups and most menus are in that group.

Without an armhf sysroot, a static musl link from the `ziglang` wheel plus libc++ 18 headers also records: every game function is compiled the same way, the libc is the only difference. `dlsym` does not exist there, so the libc clock interposers of `np_vclock.cpp` are off (`NP_NO_TIME_INTERPOSE`) and linker `--wrap` of musl's time64 entry points takes their place.

For 1.3, a candidate for a console A/B is the normal Vita build plus `OPT=-O3 NP_LTO_OLEVEL=3`, alone and then with `NP_CLANG_PGO=$PWD/build/msc.profdata`, each in a clean
`BUILD_ROOT` and with a profile recorded from the sources being built. `OPT` applies to the game and platform objects; `VGX_OPT` is the renderer's. It keeps strict floating point: clang does not auto-vectorise
floats for ARMv7 NEON without fast math, and the Cortex-A9 has no fused multiply-add. A larger image can lose on the instruction cache, and `-O3` gives the optimiser more room with the decomp's undefined behaviour (the note at `VCOMP` in `native/Makefile` says what that cost under GCC), so a lockstep pair of matches on the build is part of the test.

The profile is keyed by function name and a hash of its control flow. After the game sources change, functions whose
flow changed silently fall back to no profile; re-record before a release. To count them, compile with
`WARN=-Wno-everything NP_CLANG_PGO_WARN="-Wbackend-plugin -Wprofile-instr-out-of-date"` and search the per-file logs under
`<BUILD_ROOT>/<target>/log` for `hash mismatch`. Profiles are not committed (about 2 MB and tied to the exact sources).
The recording is deterministic (virtual clock), but counters of the helper threads are not atomic, so two recordings differ
slightly and results are close, not byte-identical, between profiles. PGO does not change what the game computes: the
simulation digest and the renderer frame hashes of the scripted match are identical with and without it.

## Optional Build Switches

| Make variable | Effect |
| --- | --- |
| `NP_LOGGING=1` | Runtime log in `ux0:data/mscharged/log.txt` (development; see [Diagnostics](DIAGNOSTICS.md)) |
| `NP_DIAG_GPU=1` | GPU-call timing for the `MSC_TOUCH_DIAG=1` recorder (adds `--wrap=sceGxm*`; use a clean `BUILD_ROOT`) |
| `LAB=1` | Lab server and scripted-run support (development only) |
| `NP_CLANG_PGO=<file.profdata>` | Compile the game objects (`native/src`; with `NP_PGO_ALL=1` the renderer and platform layer too) with clang profile data (see [Profile-guided build](#profile-guided-build)) |
| `NP_CLANG_PGO_GEN=1` | Instrumented game objects, used only to record that profile (armhf harness build) |
| `NP_PGO_ALL=1` | With either of the two above: the renderer library, the GX glue and the platform layer too (`VGX_PGO=1 NP_PGO_PLAT=1`) |
| `NP_MBEDTLS=<prefix>` | Link the HTTPS client against the Mbed TLS built by `tools/build-mbedtls.py` (default off: the client answers UNAVAILABLE; see [ONLINE-TLS.md](ONLINE-TLS.md)) |
| `VGX_OPT=-O3` | Extra optimisation flags for the renderer library and the GX glue, appended last so `-O3` wins over `-O2` (default none) |
| `VGX_LTO=thin` | Build the renderer library and the glue as ThinLTO bitcode (archived with `llvm-ar`) so they join the game's ThinLTO (default off) |
| `NP_LTO_OLEVEL=3` | Optimisation level of the LTO code generation (default `2`) |
| `VGL_EXTRA_OPTS=<flags>` | Extra defines for a vitaGL build of its own prefix, e.g. `DEPS_OUT=<dir> VGL_EXTRA_OPTS=-DGXM_STATE_CACHE`. The default is `-DGXM_STATE_CACHE -DSAMPLERS_SPEEDHACK -DINDICES_SPEEDHACK` (the `gsco` library the release build uses; the state cache is compiled in but off until `MSC_VGX_GXMCACHE=1`); an empty value builds the plain library. None of these changes the shader-cache key |

The 1.2 build options above, except the default `VGL_EXTRA_OPTS`, are for console A/B tests; none is on by default. Use a clean `BUILD_ROOT` when you switch one on or off.

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
