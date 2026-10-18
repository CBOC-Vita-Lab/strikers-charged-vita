// Cross-check of two independent implementations of the same step: this directory's
// port_chunk_swap_tree (parse-time) against NPA's npa_convert (load-time, platform/assets), on the real
// disc files. For every chunk-tree file both convert a private copy; every chunk header my walk reaches
// must hold the same two words in NPA's output. (Payloads are compared by NPA's own verifiers, not here.)
//   NPA=/home/claude/msc/npw/NPA/platform/assets
//   gcc -O1 -I$NPA -c $NPA/np_*.c ; g++ -O1 -I<np>/include -I$NPA -o /tmp/xc tests/npa_crosscheck_test.cpp np_*.o
//   /tmp/xc /home/claude/msc/game/files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include "port/chunk_endian.h"
#include "np_assets.h"

static const char* exts[] = { ".sanim", ".shier", ".rlg", ".cam", ".nis", ".cph" };
static int files, fails;
static unsigned long totalChunks, totalBytes;

// walk my converted (host-order) tree and compare every header with NPA's output at the same offset
static int cmp_tree(const unsigned char* a, const unsigned char* b, const unsigned char* aend, const unsigned char* abase, int d, unsigned long* n)
{
    while (a + 8 <= aend)
    {
        uint32_t ia, sa, ib, sb;
        memcpy(&ia, a, 4); memcpy(&sa, a + 4, 4);
        memcpy(&ib, b, 4); memcpy(&sb, b + 4, 4);
        (*n)++;
        if (ia != ib || sa != sb) return 0;
        if ((ia & 0x80000000u) && d < 8)
        {
            const unsigned char* ce = (a + 8 + sa > aend) ? aend : a + 8 + sa;
            if (!cmp_tree(a + 8, b + 8, ce, abase, d + 1, n)) return 0;
        }
        unsigned long step = 8 + ((sa + 3u) & ~3u);
        a += step; b += step;
    }
    return 1;
}
static void scan(const std::string& dir)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st; if (stat(p.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) { scan(p); continue; }
        bool ok = false;
        for (const char* x : exts) { size_t l = strlen(x), n = strlen(e->d_name); if (n > l && !strcmp(e->d_name + n - l, x)) ok = true; }
        if (!ok) continue;
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) continue;
        std::vector<unsigned char> mine(st.st_size);
        if (fread(mine.data(), 1, mine.size(), f) != mine.size()) { fclose(f); continue; }
        fclose(f);
        std::vector<unsigned char> npa(mine);
        int zl = 0;
        NpaKind k = npa_classify(p.c_str(), &zl);
        NpaResult r; memset(&r, 0, sizeof r);
        int rc = npa_convert(k, npa.data(), npa.size(), &r);
        unsigned long got = port_chunk_swap_tree(mine.data(), mine.size());
        files++;
        if (rc < 0 || got == 0) { printf("SKIP/FAIL %s npa=%d(%s) mine=%lu\n", p.c_str(), rc, r.msg, got); fails++; continue; }
        unsigned long n = 0;
        if (!cmp_tree(mine.data(), npa.data(), mine.data() + mine.size(), mine.data(), 0, &n))
        { printf("MISMATCH %s after %lu chunks\n", p.c_str(), n); fails++; }
        totalChunks += n; totalBytes += mine.size();
    }
    closedir(d);
}
int main(int argc, char** argv)
{
    scan(argc > 1 ? argv[1] : ".");
    printf("%d files compared (%lu chunk headers, %lu bytes), %d failures\n", files, totalChunks, totalBytes, fails);
    return fails != 0;
}
