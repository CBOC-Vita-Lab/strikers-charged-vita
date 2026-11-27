// MSC_VGX_LADDER_ALIAS, the host GL model (wave 19 alias19). On the console the depth surfaces of the picture-size EFB targets are windows onto one block per sample count (vitaGL 0044);
// the host cannot run vitaGL, so the desktop back end does the same with the depth TEXTURE of the EFB: one texture at the capacity (the held size) that every one-sample target of the
// ladder attaches, instead of one texture each. A target of w x h uses the window [0,w) x [0,h) of it, exactly as the console's smaller picture uses the first h rows (stride = capacity) of
// the shared surface; everything that samples the texture maps its coordinates by (w / cw, h / ch). A picture identical to the one without sharing therefore proves that no target reads depth
// another size left (every use of the block clears or fully overwrites the window it uses), and a block that is not cleared must show (MSC_VGX_ALIAS_MUTANT, a test hook).
// Header-only and pure: the decisions, so a host test (renderer/tests/depthshare.cpp) runs them with the vitaGL block table's own code (depth_share_core.h).
#pragma once

namespace vgx::aliasdepth {

// a target of w x h takes part when it fits the capacity (the console: width <= cw and height <= ch; no stencil, no pass object)
inline bool Fits(int w, int h, int cw, int ch) { return cw > 0 && ch > 0 && w > 0 && h > 0 && w <= cw && h <= ch; }
// the part of the shared texture a w x h target uses, as texture coordinates (the window starts at 0,0)
inline float WinU(int w, int cw) { return cw > 0 && w > 0 && w <= cw ? static_cast<float>(w) / static_cast<float>(cw) : 1.0f; }
inline float WinV(int h, int ch) { return ch > 0 && h > 0 && h <= ch ? static_cast<float>(h) / static_cast<float>(ch) : 1.0f; }

} // namespace vgx::aliasdepth
