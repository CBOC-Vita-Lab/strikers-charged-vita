// Frame interpolation (workstream FI): present more pictures than the game builds.
//
// The game builds a frame (one CmdBuf) at its own rate; this module, which sits between the
// front end and the back end on the thread that owns the back end, shows one or more
// IN-BETWEEN pictures before it: the new frame's own command list, executed with the uniform
// blocks of every draw that could be paired with a draw of the previous frame replaced by a
// blend of the two frames' blocks (projection, model-view, texture matrices, matrix palette,
// lights), and with the vertex positions of paired draws of immediate geometry (sprites,
// impostor quads, HUD) blended likewise. Everything that cannot be paired, and everything
// that is neither a matrix nor a position (textures, texture coordinates, TEV colours, render
// state), is the new frame's: such things change at the built rate, half a presented frame
// early, never wrongly. Then the frame is executed as itself (ExecOpts::replay: nothing is
// created twice).
//
//   MSC_VGX_INTERP=0|1|2|32   0 off (default): Submit() is backend::Execute().
//                             1: one in-between picture per built frame (15 built -> 30 shown)
//                             2: two (20 -> 60)
//                             32: three pictures for two built frames (20 built -> 30 shown):
//                                 a frame is shown as itself only every second time
//   MSC_VGX_INTERP_LOG=1      one line per frame (pairing, cut decisions); 2: every unpaired or
//                             held draw as well
// Thresholds (see Params in vgx_interp.cpp): MSC_VGX_INTERP_JUMP, _CUT, _MINPAIR, _TEXM, _ROT.
//
// A frame is shown WITHOUT an in-between (as itself in the first slot, and it stays) when
// there is no previous frame, when most of what could be paired moved further than something
// that moves can (a camera cut), when too little could be paired (another scene), or when the
// frame replaces or frees a resource that its own earlier draws use, or does not leave the
// EFB in the state the frame before left it in (either way it cannot be executed twice).
#pragma once
#include "vgx_cmd.h"

namespace vgx::interp {

bool Enabled();
// Executes one finished frame on the calling thread (the back end's). With the switch off
// this is backend::Execute(buf) and nothing else. `buf` may be modified (blocks are appended
// to its uniform arena; its draw records and arena are kept for the next frame by swapping).
void Submit(CmdBuf& buf);

// The same machinery in steps, for the presenter (vgx_present.cpp), which shows a built frame over
// several display ticks that are not back to back: Begin() once per built frame (pairing against the frame
// kept by the previous End(); true = in-between pictures are possible, false = the frame can only be shown as
// itself: first frame, cut, another scene, ...), then any number of Blend() / Self() pictures, then End().
// `first` = the first execution of this frame (it creates what the frame creates; later ones are replays);
// `hidden` = executed but not presented (a frame the display clock jumped over). Not a frame: use Submit().
bool Begin(CmdBuf& buf, bool wantPairs = true); // wantPairs false: no pairing (a frame that will be shown as itself); returns false
void Blend(CmdBuf& buf, float t, bool first, bool hidden = false);
void Self(CmdBuf& buf, bool first, bool hidden = false);
void End(CmdBuf& buf);
const char* ReasonText(uint8_t reason);

// The host is told about every picture slot: `slot` counts from 0 within the built frame;
// `drawn` false means nothing was rendered for this slot (the previous picture is to stay);
// `t` is where the picture is between the previous frame (0) and this one (1).
using PictureHook = void (*)(uint64_t frame, int slot, bool drawn, float t);
void SetPictureHook(PictureHook hook);
PictureHook GetPictureHook();

struct FrameStats {
    uint64_t frame = 0;
    uint32_t draws = 0, staticDraws = 0;
    uint32_t paired = 0, pairedStatic = 0; // draws paired with a draw of the previous frame
    uint32_t jumps = 0;                    // paired draws shown with the new matrices because they moved too far
    uint32_t groupsAmbiguous = 0, groupsReordered = 0;
    uint32_t blocksBlended[UB_COUNT] = {}; // distinct blended blocks appended, by kind
    uint32_t blocksHeld = 0;               // texture-matrix / projection blocks left alone by their own tests
    uint32_t floatsAppended = 0;
    uint32_t vertsBlended = 0, vertsHeld = 0; // arena vertices given an in-between position / left as they are although they moved
    uint8_t pictures = 0;                  // in-between pictures drawn (0: shown without)
    uint8_t reason = 0;                    // why not: see kReason* in the source
};
const FrameStats& LastStats();
// Totals since the start (for the exit report of a tool).
struct Totals { uint64_t frames = 0, withInBetween = 0, draws = 0, paired = 0, jumps = 0, floats = 0, cuts[8] = {}; };
const Totals& GetTotals();
void PrintTiming(); // MSC_VGX_INTERP_TIMING=1
// Forget the previous frame (the next one is shown without an in-between).
void Reset();
// Sets the mode without the environment (tools). Same values as MSC_VGX_INTERP.
void SetMode(int mode);

} // namespace vgx::interp
