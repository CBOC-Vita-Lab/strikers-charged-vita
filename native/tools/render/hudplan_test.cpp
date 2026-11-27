// The run of 2D layers that MSC_VGX_HUDNATIVE draws on the display (vgx_hudplan.h): what is in it, and above all what must stop it.
#include <cstdio>
#include <vector>
#include "vgx_hudplan.h"
using namespace vgx;
using namespace vgx::backend;
namespace {
struct Frame {
    std::vector<Cmd> cmds;
    std::vector<DrawRec> draws;
    std::vector<CopyTexCmd> copies;
    void Draw(int layer, bool depth = false, bool fx = false, uint8_t src = BF_SRC_ALPHA, bool blend = true) {
        DrawRec d;
        d.layer = static_cast<uint8_t>(layer);
        d.fx = fx;
        d.rs.depthTest = depth;
        d.rs.blend = blend;
        d.rs.blendSrc = src;
        d.rs.blendDst = BF_INV_SRC_ALPHA;
        draws.push_back(d);
        cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(draws.size() - 1)});
    }
    void Copy(bool skip, bool color, bool depth, int w = 640, int h = 448) {
        CopyTexCmd t;
        t.skip = skip;
        t.src.w = static_cast<int16_t>(w);
        t.src.h = static_cast<int16_t>(h);
        t.clear.enable = true;
        t.clear.color = t.clear.alpha = color;
        t.clear.depth = depth;
        copies.push_back(t);
        cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(copies.size() - 1)});
    }
    void Add(CmdType type) { cmds.push_back(Cmd{type, 0}); }
    HudPlan Plan() const { return PlanHudRun(cmds.data(), cmds.size(), draws.data(), copies.data(), 640, 448); }
};
int fails = 0;
void Check(const char* what, const HudPlan& p, int start, uint32_t draws, uint32_t depthDraws = 0) {
    if (p.start == start && (start < 0 || (p.draws == draws && p.depthDraws == depthDraws))) return;
    std::printf("FAIL %s: start %d draws %u depth %u, wanted %d %u %u\n", what, p.start, p.draws, p.depthDraws, start, draws, depthDraws);
    ++fails;
}
} // namespace
int main() {
    { Frame f; f.Draw(10); f.Draw(20); f.Draw(38); f.Draw(43); f.Add(CMD_COPY_DISP); f.Add(CMD_PRESENT); Check("a plain frame", f.Plan(), 2, 2); }
    { Frame f; f.Draw(10); f.Add(CMD_COPY_DISP); Check("nothing 2D", f.Plan(), -1, 0); }
    { Frame f; f.Draw(38); Check("no display copy", f.Plan(), -1, 0); }
    { Frame f; f.Draw(10); f.Draw(38); f.Copy(false, false, false); f.Draw(39); f.Add(CMD_COPY_DISP); Check("a copy that reads the EFB", f.Plan(), 3, 1); }
    { Frame f; f.Draw(38); f.Add(CMD_PASS_BEGIN); f.Draw(39); f.Add(CMD_COPY_DISP); Check("a pass", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Add(CMD_SET_TARGET); f.Draw(39); f.Add(CMD_COPY_DISP); Check("a new target", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Add(CMD_SHADER); f.Draw(39); f.Add(CMD_COPY_DISP); Check("shader work", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Add(CMD_TEX_UPLOAD); f.Draw(39); f.Add(CMD_COPY_DISP); Check("an upload stays in", f.Plan(), 0, 2); }
    { Frame f; f.Draw(38); f.Copy(true, false, true); f.Draw(40, true); f.Draw(40, true); f.Add(CMD_COPY_DISP); Check("the wipe's depth clear stays in", f.Plan(), 0, 3, 2); }
    { Frame f; f.Draw(38); f.Copy(true, true, true); f.Draw(40); f.Add(CMD_COPY_DISP); Check("a clear with colour", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Copy(true, false, true, 320, 224); f.Draw(40); f.Add(CMD_COPY_DISP); Check("a clear of a part", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Draw(40, false, true); f.Draw(41); f.Add(CMD_COPY_DISP); Check("an effect draw", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Draw(40, false, false, BF_DST_ALPHA); f.Draw(41); f.Add(CMD_COPY_DISP); Check("destination alpha", f.Plan(), 2, 1); }
    { Frame f; f.Draw(38); f.Draw(40, false, false, BF_DST_ALPHA, false); f.Add(CMD_COPY_DISP); Check("no blend, no matter", f.Plan(), 0, 2); }
    { Frame f; f.Draw(38); f.Add(CMD_COPY_DISP); f.Draw(39); f.Add(CMD_COPY_DISP); Check("the last display copy", f.Plan(), 2, 1); }
    if (fails) return 1;
    std::printf("hudplan: all cases pass\n");
    return 0;
}
