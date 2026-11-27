// galaxyport18: vgx_copyguard.h on a hand-made buffer whose copyTex is empty (the replay36 shape: the command list copied, the sibling vectors not).
//   g++ -std=gnu++17 -fsanitize=address,undefined -I../src copy_guard.cpp -o /tmp/copy_guard && /tmp/copy_guard      (run_copy_guard_test.sh also builds mutants)
#include "vgx_copyguard.h"

#include <cstdio>

int main() {
    using namespace vgx;
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } };
    CmdBuf b;
    Cmd c{};
    c.type = CMD_COPY_TEX;
    c.arg = 0;
    b.cmds.push_back(c);                       // names copyTex[0] of an empty vector
    const CopyTexCmd& dead = copyguard::At(b, b.cmds[0].arg);
    check(dead.skip && !dead.texId && !dead.fromPass && !dead.srcTex && !dead.depth, "an empty copyTex reads a dead record");
    check(copyguard::Count() == 1, "the miss is counted once");
    CopyTexCmd real; real.texId = 7; real.dstW = 64; real.dstH = 32;
    b.copyTex.push_back(real);
    const CopyTexCmd& ok0 = copyguard::At(b, 0);
    check(&ok0 == &b.copyTex[0] && ok0.texId == 7, "a record in range is the record itself (no copy)");
    check(copyguard::Count() == 1, "an in-range read is not counted");
    const CopyTexCmd& edge = copyguard::At(b, 1);   // one past the end
    check(edge.skip && copyguard::Count() == 2, "index == size is out of range");
    check(copyguard::At(b, 0xFFFFFFFFu).skip && copyguard::Count() == 3, "a huge index is out of range");
    for (int i = 0; i < 10; ++i) (void)copyguard::At(b, 5);   // only the first lines are logged, all are counted
    check(copyguard::Count() == 13, "every miss counted");
    std::printf("%s\n", failures ? "FAILED" : "copy_guard ok");
    return failures ? 1 : 0;
}
