// Host test of the Dump material's text rewrite (vgx_dumpmat25.h): the real generated text of fragment program 1DEECD4F (tests/data/dumpmat25_fs.txt) is recognised, a layer
// constant replaces exactly its fetch, the far variant keeps the fetch in the else branch, other programs are left alone. DUMPMAT_SRC names the header directory (the run script
// points it at a mutated copy to prove the test can fail).
#include "vgx_dumpmat25.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
using namespace vgx::dumpmat25;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
static int Count(const std::string& s, const std::string& k) { int n = 0; for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p + 1)) ++n; return n; }
int main(int argc, char** argv) {
    std::ifstream f(argc > 1 ? argv[1] : "data/dumpmat25_fs.txt");
    CHECK(f.good());
    std::stringstream ss; ss << f.rdbuf();
    const std::string t = ss.str();
    CHECK(IsMaterial(t));
    CHECK(!IsMaterial("void main() { gl_FragColor = vec4(1.0); }"));
    std::string t3 = t; t3.replace(t3.find("texture2D(u_tex3, uv3).rgba"), 27, "vec4(1.0)");
    CHECK(!IsMaterial(t3));   // a program without the third layer fetch is not the material
    const Consts k = ReadConsts();
    std::string o;
    CHECK(!Rewrite(t, 0, k, 0.3f, o));                      // nothing asked
    CHECK(!Rewrite("void main(){}", L1, k, 0.3f, o));       // not the material
    CHECK(Rewrite(t, L1, k, 0.3f, o));
    CHECK(Count(o, "texture2D(") == 3 && o.find("texture2D(u_tex1") == std::string::npos && o.find("texture2D(u_tex2") != std::string::npos && o.find("texture2D(u_tex3") != std::string::npos && o.find("texture2D(u_tex0") != std::string::npos);
    CHECK(o.find("vec4(0.17300, 0.23500, 0.23200, 1.00000)") != std::string::npos);
    CHECK(Rewrite(t, L1 | L2, k, 0.3f, o));
    CHECK(Count(o, "texture2D(") == 2 && o.find("texture2D(u_tex1") == std::string::npos && o.find("texture2D(u_tex2") == std::string::npos);
    CHECK(Rewrite(t, L3, k, 0.3f, o));
    CHECK(Count(o, "texture2D(") == 3 && o.find("texture2D(u_tex3") == std::string::npos);
    CHECK(Rewrite(t, FAR2 | FAR3, k, 0.3f, o));
    CHECK(Count(o, "texture2D(") == 4);                      // all four fetches stay (in the else branches)
    CHECK(Count(o, "if (gl_FragCoord.z > 0.300000)") == 2);
    // the rest of the program is untouched: the fog block and the multiplies of the stages
    CHECK(o.find("fin.rgb = mix(fin.rgb, u_tev[10].rgb, fg);") != std::string::npos && o.find("prev.rgb * tex.rgb") != std::string::npos);
    CHECK(Rewrite(t, L1 | FAR3, k, 0.5f, o) && Count(o, "texture2D(") == 3 && Count(o, "if (gl_FragCoord.z > 0.500000)") == 1);
    std::printf("dumpmat25: ok\n");
    return 0;
}
