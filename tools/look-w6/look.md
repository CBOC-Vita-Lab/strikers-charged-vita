# look (wave 6): the best default picture below 960x544

Agent `look`, 11 Nov 2026. Worktree agent/look (commits: tools and notes; vita_plan.cpp + CHANGELOG + TECHNICAL change). Contact sheets and index: /home/claude/s13/w6/look/ (index.md). Nothing here ran on a Vita.

## Bottom line
1. At the GC port's cost (271k pixels) today's Charged picture (game-shape 622x436 or 640x448 at 287k, `combined`, k = sqrt(stretch-1)) is about 0.011-0.014 SSIM and 0.4-0.6 dB closer to native than the GC port's 691x392 plain bilinear, with 10 % more edge contrast
   (gradient energy 0.87 against 0.79 of native) and less ringing than any sharper variant. Visually it is a small, real gain: the filter itself is worth little (SSIM equal to bilinear, +0.08 gradient); most of the gain is that the game's shape puts the pixels where the picture needs them (vertical).
2. Below that cost the picture is clearly softer than GC's: the LOCK60 open-play floor (ladder 0.70 = 448x314, 141k pixels) is 52 % of GC's pixels, SSIM 0.917 against 0.939. What closes the gap at that size is the HUD (text and digits are drawn at the 3D size and blurred by the upscale), not the filter.
3. Defaults: keep `combined`, k = 1.0, cap 0.8, SHAPE off, MSAA rungs at/above the game's own size only, bias 60. One change proposed: `MSC_VGX_HUDNATIVE=1` as a supporting default of LOCKED 60 (the mode that lives at the low rungs); it is not run on a console and the host harness cannot test it.

## What the harness could and could not show (so you know what these pictures are)
- Prebuilt main-tree host32 binary through `PICS_NOBUILD=1 tools/pics.sh`, one run per (size, env), 10 frames each: 720 title sky gradient, 1300 captain menu (text), 2040/2041 zoomed play + next frame, 2400 close-up cut-scene, 2520 and 2760 Mega Strike (prompt text, glow), 3120 goal + banner, 3600/3601 wide play + next frame. 19 runs (about 3-5 min each), queued behind other agents. Native = look-r100 (960x544; identical to ref-w6a).
- The harness dumps the scene at `MSC_RENDER_SIZE`, before the present shader. The upscale is done offline (`tools/pl.py`: numpy copies of bilinear, `combined`, `fast`, SMOOTH). Validated against the real GL shaders (one run `look-gl691` with `MSC_VGX_SCREEN`, 5 frames x 4 programs): combined/fast 52-55 dB (max 2-5 levels), bilinear 58 dB (max 1), SMOOTH 53 dB with 0.1 % tie pixels off (validate.txt). So the variants are what the shaders give within 1-5 levels. The shipped `combined` zooms 0.3 % (one texel inset per side: up to 1.4 px at the border at 0.72, 2 px at 0.5); scores here use no inset so that PSNR/SSIM measure the filter, not the zoom.
- **The texture LOD bias cannot be seen on the host.** `MSC_VGX_LODBIAS_RUNG` is applied only in the Vita build (`vgx_backend_gl.cpp:2128`, `#ifdef __VGX_VITA__`: GLES has no texture bias): rung 0 and rung 60 frames are bit-identical (4 frames, max difference 0). The 40/60/80 strength study is therefore not possible here (4 runs dropped); `MSC_VITA_ANISO_MIN` is Vita-only too.
- **`MSC_VGX_HUDNATIVE` cannot run on the host** (the present split exists only in `renderer/vita/vgx_host_vita.cpp`). The "HUDNATIVE" pictures are an emulation: native 960x544 HUD pixels pasted by hand-made rectangles (a little 3D background comes with them) over the upscaled scene. Pause menu and wipes are not in the route: untested.
- llvmpipe's 2x MSAA pattern is not necessarily the Vita's 2x.

## Scales
Ladder rungs are per side of the game's 640x448 (1.0 = 286k px); GC's 72 % of 960x544 is 270k px = rung 0.97. Fractions of 960x544 used for the 16:9 runs: 0.86 = 826x468, 0.78 = 749x424, 0.72 = 691x392, 0.62 = 595x337, 0.52 = 499x283 (= ladder 0.70 in pixels), 0.46 = 442x250 (= ladder 0.62). Game-shape runs: 640x448, 622x436 (= 0.72 in pixels), 535x375 (= 0.62), 448x314 (= ladder 0.70).

| ladder rung | size (game shape) | pixels vs GC | stretch | k today (cap 0.8) | LOD bias 60 % (raw 1/8 level) |
|---|---|---|---|---|---|
| 1.20 | 768x538 | 1.53 | 1.12 | 0.35 | -1 |
| 1.00 | 640x448 | 1.06 | 1.35 | 0.59 | -2 |
| 0.93 (LOCK60 start) | 595x417 | 0.92 | 1.45 | 0.67 | -3 |
| 0.86 | 550x385 | 0.78 | 1.57 | 0.76 | -3 |
| 0.78 | 499x349 | 0.64 | 1.73 | 0.80 (0.86) | -4 |
| 0.70 (LOCK60 floor) | 448x314 | 0.52 | 1.93 | 0.80 (0.96) | -5 |
| 0.62 | 397x278 | 0.41 | 2.18 | 0.80 (1.08) | -5 |

## Scores against native 960x544 (mean of 8 scenes; eqcost.txt, table-b60.tsv)
| picture | pixels | SSIM | PSNR | gradient / native | overshoot (levels) |
|---|---|---|---|---|---|
| GC: 691x392 bilinear | 271k | 0.9392 | 32.19 | 0.788 | 0.054 |
| 691x392 combined (16:9) | 271k | 0.9390 | 32.10 | 0.867 | 0.087 |
| 622x436 combined (game shape, same pixels) | 271k | 0.9497 | 32.61 | 0.879 | 0.063 |
| 640x448 combined (rung 1.0) | 287k | 0.9531 | 32.74 | 0.874 | 0.044 |
| 714x401 combined (rung 1.0 in 16:9, SHAPE=1) | 286k | 0.9470 | 32.56 | 0.871 | 0.066 |
| 595x337 combined | 201k | 0.9313 | 30.89 | 0.855 | 0.130 |
| 535x375 combined (game shape, same pixels) | 201k | 0.9370 | 31.54 | 0.866 | 0.081 |
| 499x283 combined | 141k | 0.9109 | 29.82 | 0.812 | 0.145 |
| 448x314 combined (ladder 0.70, game shape) | 141k | 0.9167 | 30.02 | 0.815 | 0.103 |
| 442x250 combined | 111k | 0.8942 | 28.92 | 0.775 | 0.223 |
Bilinear against combined is SSIM-equal at every size (within 0.0004) with +0.08..+0.11 gradient energy and +0.03..+0.05 levels of ringing.

## Per question
**Sharpening strength k 0.6 / 1.0 / 1.4** (595x337: SSIM 0.9326 / 0.9313 / 0.9288, bilinear 0.9317; gradient 0.81 / 0.86 / 0.91; overshoot 0.10 / 0.13 / 0.17). Fidelity is flat, crispness rises, ringing rises: a taste dial, not a defect. A grid over the edge term's own constants (E 2-12, offset 0-0.3, edgegrid-595.txt, linegrid-691.txt) only slides along the same sharpness-against-ringing curve, so there is **no shader text change worth a pack**. At the floor the 0.8 cap binds (0.96-1.1 uncapped): x1.4 there gives gradient 0.86 against 0.81 for SSIM -0.003; I left it. **Keep SHARPEN_SCALE 1.0.**
**What I see** (sheets in index.md): no ringing on HUD or menu text with `combined`, visible dark halos on text and sprites with `fast` and with `combined` without its edge term; pitch lines stay soft in every variant that has the edge term: line contrast 0.77 (bilinear) / 0.79 (combined) of native at 691x392, 0.68 at 499x283, 0.92 only with no edge term (which rings on the players); the grass speckle of the native pitch is simply absent at low size (high-band energy 0.64 at 0.97, 0.51 at 0.70, 0.46 at 0.62) - only the texture LOD bias can give some of it back, and the host cannot show that.
**Edge smoothing on/off.** SMOOTH's smoothing step (`MSC_VITA_EDGE_AA`) does not visibly remove stair steps at 0.52 (sheet-edgeaa-499-closeup.png); what the edge terms do is stop ringing (overshoot 0.24 with, 0.44 without, at 499x283; `combined` 0.23). SMOOTH and `combined` differ by 48 dB. Real anti-aliasing comes only from MSAA.
**MSAA 2x at the lower size against no MSAA at the next size up (equal cost by the model, 2x = 1.3).** Resolution wins on fidelity every time: 442x250+2x (144k) 0.8959 against 499x283 (141k) 0.9109; 499x283+2x (184k) 0.9125 against 595x337 (201k) 0.9313; 595x337+2x (261k) 0.9334 against 691x392 (271k) 0.9390; 691x392+2x (352k) 0.9407 against 749x424 (318k) 0.9562. MSAA lowers the high band (0.53 against 0.58) and pitch-line contrast (0.65 against 0.73 of native at 595x337); what it buys is clean stair edges (sheet-equalcost-closeup.png, -zoom, -players: the diagonal edges and outlines are smooth, native is not). Edges against detail; the measurements side with detail. Motion error between consecutive frames (|dV - dN| / |dN|): 0.469 with and without MSAA, 0.42 at 691x392, 0.54 at 499x283: lower size, not MSAA, makes it move differently. **The ladder keeps MSAA at and above the game's size only; do not add 2x below it** unless the owner prefers smooth edges to detail (that is a taste decision; the pictures are there).
**Picture shape** (equal pixels, game shape against 16:9 = `MSC_VITA_SHAPE=1`): SSIM 0.9531 against 0.9470 at rung 1.0, 0.9497 against 0.9390 at 0.97, 0.9370 against 0.9313 at 0.62, 0.9167 against 0.9109 at the ladder's 0.70; PSNR +0.2..+0.65 dB, less ringing; wide play +0.020 SSIM, zoomed +0.012, goal banner +0.05, menus equal. The pitch view compresses detail vertically, so vertical pixels are worth more. **Keep SHAPE off.**
**Shimmer / crawl** (two consecutive frames, zoomed and wide pairs): energy of the frame-to-frame change against native's 0.84 (bilinear) / 0.89 (combined) at 691x392, 0.82 at 499x283; motion error unchanged by sharpening (0.421 / 0.417 at 691x392, 0.541 / 0.542 at 499x283). So the combined pass adds no measurable crawl of its own; texture shimmer from the LOD bias is not testable here (Galaxy's console: 60 % good, 100 % shimmers).

## HUD (item 4)
Yes: the HUD is drawn in the render target at the 3D size and blurred by the upscale. At 499x283 and 448x314 the clock, score digits, portrait art, "BLOCK THE BALLS" prompt and goal banner text are visibly soft against native (hud-*-mega-text.png, hud-*-match-topleft.png; large translucent goal digits barely change). Native-HUD composite at 0.70 (half GC's pixels) has text as crisp as native and clearer than GC's (sheet-bar-mega-text.png). Not ready to be the build default: never run on a console; per the code the run is cut at any copy that reads the EFB, a pass, an effect draw, shader work and destination-alpha blends, so in frames with those (pause menu with its blur? wipes?) the HUD would flip between sharp and soft - unknown here; it also costs about 1.8 of the HUD's old pixel cost at 640x448 (counted) and has no split within 3 % of the screen's size. Galaxy measured and dropped its own native HUD ("no visible sharpening"); Strikers' HUD is large 640x448-canvas text, so the effect is visible here, but that is a host-picture judgement. **Proposed: LOCKED 60 supporting default (done in the worktree), AUTO modes after the console kit run `hudnative`** (cost and flip check in match, pause menu, wipe).

## Colour (item 5)
No banding added by the pass: rounding to 8 bits leaves a low-frequency error of 0.035 level rms against the float result (0.03 at every size), and the bands in the picture are the 8-bit render's own, the same as native's (sheet-banding-mega-glow.png at x12 shows the glow's colour rings, not steps). An ordered 4x4 or gradient-noise dither before the final rounding lowers that low-frequency error to 0.026 and adds 0.1 level rms of noise: nothing visible gained. **Not worth it.** Pack impact if wanted anyway: `combined`'s text is not in pack 15 (cold compile today), so a dither costs no pack entry now, but it adds a fragment-coordinate hash (3-4 instructions on a 15-instruction program, ~25 % of the pass's arithmetic, none of its fetches) and must be settled before the pack that will contain `combined` is regenerated.

## Proposal in vita_plan.cpp (commit 1bd4c19 in agent/look)
- Per rung (all rungs): `MSC_VITA_UPSCALE=combined`, `MSC_VITA_SHARPEN_SCALE=1.0`, cap 0.8, `MSC_VGX_LODBIAS_RUNG=60`, `MSC_VITA_ANISO_MIN=128`, `MSC_VITA_SHAPE` off, MSAA only at/above native: unchanged from today (the table above is what each rung gets). Comment in the plan records why.
- Changed: `MSC_VGX_HUDNATIVE=1` added to the LOCKED 60 supporting defaults (kLock60; LOCKED 60 itself is off by default). Picture difference from today under LOCKED 60 (rungs 0.93 to 0.70 in open play): crisp HUD text and portraits instead of a blurred one; the 3D picture is the same; HUD pixel cost about 1.8x. TECHNICAL.md row and CHANGELOG line added.

## Not verified
Everything is from one route, one stadium, 10 frames, host llvmpipe; LOD bias strength, anisotropic floor, half precision of the Vita shader, HUDNATIVE itself (cost, flip, pause menu, wipe), the Vita's 2x MSAA pattern, and any timing. The judging console A/B: `lodrung0` against default with screen captures at the 0.70 rung; `hudnative` kit run; a 60 fps open-play run at 0.70 with and without HUDNATIVE.
