// Host test of vgx_shadertab.h (MSC_DM_COMPACT): the shader tables read, write and size exactly as the plain vector they replace, but an index far above the real ids (0xE000 / 0xF000 derived programs)
// stores only the few entries it needs. The split is 16 here (SHADERTAB_SPLIT) so the test is small.
#define SHADERTAB_SPLIT 16
#include "vgx_shadertab.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    ShaderTab<unsigned> t; std::vector<unsigned> ref;
    CHECK(t.size() == 0);
    // the way CompileShaderSlot grows: size() <= index -> resize(index + 1), then write
    const size_t seq[] = {3, 1, 7, 15, 16, 20, 18, 5, 40, 17};
    for (size_t idx : seq) {
        if (t.size() <= idx) t.resize(idx + 1, 0);
        if (ref.size() <= idx) ref.resize(idx + 1, 0);
        t[idx] = static_cast<unsigned>(idx * 10 + 1); ref[idx] = static_cast<unsigned>(idx * 10 + 1);
    }
    CHECK(t.size() == ref.size());
    for (size_t i = 0; i < ref.size(); ++i) CHECK(static_cast<const ShaderTab<unsigned>&>(t)[i] == ref[i]);   // holes read zero
    // far entries: only the low part and the high part are stored, never the gap
    ShaderTab<unsigned> far_; far_.resize(61441, 0); far_[61440] = 9; far_[2] = 4;
    CHECK(far_.size() == 61441 && far_[61440] == 9 && far_[2] == 4);
    CHECK(far_.stored() == 3 + (61441 - 16));   // split 16 in this test: low 3, high 61425
    // a real id that appears after the derived one grows only the low part
    ShaderTab<unsigned> g; g.resize(40, 0); g[39] = 1; g[5] = 2; CHECK(g.size() == 40);
    ShaderTab<unsigned> h; h[20] = 1; h[3] = 2; CHECK(h.size() == 21 && h.stored() == 4 + 5);
    // read-only access past the end is the zero value, not a crash; get() never mutates lo_ even when hi_ is populated (Carbon #3)
    const ShaderTab<unsigned>& c = h; CHECK(c[1000] == 0 && c[10] == 0);
    CHECK(h.get(10) == 0 && h.get(1000) == 0 && h.stored() == 4 + 5);
    // resize() into lo_ after hi_ is populated must still grow lo_ with fill value v (Carbon #3)
    h.resize(12, 77);
    CHECK(h.stored() == 12 + 5 && h.get(11) == 77 && h.get(3) == 2 && h.get(20) == 1);
    unsigned n = 0; h.each([&](unsigned x) { if (x) ++n; }); CHECK(n == 10);
    std::printf("shadertab: ok\n");
    return 0;
}
