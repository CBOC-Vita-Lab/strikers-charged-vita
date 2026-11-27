# Pipeline Central: disc audit (1.3)

Read from the owner's disc dump with the game's own formats (`np_world.c`, `np_rlg.c`, `np_rlt.c`, `np_bun.c`, `World::LoadChunks`, `StadiumWorldDrawable`, `ParticleSystem`). No disc data is in this tree.
Nothing here was run on a Vita. Numbers say what the data holds, not how fast anything runs.

**Measured from data:** counts, areas, texture sizes, emitter and template values, ini values.
**Estimated by a model:** "screens". A software rasteriser drew the stadium's world objects and a steady-state sample of its world-object emitters from 12 gameplay cameras (distance and pitch 20/30, 23/31.6, 35/38; four targets; `gCameraData`, FOV 27.3 vertical, 16:9), and counted fragments that pass the depth test, as multiples of the screen. Objects `StadiumWorldDrawable::UpdateBlend` hides under the gameplay camera (flag 2) are left out, as in open play; replays and cut-scenes draw them. Not included: players, crowd sprites, gameplay effects, frustum culling of non-always emitters (off-screen sprites count zero anyway). Animated objects stand at their stored matrices. Particle position is approximate: emitter shape, speed, direction. A 640:448 aspect gives Mist 5.5 instead of 5.9.

| | Pipeline | Palace | Bowser | Konga | Wastelands | Underground | Lava Pit | Crystal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Display lists, all (packets per instance) | 355 | 355 | 309 | 631 | 402 | 309 | 211 | 204 |
| Triangles, all | 74.8k | 76.0k | 86.4k | 94.2k | 75.7k | 56.0k | 75.5k | 132.3k |
| Display lists in open play | 187 | 154 | 168 | 422 | 214 | 193 | 133 | 112 |
| Triangles in open play | 41.7k | 31.1k | 49.1k | 46.3k | 49.7k | 29.8k | 40.1k | 37.8k |
| Draws: ScrollingDiffuse / ScrollingSpecular | 206 / 33 | 255 / 72 | 113 / 153 | 517 / 34 | 353 / 37 | 188 / 0 | 136 / 49 | 113 / 28 |
| Draws: MaskedSpecularFresnel / CameraScrolledOverlay | 96 / 20 | 4 / 4 | 9 / 16 | 43 / 16 | 4 / 8 | 62 / 59 | 13 / 13 | 4 / 0 |
| Blended world surfaces, world units² | 646 | 505 | 56.1M (backdrop) | 9,691 | 2,230 | 20,050 | 52,614 | 208,875 |
| Screens: blended world, mean (worst camera) | 0.01 (0.02) | 0.01 (0.02) | 0.07 (0.19) | **3.52 (11.70)** | 0.12 (0.22) | 0.09 (0.17) | 0.25 (0.46) | 0.20 (0.79) |
| Screens: world emitters, mean (worst) | **5.89 (8.12)** | 0.00 (0.01) | 0.01 (0.07) | 0.00 (0.01) | 0.78 (4.80) | 0.02 (0.04) | 7.27 (9.43) | 4.23 (11.71) |
| Largest emitter | Mist_rain 5.88 | none | engineFlare 0.01 | none | fx_ash 0.78 | Ventsteam 0.01 | Warble 5.36 (half-size pass) | Crystal_casutics 4.22 |
| Most of what is visible | Overlay 81 % | Diffuse 85 % | Overlay 57 %, Specular 34 % | Diffuse 89 % | Overlay 78 %, Diffuse 19 % | Overlay 88 % | Overlay 77 % | Diffuse 97 % |
| Texture reads per visible pixel (programs' binding counts) | 2.79 | 1.21 | 2.56 | 1.20 | 2.60 | 2.82 | 2.64 | 1.05 |
| Textures | 75 | 74 | 132 | 81 | 54 | 66 | 67 | 36 |
| MiB in the renderer's cache (DXT1 where `CmprToDxt1` takes it); budget 32 | 2.5 | 1.9 | 2.6 | 3.1 | 2.7 | 2.4 | 2.7 | 1.5 |
| MiB if everything were decoded | 17.5 | 12.2 | 9.9 | 11.3 | 13.1 | 17.8 | 18.3 | 7.2 |
| Largest textures, 512+ / 256 | 8 / 16 | 6 / 11 | 0 / 19 | 5 / 14 | 3 / 33 | 9 / 15 | 11 / 9 | 2 / 12 |
| Most mip levels | 7 | 7 | 7 | 7 | 7 | 7 | 7 | 7 |
| Animated textures | 0 | 6 | 8 | 3 | 0 | 2 | 1 | 0 |
| UV-scrolling packets | 20 | 8 | 43 | 17 | 67 | 62 | 59 | 16 |
| Vertex animations (`0x1B201`) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| Hierarchies / animated drawables / their triangles | 7 / 71 / 18.3k | 5 / 61 / 12.0k | 6 / 65 / 18.9k | 9 / 278 / 21.0k | 10 / 76 / 15.9k | 4 / 65 / 8.7k | 3 / 34 / 15.2k | 1 / 24 / 5.4k |
| Lights / shadow volumes | 1 / 0 | 1 / 0 | 1 / 0 | 1 / 0 | 1 / 0 | 1 / 0 | 1 / 0 | 1 / 0 |
| Emitters / always visible | 34 / 9 | 29 / 1 | 46 / 1 | 25 / 1 | 42 / 2 | 24 / 0 | 57 / 15 | 60 / 9 |
| Warble emitters | 0 | 0 | 0 | 0 | 0 | 0 | 12 | 0 |
| High-range objects | 4 | 1 | 2 | 7 | 113 | 45 | 10 | 1 |

## Causes, ranked

1. **Mist_rain, 5.9 screens of additive sprites (measured template, modelled screens).** Two always-visible emitters at (±10, 0, 15), every 3 s, in Pipeline Central's own bundle. Template: additive `fx_smoke02` (128x128), 3 sprites a second for 3 s, life 9-10 s, width 12 world units (2 x 6, `s2 = size / 2`), alpha 130-143 for most of its life, falling 2.3-2.8 units a second through the field. That is about 55 sprites alive. At a camera distance of 23 and FOV 27.3 one sprite is nearly the height of the picture. Of the others, Lava Pit's Warble goes to a half-size target, and Crystal Canyon's haze lies over a one-texture floor.
2. **Full-size three-texture floor under that haze (measured binding counts, modelled share).** 81 % of the visible picture is CameraScrolledOverlay. Not unique (Underground 88 %, Wastelands 78 %, Lava Pit 77 %), but no other stadium here has both this floor and full-size haze: 2.79 reads per pixel with 5.9 screens on top. Underground has no haze.
3. **Everything else is not an outlier (measured).** Triangles, draws, lights, hierarchies and animated drawables sit in the range of the others. Blended world surfaces are 646 units² (glass 526, nets 72). No warble, no shadow volume, no vertex animation, no texture animation: audit candidates C4, C5 and C6 do not apply. Textures are 2.5 MiB in the cache against 32 (C3 does not apply). `brokenpipes`, `fieldgenerator` are 7 small hierarchies; there is no `engine` here.

**Open (settled below: the grass was a modelling error):** Konga Coliseum's blended grass tufts (`_kongacoliseum/grass1`, 195 packets, 3.5 screens, 11.7 at worst) are as large as the mist, normal blend rather than additive. There is no report on Konga either way. If it runs well on a console, overdraw alone does not explain 6-7 fps and the ranking above needs the census.

## High Range Mult 7.5

Bound by name to `mfHighRangeMult` and read in one place, `RenderHighRangePass`: it multiplies the texture-coordinate offset of the four samples, with `mfHighRangeOffset`. 7.5 x 2.625 = 19.7 against the default 2.5 x 3.5 = 8.75, so the glow reaches 2.25 times as far. It is neither of the two things asked: it enlarges no pass (targets, viewports and quads are fixed in `InitializeHighRange`) and it does not scale brightness (that is `High Range Gray` and the composite colour). Fragment count is unchanged. Wider offsets can only touch the texture cache more on a 320x224 source. Pipeline's High Range Index 6 is the default.

## Fixes

| Switch | Default | What | Where the data says it helps (arithmetic, not a measurement) |
| --- | --- | --- | --- |
| `MSC_FX_HAZE=1..4` | 2, only under the profile | One Mist_rain sprite in n drawn, n times the alpha | 5.9 to about 3 screens; peak alpha 143 x 2 clamps at 255, a tenth lost for that part of the life |
| `MSC_FX_HEAVY_PX=0\|1` | 1, only under the profile | The effects pass decided by the run's pixels, at any picture size | Mist alone is about 221 vertices, under the 400 floor, and all of the stadium's always-drawn sprites about 590 if they form one run. The pass also waited for a smaller picture. A run at a quarter per side costs about 1.06 screens and keeps 1/16 |
| `MSC_STADIUM_HEAVY_CROWD=0\|1` | 1 (as merged) | 0 keeps the crowd under the profile | The data gives no reason to hide it |

Both together: about 0.2 + 1.06 screens for the haze instead of 5.9.

**Recommendation.** The data justifies `MSC_STADIUM_HEAVY=8` with `MSC_STADIUM_HEAVY_CROWD=0`. It is the Vita build's default since 1.3 (`vita_plan.cpp` kSpeed); the console A/B `pipeheavy0` turns it off to measure what it buys. Render-only, peers may differ.

Not worth doing: lowering High Range Mult (no fragments saved), texture changes.

## All stadiums (floor track)

The same model, over all 19 environments on the disc. **Measured from data:** floor bindings, triangles, emitter values. **Modelled:** screens, floor share (flat surfaces within 1.5 units of the pitch plane that win the depth test, from the same 12 cameras). Screens repeat to about 8 % between runs (random sprites). Floor textures do not count the shadow lookup, one more read where a material enables it (Pipeline: yes).

**Correction to the table above.** Drawables and emitters bound to an animated hierarchy stand in the hierarchy's space: the matrix of the `0x106` object that binds them places them. The first model left them at their stored matrices, so all of Konga's grass stood in a heap at the centre circle. Placed properly it is 0.60 screens (0.97 at worst), not 3.52 (11.70). The "Open" paragraph above no longer stands: Konga has no overdraw problem. Thunder Island's floor glow moved the same way. The table here has the correction.

| Stadium (number) | Screens: world emitters, mean (worst) | Largest | Screens: blended world, mean (worst) | Floor, share of the picture | Floor textures per pixel | Triangles in open play |
| --- | --- | --- | --- | --- | --- | --- |
| Lava Pit (7) | 7.23 (9.91) | Warble 5.24 | 0.24 (0.45) | 83 % | 2.94 | 40.1k |
| Pipeline Central (8) | 5.45 (7.53) | Mist_rain 5.44 | 0.01 (0.02) | 84 % | 2.98 | 41.7k |
| Crystal Canyon (3) | 4.09 (9.18) | Crystal_casutics 4.08 | 0.20 (0.77) | 91 % | 1.02 | 37.8k |
| Sand Tomb (15) | 4.02 (4.87) | Warble 3.86 | 0.01 (0.02) | 84 % | 1.10 | 61.4k |
| Thunder Island (11) | 2.13 (3.00) | Thunder_island_mist 1.24 | 0.05 (0.12) | 83 % | 1.03 | 73.6k |
| Storm Ship (9) | 0.41 (0.68) | storm_ship_energy_ball 0.23 | 1.06 (1.38) | 40 % | 2.78 | 38.9k |
| Wastelands (14) | 0.82 (5.13) | fx_ash 0.81 | 0.12 (0.22) | 89 % | 2.80 | 49.7k |
| Konga Coliseum (6) | 0.00 (0.01) | none | 0.60 (0.97) | 90 % | 1.13 | 46.3k |
| The Dump (4) | 0.31 (0.52) | fx_dump_steam 0.29 | 0.06 (0.08) | 84 % | 2.85 | 48.5k |
| Battle Dome (0) | 0.34 (0.98) | Ventsteam 0.33 | 0.01 (0.02) | 90 % | 1.02 | 28.7k |
| Crater Field (2) | 0.00 (0.01) | none | 0.17 (0.21) | 87 % | 1.08 | 49.7k |
| Galactic Stadium (5) | 0.08 (0.30) | AsteroidShadows 0.07 | 0.07 (0.17) | 89 % | 2.88 | 72.1k |
| Bowser Stadium (1) | 0.02 (0.16) | none | 0.12 (0.27) | 85 % | 2.95 | 49.0k |
| Tutorial Field (16) | 0.00 (0.00) | none | 0.13 (0.32) | 80 % | 2.85 | 11.8k |
| Underground (12) | 0.02 (0.06) | none | 0.08 (0.17) | 89 % | 2.99 | 29.8k |
| Vice (13) | 0.01 (0.01) | none | 0.01 (0.02) | 88 % | 1.02 | 53.2k |
| Low Poly (17) | 0.00 (0.00) | none | 0.01 (0.02) | 81 % | 1.02 | 12.7k |
| The Palace (10) | 0.00 (0.01) | none | 0.01 (0.02) | 87 % | 1.11 | 31.1k |
| Vice (night) (-) | 0.00 (0.00) | none | 0.01 (0.03) | 88 % | 1.10 | 52.0k |

- **Lava Pit is the nearest to Pipeline:** the same three-texture floor and 7 screens of effects. 5.2 of them are Warble, which goes to a half-size target and which `MSC_STADIUM_HEAVY` already halves again. `lava_spin` adds 1.5 full-size screens; its sprites are too large and short-lived for haze thinning to take them (25 to 60 units, 2 s).
- **Crystal Canyon** has full-size haze as large as Pipeline's in the worst camera (caustics, additive, flat on the pitch), but its floor is one texture.
- **Sand Tomb** is Warble, like Lava Pit, over a one-texture floor.
- **Wastelands** is 0.8 on average and 5.1 at worst: ash near the camera.
- **Konga, Thunder Island, Storm Ship:** nothing of this kind. Thunder Island's mist and glow have sizes on curves and 2 screens together.

`MSC_FX_HAZE_WIDE=1` adds, under the profile, what `NpSceneHazeTemplateWide` takes. Of every template in every bundle on the disc that is `fx_mistrain`, `fx_crystalCaustic`, `fx_snow` (Wastelands' ash), `fx_stormWind`, and not the ball's ground glow. By arithmetic, at `MSC_FX_HAZE=2`: Crystal Canyon 4.1 to 2.0 screens, Wastelands 0.8 to 0.4 (worst 5.1 to 2.6). Konga's grass is not thinned: nothing to gain, and normal blend has no alpha to give back. Candidates to try: `MSC_STADIUM_HEAVY=8,3,14 MSC_STADIUM_HEAVY_CROWD=0 MSC_FX_HAZE_WIDE=1`.

## The pitch floor

Formula of both reflective materials (`CameraScrolledOverlay`, `ScrollingCameraOverlay`): vertex colour x diffuse + reflection x reflection alpha x amount x mask, then x the stadium's shadow lookup where enabled.

- **It cannot be baked whole.** The reflection's coordinates are the position plus the camera's position times `cameraScroll`, every frame (5.0 in Pipeline, 0.2 to 1.0 elsewhere). A render of the material into one texture would freeze one camera's reflection on the pitch. Amounts are 0.2 to 1.0 on the eight stadiums with such a floor.
- **Shadows do not stop it.** Player shadows, the ball's shadow and planar shadows are quads drawn over the floor afterwards; none is in the material. The lookup is a 16 to 128 pixel static picture and stays a read of its own. Vertex colours stay: they are in the program, not the texture. Lighting stays for the same reason.
- **How much the sheen is (disc data, decoded).** Mean added brightness: Pipeline 0.6 to 1.7 %, Lava Pit 2.5 %, Underground 4 to 5 % on the tiles, Wastelands 6.5 %. Mask peaks 6 to 37 %. Mask coordinates follow the diffuse's exactly on all tile floors, or by a plain scale and offset (Wastelands, Tutorial).

**`MSC_FLOOR_BAKE=<numbers>|all` (default empty).** What can go into a texture does: the mean of the reflection x amount x mask is added at load to the end colours of each CMPR block of the diffuse, at every mip level, in place, and the packet is drawn with one TEV stage. So two reads of four go (three to one, plus the lookup). Costs **no** CDRAM: same texture, same bytes, no render target, so render-target loss does not concern it; masks and reflections nothing else uses are never uploaded. Load cost: decoding the masks and rewriting about 1.4 MiB of blocks, estimated tens of ms. What is lost: the sheen moving as the camera moves; the mask's shape below one block (4 texels); a mean add under half a 5-bit step (Pipeline keeps about half of its 1.2 %). Diffuse and mask scrolling (The Dump) survive, because the mask scrolls with the diffuse. Run against the disc's textures with the header's own code: error against the exact mean term 1 to 2 % of full scale on average, 3 to 13 % at the 99th percentile (crack edges in Wastelands). Not baked, drawn as before: Storm Ship's animated generators, non-CMPR diffuse (Bowser's glass), masks sharper than the diffuse (Underground's cement surround, Galactic's panels).

A render-target bake was the other way. It would cost 1.3 MiB at 512 and 5.3 MiB at 1024 (RGBA8 with mips) against a texture budget of 32 with 2.5 in use, so memory would be no obstacle, but 1024 over a 41.6 x 24.8 pitch is 25 texels a unit against the disc's 37, it needs a new pass in the load, a target that survives, and it still loses the reflection. Rejected. Dropping the layers by distance is not needed once the far tiles cost one read as the near ones do.

Estimate, not a measurement: 45 % fewer texture reads on 80 to 88 % of the picture in the eight three-texture stadiums (2.8 + 0.8 to 1.2 + 0.8 per visible pixel). Before Pipeline's haze is cut that is about a seventh of the stadium's fragment work; after it, up to a third. As frame time, perhaps 5 to 15 % of the GPU's frame.

## The floor's sheen (`MSC_FLOOR_SHEEN`)

The moving part of the reflection, per vertex, added by a second TEV stage from COLOR1. From the disc: Pipeline has 1,085 floor vertices (median edge 2.0 units), Lava Pit 400 (2.1), against 2.2 to 2.3 units per reflection texel: enough. Wastelands has 132 (longest edge 11.4 against a 0.3-unit reflection): not enough, so it takes a 16 x 16 lookup.

Texture reads per floor pixel, by `MSC_VGX_FILLCENSUS` in Vita3K (stages / textures of the draws):

| | Full | Sheen | Bake only |
| --- | --- | --- | --- |
| Pipeline Central | 5 / 4 | 3 / 2 | 2 / 2 |
| Lava Pit | 5 / 4 | 3 / 2 | 2 / 2 |
| Wastelands (no shadow lookup, sheen by lookup) | 4 / 3 | 2 / 2 | 1 / 1 |

Modelled against the exact material over 24 cameras (top-down, disc textures, no emulator): correlation of the motion 0.73 Pipeline, 0.94 Lava Pit (vertex), 0.76 Wastelands (lookup), 0.42 if Wastelands used its vertices.

Pictures from Vita3K, same frames at the start of open play (Vita3K validation build with virtual time, 480x336): Pipeline: no visible difference between the three; the sheen there is about 1.5 of 255 and bake alone would do. Lava Pit: close, a little less contrast in the dark cracks; the sheen is nearer than bake alone (RMS 5.1 against 8.3 of 255 at the same frame). Wastelands: **not close.** The cracks lose most of their hot orange with either mode (RMS 17 sheen, 25 bake). Keep the full material there.

Not settled: the sheen's motion was not seen on the same camera in Vita3K, because matches diverge in open play even under virtual time, and the base-against-base control crashed before play. The code reads no simulation state and writes none. Bake mean is the whole reflection's; a mean at a usual camera would be nearer on Lava Pit and Wastelands.

## Shadows

Players: 11 shadow pictures of 144 x 144 pixels (`CreateShadowPartitionViews`), each copied to 72 x 72, every second frame in open play. Stadiums: no shadow map. The static lookup is 16 to 128 pixels, and The Dump (1) and Vice (4) are the only worlds with a shadow volume. `MSC_SHADOW_RES=96|72` draws them at that side and halves it in the copy, default 144. Arithmetic: about 0.2 screens of very cheap fragments at 144, about 0.05 at 72, so under 2 % of the fill. It leaves the number of passes alone, which is the larger cost of shadows if it costs anything. Coarser shadows on the pitch for that: only worth it after the floor and haze.
