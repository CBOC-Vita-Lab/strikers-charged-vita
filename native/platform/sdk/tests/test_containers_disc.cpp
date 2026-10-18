// ARC and TPL on the real files of the disc dump: every Art/fe/*.tpl (the save icons and region banners
// the game binds with TPLBind) and the two U8 archives (RFLRes01.arc, HomeButton2/homeBtn_GER.arc) are
// converted by the SDK layer and walked: every header field in range, every file inside its archive.
// Skipped when the disc dump is not present.
#include <revolution/arc.h>
#include <revolution/os.h>
#include <revolution/tpl.h>

#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static unsigned char* load(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* p = (unsigned char*)memalign(32, (*size + 31) & ~31u);
    size_t n = fread(p, 1, *size, f);
    (void)n;
    fclose(f);
    return p;
}

static int g_files = 0, g_dirs = 0;
static void walk(ARCHandle* h, const char* path, unsigned char* base, size_t size, int depth) {
    ARCDir dir;
    ARCDirEntry e;
    if (!ARCOpenDir(h, path, &dir)) {
        CHECK(!"ARCOpenDir");
        return;
    }
    ++g_dirs;
    while (ARCReadDir(&dir, &e)) {
        char sub[256];
        snprintf(sub, sizeof(sub), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e.name);
        if (e.isDir) {
            if (depth < 8) walk(h, sub, base, size, depth + 1);
        } else {
            ARCFileInfo f;
            CHECK(ARCOpen(h, sub, &f));
            unsigned char* start = (unsigned char*)ARCGetStartAddrInMem(&f);
            CHECK(start >= base && start + ARCGetLength(&f) <= base + size);
            ARCClose(&f);
            ++g_files;
        }
    }
    ARCCloseDir(&dir);
}

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    OSInit();
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir) dir = "/home/claude/msc/game/files";
    char path[512];
    int ran = 0;

    snprintf(path, sizeof(path), "%s/Art/fe", dir);
    DIR* d = opendir(path);
    if (d) {
        while (struct dirent* de = readdir(d)) {
            size_t n = strlen(de->d_name);
            if (n < 5 || strcmp(de->d_name + n - 4, ".tpl")) continue;
            char fp[600];
            snprintf(fp, sizeof(fp), "%s/%s", path, de->d_name);
            size_t size;
            unsigned char* buf = load(fp, &size);
            if (!buf) continue;
            TPLPalette* pal = (TPLPalette*)buf;
            TPLBind(pal);
            CHECK(pal->numDescriptors >= 1 && pal->numDescriptors < 64);
            for (unsigned i = 0; i < pal->numDescriptors; ++i) {
                TPLDescriptor* t = TPLGet(pal, i);
                CHECK(t->textureHeader != 0);
                if (!t->textureHeader) continue;
                TPLHeader* h = t->textureHeader;
                CHECK(h->width >= 1 && h->width <= 1024 && h->height >= 1 && h->height <= 1024);
                CHECK(h->format <= 14);
                CHECK((unsigned char*)h->data >= buf && (unsigned char*)h->data < buf + size);
                CHECK(h->unpacked == 1);
                if (t->CLUTHeader) CHECK((unsigned char*)t->CLUTHeader->data < buf + size && t->CLUTHeader->numEntries <= 16384);
                if (i == 0) printf("  %s: %u texture(s), first %ux%u format %u\n", de->d_name, (unsigned)pal->numDescriptors, h->width, h->height, (unsigned)h->format);
            }
            free(buf);
            ++ran;
        }
        closedir(d);
    }
    const char* arcs[] = {"RFLRes01.arc", "HomeButton2/homeBtn_GER.arc"};
    for (const char* a : arcs) {
        snprintf(path, sizeof(path), "%s/%s", dir, a);
        size_t size;
        unsigned char* buf = load(path, &size);
        if (!buf) continue;
        ARCHandle h;
        CHECK(ARCInitHandle(buf, &h));
        g_files = g_dirs = 0;
        walk(&h, "/", buf, size, 0);
        printf("  %s: %u entries, %d directories, %d files, all inside the archive\n", a, (unsigned)h.entryNum, g_dirs, g_files);
        CHECK(g_files > 0 && g_files + g_dirs <= (int)h.entryNum);
        free(buf);
        ++ran;
    }
    if (ran == 0) {
        printf("  (disc files not present: skipped)\n");
    }
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_containers_disc: ok (%d file(s))\n", ran);
    return 0;
}
