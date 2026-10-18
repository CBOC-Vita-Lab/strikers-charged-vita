// Host test for include/port/chunk_endian.h against real game files (reads the owner's extracted disc,
// nothing is written or copied).  Build/run:
//   g++ -O1 -I<np>/include -o /tmp/ce tests/chunk_endian_test.cpp && /tmp/ce /home/claude/msc/game/files
// For every file with one of the chunk extensions: count chunks by an independent big-endian walk, swap
// with port_chunk_swap_tree, walk again in host order the way nlChunk does, require equal counts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include "port/chunk_endian.h"

static const char* exts[] = { ".sanim", ".shier", ".rlg", ".cam", ".nis", ".cph" };

static unsigned long walk_be(const unsigned char* p, const unsigned char* end, int d)
{
    unsigned long n = 0;
    while (p + 8 <= end)
    {
        uint32_t id = port_be32(p), size = port_be32(p + 4);
        n++;
        if ((id & 0x80000000u) && d < 8) n += walk_be(p + 8, (p + 8 + size > end) ? end : p + 8 + size, d + 1);
        p += 8 + ((size + 3) & ~3u);
    }
    return n;
}
static unsigned long walk_host(const unsigned char* p, const unsigned char* end, int d)
{
    unsigned long n = 0;
    while (p + 8 <= end)
    {
        uint32_t id, size; memcpy(&id, p, 4); memcpy(&size, p + 4, 4);
        n++;
        if ((id & 0x80000000u) && d < 8) n += walk_host(p + 8, (p + 8 + size > end) ? end : p + 8 + size, d + 1);
        p += 8 + ((size + 3) & ~3u);
    }
    return n;
}
static int fails = 0, files = 0;
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
        FILE* f = fopen(p.c_str(), "rb"); std::vector<unsigned char> b(st.st_size); fread(b.data(), 1, b.size(), f); fclose(f);
        unsigned long want = walk_be(b.data(), b.data() + b.size(), 0);
        unsigned long got = port_chunk_swap_tree(b.data(), b.size());
        unsigned long after = walk_host(b.data(), b.data() + b.size(), 0);
        files++;
        if (got != want || after != want) { printf("FAIL %s want %lu got %lu after %lu\n", p.c_str(), want, got, after); fails++; }
    }
    closedir(d);
}
int main(int argc, char** argv)
{
    scan(argc > 1 ? argv[1] : ".");
    printf("%d files, %d failures\n", files, fails);
    return fails != 0;
}
