# Third-party Notices

There is no blanket license covering this entire mixed-source snapshot.
Keep file-level notices and the license texts with any permitted copy.

| Component | Provenance and terms |
| --- | --- |
| Reconstructed game/source base | [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp/tree/8cd3c6d2cdd5aaf2147fa60efcac6be5a293452b), imported base `8cd3c6d`; original contributions CC0, not a license to game assets or all third-party content |
| Upstream third-party code | Pinned upstream texts in `licenses/`: Apache 2.0, GameSpy BSD-3-Clause and ODE BSD; zlib retains its source notice |
| Native renderer | Retains `renderer/third_party/LICENSE-aurora`, `LICENSE-stb` and `LICENSE-xxhash`, plus file notices |
| vitaGL | Rinnegatamante and contributors; statically linked. The source files' headers say LGPL version 3 or later (the terms the author also states), `licenses/LGPL-3.0.txt`, which builds on `licenses/GPL-3.0.txt`. This tree has no upstream licence file: see the owner's items below. The tree is modified: upstream at `vitagl_base` plus patches 0001 to 0017, and `rt.c`, a file of this port without a licence header. Other people's code inside it: `licenses/vitaGL-bundled-notices.txt`, Apache-2.0 (`etc1_utils.c`), xxHash and stb_dxt (`LICENSE-xxhash`, `LICENSE-stb`); `utils/preprocessor` credits [john-blackburn/preprocessor](https://github.com/john-blackburn/preprocessor) and states no licence |
| vitaShaRK | Rinnegatamante; statically linked, unmodified; LGPL-3.0-or-later (repository `LICENSE` and file headers), `licenses/LGPL-3.0.txt`; includes xxHash under its retained BSD notice |
| SceShaccCgExt | bythos14 and contributors; statically linked, unmodified; GPL-3.0 (repository `LICENSE`), `licenses/GPL-3.0.txt`. Not LGPL: see the owner's items below |
| taiHEN import stubs | Generated from Yifan Lu's pinned exports; MIT, `licenses/taiHEN-MIT.txt` |
| kubridge interface definitions | bythos14 and contributors; [pinned header and exports](https://github.com/bythos14/kubridge/tree/417ddde9a744eba98d769382c1c1372b8e119139); no license statement found at that revision, redistribution basis not established; plugin not bundled |
| Monocypher 4.0.3 | Loup Vaillant; BSD-2-Clause or CC0-1.0 (either), `licenses/Monocypher-BSD-2-or-CC0.txt`; vendored unmodified (version string only) in `native/platform/sdk/net/online/third_party/monocypher` for the online play transport's encryption |
| Mbed TLS 3.6.7 | TLS library of the HTTPS client, linked only when built with `NP_MBEDTLS` (`docs/ONLINE-TLS.md`); the Mbed TLS project and contributors; Apache-2.0 (dual-licensed GPL-2.0-or-later), `licenses/Apache-2.0.txt`; stable pin in `dependencies/lock.json` plus only the exact upstream [non-PSA TLS 1.3 cleanup backport](https://github.com/Mbed-TLS/mbedtls/commit/afa648aa44c698f6f01b1dd653edc3dfbe140355), applied as a verified output overlay by `tools/build-mbedtls.py`; [patch provenance](dependencies/mbedtls/patches/README.md) |
| Root certificates | `native/platform/sdk/net/np_https_roots.h`: public root certificates of Google Trust Services, Internet Security Research Group (Let's Encrypt) and SSL.com, as shipped in the ca-certificates package |
| math-neon | Lachlan Tychsen-Smith; MIT (expat, stated in its README; the repository has no licence file), `licenses/math-neon-MIT.txt`; statically linked |
| VitaSDK libraries | Statically linked from VitaSDK GCC 15.2.0: **libpthread** (Pthreads-embedded, header: LGPL version 2 or later, `licenses/LGPL-2.1.txt`), **libstdc++ and libgcc** (GPL-3.0 with the GCC Runtime Library Exception, `licenses/GCC-runtime-exception.txt`), **newlib libc and libm** (many permissive notices; the SDK install here holds no copy of them). The `Sce*` stubs are generated import stubs |
| DejaVu fonts | Bitstream Vera / DejaVu terms in `licenses/DejaVu.txt`; rasterized Mono and Condensed Bold glyphs are included in the console and button prompts, with Sans Bold used in original project artwork |
| zlib 1.2.2 | Jean-loup Gailly and Mark Adler; `licenses/zlib.txt`, with original source notices retained |
| NFS Underground 2 Vita sampler (method) | danos-x5/NFSUG2_VITA `src/platform/sampler.cpp`, MIT. The on-console pc sampler (`native/platform/vita/src/vita_sampler.cpp`, off by default) follows its method: suspend / read context / resume through `sceKernelSuspendThreadForVM`, `sceKernelGetThreadContextForVM`, `sceKernelResumeThreadForVM`, a self-calibrated register layout and wait types. The code was written from the description of the method, not copied; the credit is given regardless. |
| Port-specific additions | Preserved source; no new blanket license is invented by this import |
| File checker and preparation/build helpers | MIT; `licenses/CBOC-tools-MIT.txt` lists the covered scripts and tests |

The upstream CC0 text and third-party license texts were retrieved from the
exact imported upstream revision, not copied from an unrelated latest tree.
Upstream licensing does not grant rights to Nintendo/Next Level Games assets,
trademarks, retail executables, fan art or extracted content.

Dependency revisions and source URLs are in [dependencies/lock.json](dependencies/lock.json).
The source-build VPK carries these license texts and renderer notices in
`licenses/`. The DejaVu text is from the font project's
[2.37 release](https://github.com/dejavu-fonts/dejavu-fonts/blob/version_2_37/LICENSE).

The [kubridge project](https://github.com/bythos14/kubridge) is credited above.
Its header/export input has no license file at the pinned revision; attribution
and a source link do not establish redistribution permission. No plugin is bundled.

## What is in the executable

From the link map of a 1.3 build (`mscharged-np.map`): 2 objects of `libSceShaccCgExt.a` (`vitashark.o` calls `sceShaccCgExtEnableExtensions`), 1 of
`libvitashark.a`, 27 of `libvitaGL.a` (among them `rt.o`, `preprocessor.o`, `expression.o`, `etc1_utils.o`, `etc_utils.o`, `eac_utils.o`, `atitc_utils.o`), 5 of `libmathneon.a`,
Mbed TLS, `libpthread.a`, `libstdc++.a`, `libgcc.a`, newlib's `libc.a` and `libm.a`, the `Sce*` and taiHEN import stubs. All are static: no vitaGL, vitaShaRK or SceShaccCgExt
module is loaded at run time. `libshacccg.suprx`, the Vita's shader compiler, is the player's own file and is not shipped.

## What a release must ship for the LGPL parts

The vitaGL author's condition, as the owner reports it: "Either you release sourcecode or provide a relinking package". Which of the two applies is the owner's choice; each needs all of its list.
The texts of `licenses/` go in the VPK already (`native/platform/vita/Makefile.vpk`).

**Source route.** With each release: the exact commit of this repository (a tag); `dependencies/lock.json`; the patched vitaGL tree and its patches, which are in the repository; the pinned
vitaShaRK, SceShaccCgExt, math-neon, taiHEN and kubridge sources, or a place that will keep serving those exact commits; the build recipe (`docs/BUILDING.md`, `dependencies/Makefile.vitadeps`)
with the toolchain versions. A release built from uncommitted changes does not meet this.

**Relinking route.** With each release: every object and archive of the game, the platform layer and the renderer that the link takes (`native/platform/vita/Makefile.vpk`, `LINKRULE`); the exact link line with
`NP_VLIBS`, `NP_VWRAPS` and the wraps of vitaGL's memory calls; `mscharged-np.map`; the `vita-elf-create`, `vita-make-fself` and VPK steps; the source of the vitaGL, vitaShaRK and Pthreads-embedded
actually linked, with this port's changes; the versions of VitaSDK, GCC, Clang and the LLVM gold plugin. ThinLTO objects are LLVM bitcode: they relink only with the same Clang 18.1.3 and plugin, unless
the package is built without LTO. A relink has not been tried: try it from the package alone before relying on this route.

## For the owner to decide

Nothing here is legal advice, and no third party has approved anything. The repository was made public, and the selected VPK is in the repository and public release, with the combined-package review still pending.

1. **SceShaccCgExt is GPL-3.0, and it is statically linked.** The author's relinking statement concerns vitaGL and vitaShaRK; it does not speak for this library. Its author (bythos14) is a different person.
   Whether the game can ship with it on its present terms is the first thing to settle. The ways that exist: ask bythos14; take the whole combined work to GPL-3.0 terms; or remove the library,
   which is engineering work, not a switch: `shark_init` calls it, and the build notes say it makes the shader compiler accept the Cg dialect vitaGL's translator emits. The seed pack's shaders may hide that until a
   shader is compiled on the console; untested.
2. **Port-specific code has no licence.** "No new blanket license is invented" (table above) means nobody has been given rights to it. Both LGPL routes assume people may modify and relink; the owner has to state terms for the port's own code. CC0 covers the upstream decomp's original
   contributions only.
3. **vitaGL's upstream licence file.** Not in this tree and not reachable from where this was written (the pin is `vitagl_base`). The earlier notice here said GPL-3.0; the file headers and the author say LGPL. Copy the upstream file at the pin into `licenses/` and confirm.
4. **This port's changes to vitaGL.** Patches 0001 to 0029 and `rt.c` carry no licence statement of their own; state that they are under vitaGL's terms.
5. **`utils/preprocessor`** in vitaGL: linked, with a credit and no licence. Ask upstream.
6. **kubridge:** no licence at the pin (above). Its header and exports build the weak stubs in the executable.
7. **newlib and Pthreads-embedded:** record the exact source revisions the SDK's libraries came from, and copy newlib's notices; neither is in the SDK install.
