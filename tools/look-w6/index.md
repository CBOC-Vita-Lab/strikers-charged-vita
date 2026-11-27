# w6/look contact sheets (all crops are 1:1 pixels of the 960x544 picture, shown 2x with nearest-neighbour; labels are in the pictures)
Produced by tools/*.py from host-harness renders (/home/claude/work/pics/look-*; frames 720 title sky, 1300 captain menu, 2040/2041 zoomed play, 2400 close-up cut-scene, 2520 and 2760 Mega Strike, 3120 goal + banner, 3600/3601 wide play).
"today" = combined upscale, k = sqrt(stretch-1) capped 0.8, scaled by MSC_VITA_SHARPEN_SCALE; "GC" = plain bilinear from the 72 % render (691x392, no AA, no sharpening, bias 0).

## The three most telling
- sheet-bar-mega-text.png : HUD / text. GC 691x392 (271k px) | Charged same cost | Charged MSAA2 | ladder 0.70 (half the cost, text blurred) | ladder 0.70 + native HUD (emulated HUDNATIVE: as sharp as native).
- sheet-equalcost-closeup.png : edges and detail at equal cost. native | GC | 691x392 combined | 595x337 MSAA2 combined | 595x337 | 749x424. MSAA is the only one that cleans the stair edges; the larger plain picture keeps more detail.
- sheet-bar-pitch-line.png : the pitch line and a player at the GC cost and at the ladder's 0.70: lines are 0.77-0.80 of native contrast in every variant with the edge term, 0.67 at 0.70.

## Per question
- Picture-size ladder, today's default against GC (bilinear): sheet-691x392-*.png (wide-line, wide-players, zoom, closeup, menu-text, goal-hud: native | GC | today | x1.4 | x0.6 | fast | noedge) and sheet-499x283-*.png (the same at 0.52 of the screen = ladder 0.70). sheet-bar-*.png: GC | today same cost | MSAA2 | ladder 0.70 | ladder 0.70 + native HUD (pitch-line, zoom, banner, menu-text, mega-text).
- Upscale programs on the real GL shaders (look-gl691): sheet-modes-691-{zoom,players,menu}.png: native | bilinear | combined | smooth | fast | cas.
- Edge smoothing on/off: sheet-edgeaa-499-closeup.png (bilinear | SMOOTH EDGE_AA=1 | SMOOTH EDGE_AA=0 | combined).
- MSAA 2x at the lower size against the next size up at equal cost: sheet-equalcost-{closeup,closeup2,zoom,players}.png.
- Picture shape (16:9 SHAPE=1 against the game's shape, equal pixels): sheet-shape-{691,595,499}-{menu-text,pitch-line,banner-text}.png (native | 16:9 | game shape).
- HUD drawn at 3D size, blurred, against the native HUD (emulation): hud-{448x314,499x283,691x392}-{match-topleft,match-score,goal-digits,mega-text}.png.
- Banding: sheet-banding-mega-glow.png (x12 contrast: native | 595x337 combined 8-bit | + ordered dither).

## Numbers
table-b60.tsv (per scene, per size, per variant: PSNR, SSIM, gradient energy, high-band energy, overshoot, shimmer); eqcost.txt (equal-cost pairs); ksweep-*.txt, edgegrid-595.txt, linegrid-691.txt, linecontrast.txt; validate.txt (offline shaders against GL).
