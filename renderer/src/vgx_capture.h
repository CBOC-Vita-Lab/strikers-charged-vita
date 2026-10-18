// Command-buffer capture (workstream FI): windows of presented frames written to files, and
// read back by an offline tool (tools/fi_replay) that executes them on a back end without
// the game. A command buffer is self-contained (vgx_cmd.h), so a file holds everything a
// back end is ever given for those frames: first one buffer with every texture, static
// buffer and shader that was alive when the window began (the "preamble"), then the frames.
//
//   MSC_VGX_CAPTURE=<dir>            where the files go (<dir>/cap_<first>_<last>.vgxcap)
//   MSC_VGX_CAPTURE_LIST=a-b,c-d     windows of frame numbers (CmdBuf::frame, the renderer's
//                                    count: the first presented frame is 1)
// Nothing is tracked and nothing is written unless both are set. What a file does NOT hold:
// the contents of EFB-copy textures made before the window (a picture rendered once and
// sampled for many frames, e.g. the pause menu's backdrop, has to be made inside the window).
#pragma once
#include "vgx_cmd.h"

#include <cstdio>
#include <deque>
#include <string>

namespace vgx::capture {

bool Enabled();
// Called by the front end with every finished frame, before the back end sees it.
void OnFrame(const CmdBuf& buf);

// Reading. The buffers' shader pointers refer to objects owned by `shaderStore`.
struct File {
    std::deque<CmdBuf> frames; // [0] = preamble (no present), then the window's frames
    std::deque<ShaderSource> shaderStore;
};
bool Load(const char* path, File& out);

} // namespace vgx::capture
