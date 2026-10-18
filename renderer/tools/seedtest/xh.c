#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        FILE* f = fopen(argv[i], "rb"); if (!f) continue;
        static char buf[1 << 20]; const char* pre = "#version 100\n"; size_t n = strlen(pre); memcpy(buf, pre, n);
        n += fread(buf + n, 1, sizeof buf - n, f); fclose(f);
        unsigned long long h = XXH3_64bits(buf, n);
        printf("%02X/%llX %s\n", (unsigned)(h >> 56), h, argv[i]);
    }
    return 0;
}
