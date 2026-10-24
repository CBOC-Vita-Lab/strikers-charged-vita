# Console Tests For The Next Optimisations

The remaining slow scenes are limited by the graphics chip. The emulator cannot measure that, so
the next changes depend on numbers from a real Vita. Every switch below is a line in
`ux0:data/mscharged/np-player.txt`. Remove it afterwards.

Measure with a logging build (`NP_LOGGING=1`: `[fps]` lines in `log.txt`), or with the performance
recorder (`MSC_TOUCH_DIAG=1`, see [Diagnostics](DIAGNOSTICS.md)). For each switch, play the same
scene twice: once without the line and once with it.

| Question | Scene | Switch | What it tells us |
| --- | --- | --- | --- |
| What does the sharpening cost? | A match and the main menu | default, then `MSC_VITA_UPSCALE=off` | Expect well under 1 fps difference. If it costs more, make OFF the default again |
| Does Mega Saves hold 30 fps? | Strikers 101 > Mega Saves | default, then `MSC_MEGA_RENDER=off` | The default should hold 30. With `off` it shows the old speed. If the default looks too soft, try `MSC_MEGA_RENDER=512x352` |
| Does a match reach 60? | A full match | default (AUTO), then `MSC_FPS=30` | Logging builds print `[fps] auto: 30 -> 60` and `[vgx] dynamic resolution: ... level` lines. If 60 is rare and the reason is a CPU core, the GPU levels cannot help |
| What do the GPU timers cost? | A match | `MSC_DYNRES=off` | The timers add one notification per render scene. Expect no visible difference |
| Is the larger picture worth it? | Main menu, a match at 30 | `MSC_DYNRES_MAX=1.0` | The default may draw up to 768x544 when the GPU has room. If that ever costs frames, cap it |
| What do the low-resolution effects save? | Strikers 101 > Mega Saves | default, then `MSC_FX_LOWRES=0` | The default should be much faster. `MSC_FX_LOWRES=4` saves more if it is still slow |
| Does the anti-aliasing pay off? | A match, the main menu | Settings > Anti-Aliasing 2X | Smoother edges, small but visible. Keep it only if the frame rate holds |
| Does anisotropic filtering work? | A match | Settings > Distant Textures SHARP (TEST) | The far half of the pitch should look crisper. If textures look wrong, the filter values are not what we think |
| How much does the picture size cost? | Mega Saves, main menu, stadium select | `MSC_VITA_RENDER=480x336` | A big gain means fill rate (pixels). Little change means geometry or render-pass switches |
| Is the touch and stick delay gone in Mega Saves? | Strikers 101 > Mega Saves | default, then `MSC_VITA_QUEUE=off` | The logging build's `[lat]` lines give pad-to-screen delay; `[vgx-vita] GPU queue depth` lines show when the limit drops to 1 |
| Can input feel quicker still? | Mega Saves, a match | `MSC_VITA_DISPLAY_BUFFERS=2` | One frame fewer waits in the display queue. Check that the frame rate holds |
| Is the menu limited by geometry? | Main menu | `MSC_VITA_RENDER=480x336` | The main menu draws about 105,000 triangles per frame (a 3D scene of about 52 draws), then an 8-pass glow chain |

What a main-menu frame contains (traced in the emulator with `MSC_VGX_PASSES=<frame>`):

- **3D background:** 52 draws, about 104,700 triangles, at 640x448. This includes ten copies
  of a 4,600-triangle mesh and two large meshes of 8,800 and 14,000 vertices.
- **Glow chain:** a down-sample to 320, 160, 80 and 40 pixels, then up-sample back. Each step
  is its own render pass.
- **Composite and interface:** about 35 to 50 draws of about 1,000 triangles.

If the resolution test barely helps there, the triangle count is the cost. Lower-detail menu
models, or drawing the menu background less often, would then be the next things to try.
