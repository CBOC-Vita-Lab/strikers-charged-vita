# Third-party Notices

There is no blanket license covering this entire mixed-source snapshot.
Keep file-level notices and the license texts with any permitted copy.

| Component | Provenance and terms |
| --- | --- |
| Reconstructed game/source base | [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp/tree/8cd3c6d2cdd5aaf2147fa60efcac6be5a293452b), imported base `8cd3c6d`; original contributions CC0, not a license to game assets or all third-party content |
| Upstream third-party code | Pinned upstream texts in `licenses/`: Apache 2.0, GameSpy BSD-3-Clause and ODE BSD; zlib retains its source notice |
| Native renderer | Retains `renderer/third_party/LICENSE-aurora`, `LICENSE-stb` and `LICENSE-xxhash`, plus file notices |
| vitaGL | Rinnegatamante and contributors; GPL-3.0, text in `licenses/GPL-3.0.txt`; the local modified implementation is included |
| vitaShaRK | Rinnegatamante; LGPL-3.0-or-later, `licenses/LGPL-3.0.txt`; includes xxHash under its retained BSD notice |
| SceShaccCgExt | bythos14 and contributors; GPL-3.0, `licenses/GPL-3.0.txt` |
| taiHEN import stubs | Generated from Yifan Lu's pinned exports; MIT, `licenses/taiHEN-MIT.txt` |
| math-neon | Lachlan Tychsen-Smith; MIT, `licenses/math-neon-MIT.txt` |
| DejaVu fonts | Bitstream Vera / DejaVu terms in `licenses/DejaVu.txt`; rasterized Mono and Condensed Bold glyphs are included in the console and button prompts, with Sans Bold used in original project artwork |
| zlib 1.2.2 | Jean-loup Gailly and Mark Adler; `licenses/zlib.txt`, with original source notices retained |
| Port-specific additions | Preserved source; no new blanket license is invented by this import |
| File checker and preparation/build helpers | MIT; `CHECK-FILES-LICENSE.txt` lists the covered scripts and tests |

The upstream CC0 text and third-party license texts were retrieved from the
exact imported upstream revision, not copied from an unrelated latest tree.
Upstream licensing does not grant rights to Nintendo/Next Level Games assets,
trademarks, retail executables, fan art or extracted content.

Dependency revisions and source URLs are in [dependencies/lock.json](dependencies/lock.json).
The source-build VPK carries these license texts and renderer notices in
`licenses/`. The DejaVu text is from the font project's
[2.37 release](https://github.com/dejavu-fonts/dejavu-fonts/blob/version_2_37/LICENSE).

The kubridge header/export input has no license file at the pinned revision;
its redistribution basis is not established here. No plugin is bundled.
The complete combined package still needs a corresponding-source and license review. This is private preparation,
not a statement that a public binary is cleared for redistribution.
