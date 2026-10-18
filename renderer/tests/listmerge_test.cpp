// W5 SEED: host test of vita/vgx_listmerge.h.   listmerge_test <first.shl> <second.shl> <out.shl>
// The result must equal `python3 -I tools/seed/shl.py merge -o ref.shl first second` (np repo) byte for byte.
#include "../vita/vgx_listmerge.h"
#include <cstdio>
static std::string Slurp(const char* p) {
    std::string s;
    if (FILE* f = std::fopen(p, "rb")) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); }
    return s;
}
int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s first second out\n", argv[0]); return 2; }
    const std::string a = Slurp(argv[1]), b = Slurp(argv[2]);
    const auto A = vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(a.data()), a.size());
    const auto B = vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    std::printf("first: ok=%d vs %zu fs %zu prog %zu var %zu | second: ok=%d vs %zu fs %zu prog %zu var %zu\n", A.ok, A.vs.size(), A.fs.size(), A.prog.size(), A.var.size(),
                B.ok, B.vs.size(), B.fs.size(), B.prog.size(), B.var.size());
    unsigned added = 0;
    const std::string m = vgx_listmerge::Merge(A, B, &added);
    if (FILE* f = std::fopen(argv[3], "wb")) { std::fwrite(m.data(), 1, m.size(), f); std::fclose(f); }
    std::printf("merged: %zu bytes, %u records came from the second\n", m.size(), added);
    return 0;
}
