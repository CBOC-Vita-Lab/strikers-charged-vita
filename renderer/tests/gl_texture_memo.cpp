// glTextureMemo.h against a model of the lookup it saves: a random stream of binds, additions, removals and index changes, each change telling the memo, as the writers do.
#include <cstdio>
#include <cstdint>
#include <map>
#include <random>
#include "NL/gl/glTextureMemo.h"

static glTextureMemoTable table;
int main() {
    std::mt19937 rng(7);
    std::map<unsigned long, unsigned long> trees;
    unsigned long gen = 0, walks = 0, binds = 0;
    auto walk = [&](unsigned long h) { ++walks; auto it = trees.find(h); return it == trees.end() ? 0xFFFFul : it->second; };
#ifdef BREAK_NO_BUMP
    const bool bump = false;
#else
    const bool bump = true;
#endif
    static const unsigned long kSpecial[] = {0xFFFFFFFFul, 0, 1024, 1 << 20, 0xFFFFFFFEul};
    for (long n = 0; n < 6000000; ++n) {
        const unsigned long r = rng();
        const unsigned long h = (r & 15) == 0 ? kSpecial[(r >> 4) % 5] : (r >> 8) % 600 * 2654435761ul % 0xFFFFFFFFul;
        if ((n & 0xFFF) == 0) {  // a load or an unload
            for (int k = 0; k < 40; ++k) {
                const unsigned long a = rng() % 600 * 2654435761ul % 0xFFFFFFFFul;
                if (rng() & 1) trees[a] = rng() & 0xFFFF;
                else trees.erase(a);
            }
            trees.erase(0xFFFFFFFFul);
            if (bump) ++gen;
        }
        const unsigned long want = trees.count(h) ? trees[h] : 0xFFFFul;
        const unsigned long got = glTextureMemoGet(table, gen, h, walk);
        ++binds;
        if (got != want) { std::printf("FAIL: handle %lx memo %lx truth %lx\n", h, got, want); return 1; }
    }
    if (walks * 2 > binds) { std::printf("FAIL: memo saved little (%lu walks, %lu binds)\n", walks, binds); return 1; }
    // a zeroed table holds nothing, not even handle 0
    static glTextureMemoTable fresh;
    if (glTextureMemoGet(fresh, 0, 0, [](unsigned long) { return 77ul; }) != 77ul) { std::printf("FAIL: zeroed table answered\n"); return 1; }
    // the generation that follows wraps round to a live tag of 0
    static glTextureMemoTable wrap;
    if (glTextureMemoGet(wrap, 0xFFFFFFFFul * 1ul, 5, [](unsigned long) { return 9ul; }) != 9ul) { std::printf("FAIL: wrap\n"); return 1; }
    std::printf("gl_texture_memo ok (%lu binds, %lu walks)\n", binds, walks);
}
