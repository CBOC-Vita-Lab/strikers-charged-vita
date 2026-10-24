# HD textures (captains, sidekicks, goalies, the ball)

Higher-resolution versions of the character textures (captains, sidekicks and goalies, with
all their team-colour variants) and of the ball, made from the owner's own disc files with an upscaling
network, stored GPU-ready (DXT1/DXT5 with their own mip chains) in one pack file that the
renderer's texture cache reads on the console. The pack is game-derived data: it lives only in
the build directory and on the memory card, never in git.

## Quick start

On a Windows, Mac or Linux computer with Python 3 (about 2 GB of free space, 30-60 minutes):

1. Extract your disc with Dolphin as for the install ([how](../../PREPARE-GAME.md)), so you
   have its `files` folder.
2. Install the helpers and download the upscaling model (commands under **Rebuild** below).
3. Run `python3 tools/hdtex/build_pack.py --data <your files folder> --out hdtex-build`.
4. Copy `hdtex-build/hdtex.pack` to `ux0:data/mscharged/hdtex.pack` on the Vita.

The game finds it at the next start. Delete the file, or add `MSC_HDTEX=0` to
`ux0:data/mscharged/np-player.txt`, to go back to the original textures. Keep the pack to
yourself: it is made from your copy of the game.

## How it works

The tools are the Super Mario Galaxy port's (same pack format, same key rule, same quality
rules); the reader of the game data (`rlt.py` instead of the RARC/J3D reader), the selection
(characters and the ball) and DXT1 with 1-bit alpha for on/off alpha differ.

## Rebuild

Needs Python 3 with numpy, Pillow, xxhash, quicktex and CPU PyTorch, and the Real-ESRGAN
weights `realesr-general-x4v3.pth` (xinntao/Real-ESRGAN release v0.2.5.0, BSD-3-Clause):

    pip install --user numpy pillow xxhash quicktex
    pip install --user torch --index-url https://download.pytorch.org/whl/cpu
    mkdir -p hdtex-build/models
    curl -L -o hdtex-build/models/realesr-general-x4v3.pth \
      https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth
    python3 -E -P tools/hdtex/build_pack.py --list            # what would go in, and why the rest not
    python3 -E -P tools/hdtex/build_pack.py --upscale-only    # the network only (~35 min on 2 CPUs, resumable)
    python3 -E -P tools/hdtex/build_pack.py                   # encode and write the pack (~25 min)
    python3 -E -P tools/hdtex/build_pack.py --max-side 256 --pack hdtex-lite.pack   # smaller pack

Options: `--data` (the disc's `files/` folder, default `game/files`), `--out`
(default `hdtex-build`), `--scale 2|4`, `--max-side N` (the long side a replacement
may have; a texture that cannot get at least 2x under it stays as the game has it: 512, the
default, doubles every character texture, 256 doubles only the 128 px and smaller ones: heads,
eyes, goalie faces), `--only REGEX` (on `<name> <file>:<index>:<hash>`), `--pack NAME`.

Output in `hdtex-build/`: `hdtex.pack`, `manifest.tsv` (every texture: names,
original and HD format/size, bytes, PSNR of the compression), `memory.txt` (HD bytes per bundle,
per captain set and for a typical match), `sheets/*.png` (before | after for every texture),
`cache/` (the network's output; a rerun only re-encodes).
Install: copy `hdtex.pack` to `ux0:data/mscharged/hdtex.pack`.

## What goes in

`Art/characters/<name>/*.rlt` of every folder except `crowd*` and `npcs`: the base sets
(`<c>.rlt`), `extratextures.rlt` (eye frames), the captains' and goalies' alternate colours
(`<c>_alt.rlt`, `<c>goalie[_alt]/`), the sidekicks' team colours (`<s>_<captain>[_alt].rlt`) and
`mario/mario_shock.rlt`. Names come from `Art/hashid.bin` (the bundle hash of a texture is
`nlStringLowerHash("<folder>/<name>")`, e.g. `mario/mariohead`); labels are
`<name> <folder>/<file>:<index>:<hash>`. Identical textures (same key) go in once with all
their names (the sidekicks' `<s>/<s>_mario` and `<s>_mario/<s>_mario` are the same bytes).

The ball (`EXTRA_BUNDLES` in `build_pack.py`, only these names): `gameplay/low_ball`
(`objects/gameplay/ball.rlt`, the match ball; the same bytes are `nis_ball/low_ball` of the Mega
Strike cutscene ball and `transitions/ball_hires` of the flying-ball scene transition, so one
entry), `transitions/ball_hires_1` (`transitions/transitions.rlt`, the transition ball's second
texture) and `global/the_ball` (`global.rlt`, the Mega Strike ball indicators of
`MegaBallIndicators.cpp`, RGB5A3 with soft alpha). Not the ball's colour: `gameplay/charge0`,
`nis_ball/charge4` (charge glow), the `*_spec` maps, `gameplay/env`, `global/ball_shadow`,
`global/the_ball_exploded` (explosion sprite); front-end art: `fe/screens/images/the_ball`
(loading icon, `fe/loadingui.res`), `fe/screens/images/luigi_ball` (HUD, `fe/ingameui.dmn`).

Left out (rules at the top of `build_pack.py`): texture animations, formats other than colour
(CMPR, RGB565, RGB5A3, RGBA8, CI8; the character bundles hold only CMPR and RGB5A3),
textures under 16x16 or not a power of two, and by name: specular / environment maps and masks
(`spec`, `specular`, `specmask`, `env`, `env_spec`), the shared status overlays `dirt` and `ice`
(RGB5A3 splats, the same bytes in every set), the flat `eyegray`, and the Mega Strike / stadium /
HUD art in `extratextures.rlt` (`mega_hand*`, `mega_cone_colour`, `mega_gameplay_bg`,
`<c>_banners*`, `<c>_logo`).

## How the game hands these textures to the GPU (what the key is computed over)

- `.rlt` is big-endian on the disc. The port's converters (`npa_cvt_rlt_data`,
  `npa_texture_entry_swap` -> `npa_cvt_texture_entry`) swap only the bundle header, the
  dictionary and the `GXTextureHeader` fields (and texture-animation structs); the texel data
  and palettes stay as on the disc (GX-tiled, big-endian). So the disc bytes are the bytes the
  GPU path reads.
- `glx_MakeTexture` / `glplatTextureReplace` copy the levels unchanged; `PlatTexture::Prepare`
  hands `GXInitTexObj` the header's width/height and the game format mapped through
  `glx_GetGXFormatTable() = {4, 5, 14, 6, 1, 0, 1, 3, 9}`.
- `gbReduceTextures` (skip 1 or 2 levels at load) is a TweakValue that defaults to false;
  nothing in the port or the disc's ini files sets it. Level 0 of the file is the GPU's level 0.
  (If it were ever turned on, the GPU's level 0 would be the file's level 1 or 2 and no key
  would match: the textures would simply not be replaced.)
- CI8 (none in the character bundles): the palette is `numEntries` big-endian u16 right after
  the last level, at `GCTextureSize(all levels)` (the game's own walk, no block rounding), loaded
  as a GX_TL_RGB5A3 TLUT; `rlt.py` reads it there and the key includes it.
- Team colours: the sidekicks' `<s>_<captain>[_alt].rlt` and the captains' / goalies' `_alt`
  bundles are loaded as textures of their own (existingTextureMode 0, own hash such as
  `birdo_bowser/birdo_bowser`); the character swaps its base texture for that one when drawing.
  Either way the GPU gets the variant's own bytes, which is what the key covers.

GX format numbers (origFmt and seed): game RGB565 0 -> 4, RGB5A3 1 -> 5, CMPR 2 -> 0xE,
RGBA8 3 -> 6, I8 4 -> 1, I4 5 -> 0, A8 6 -> 1, IA8 7 -> 3, CI8 8 -> 9.

Per texture: decode level 0 (`gxdecode.py`), give fully transparent texels their neighbours'
colour, Real-ESRGAN 4x (realesr-general-x4v3), Lanczos down to 2x, mip levels by area
averaging down to 1x1, then BC1 (opaque), BC1 with 1-bit alpha (on/off alpha, every level's
alpha thresholded at 0.5 again) or BC3 (soft alpha) with quicktex at its best level.
BC1 with 1-bit alpha (`encode_bc1_punch`): quicktex has no punch-through mode, so blocks without a
transparent texel keep quicktex's four-colour encoding, and a block with one gets quicktex's
three-colour endpoints fitted to its opaque texels (transparent ones set to their mean), ordered
c0 <= c1, opaque texels on the nearest of c0, c1 and the midpoint, transparent ones on index 3.
A transparent BC1 texel decodes black (0, 0, 0, 0), as the CMPR originals' do; the colour bleed
still decides the colour of the edge texels that stay visible in every mip level (the area average
runs over the bled colours before the threshold). `decode_bc1` decodes with that alpha (quicktex's
decoder returns the transparent texel opaque black), for the PSNR (over visible texels' RGB and
alpha) and in compare.py / check_borders.py.
Three rules keep the result faithful to the original at the original's scale:
- Border padding for the network (`pad_mode`): wrap-around only when the texture really tiles
  across that seam (seam step no larger than twice the mean step between neighbouring texels);
  otherwise mirrored. (The wrap mode is in the material binding, not in the texture: every
  texture is treated as repeat and the seam test decides; the character atlases get mirrored.)
- Fidelity pass (`fidelity`): soft-thresholded back-projection. Where the HD level 0,
  area-averaged to the original's size, differs from the original by more than 4/255, the
  excess is spread back (bilinearly, 3 rounds).
- On/off alpha (CMPR with transparency) stays on/off: the HD alpha is thresholded at 0.5.

Checks:
- `python3 -E -P tools/hdtex/check_borders.py <pack> [out.tsv]`: for all entries, the HD level 0
  area-averaged to the original's size against the original, per border strip and the interior;
  flags a texture whose border strip has MAD > 2 x interior MAD + 6 or |bias| > 6 (exit 1).
- `python3 -E -P tools/hdtex/compare.py <pack> <out.png> mario/mariohead peach/peachhead ...`
  validates every index entry of a pack against the disc (keys, original format/size, offsets,
  level sizes) and draws close-up before/after pictures (both bilinear-magnified to one size).
- `python3 -E -P tools/hdtex/make_test_pack.py <out.pack>`: a synthetic one-texture pack made
  with this key function and writer, for a C++ test of the runtime's key.

## Pack format (version 1, little-endian; the Galaxy port's)

    header, 32 bytes:  char magic[4] "HDTX"; u32 version = 1; u32 count; u32 indexOffset;
                       u32 namesOffset; u32 dataOffset; u32 fileSize; u32 reserved
    index, count x 32 bytes, sorted by key:
                       u64 key; u32 dataOffset (from the file start); u32 dataSize;
                       u16 width, height (replacement level 0); u16 origWidth, origHeight;
                       u8 format (1 = DXT1, 3 = DXT5); u8 levels (down to 1x1);
                       u8 origGxFormat (GX hardware number); u8 flags (bit 0: alpha);
                       u32 nameOffset (into names)
    names:             NUL-terminated "<name> <folder>/<file>:<index>:<hash> ..." strings (for
                       the log; at most 200 characters)
    data:              per texture, 64-byte aligned: the levels, level 0 first, each a whole
                       number of 4x4 blocks (8 bytes DXT1, 16 bytes DXT5), as
                       glCompressedTexImage2D takes them

    key = XXH3_64(level-0 texel bytes as the GPU reads them (big-endian, GX-tiled; for CMPR
                  w*h/2 bytes), seed = gxFormat | width << 8 | height << 20)
    CI formats: key = XXH3_64(palette bytes (numEntries * 2, big-endian), seed = key)

The key depends only on the texture's own bytes, so a texture is found wherever the game loads
it from (base set, team-colour bundle, front end), and a texture the game rewrites is never
replaced.
