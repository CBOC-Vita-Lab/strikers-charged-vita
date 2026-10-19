#include "np_vita_internal.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

static int print(FILE* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = std::vfprintf(file, fmt, args);
    va_end(args);
    return n;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string dir = argv[1], path = dir + "/silent.log";
    std::remove(path.c_str());
    std::remove((path + ".old").c_str());
    VitaLog::Open(dir.c_str(), "silent.log");
    assert(!std::ifstream(path).good());
    { std::ofstream file(path); file << std::string(3 * 1024 * 1024, 'x'); }
    VitaLog::Open(dir.c_str(), "silent.log");
    VitaLog::SetSynchronous(true);
    VitaLog::Write("no log or console tail\n", 23);
    VitaLog::FlushIfDue();
    VitaLog::Flush();
    char lines[2][VitaLog::kTailColumns + 1] = {};
    uint32_t generation = 99;
    assert(VitaLog::Tail(lines, 2, &generation) == 0 && generation == 0);
    assert(!std::ifstream(path + ".old").good());
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file.tellg() == 3 * 1024 * 1024);
    int formatted = -1;
    std::printf("stdout%n", &formatted);
    std::fprintf(stderr, "stderr%n", &formatted);
    print(stdout, "vfprintf%n", &formatted);
    assert(formatted == -1);
    std::puts("discard");
    std::fputs("discard", stderr);
    std::putchar('x');
    std::fputc('x', stderr);
    std::putc('x', stdout);
    assert(std::fwrite("discard", 1, 7, stderr) == 7);

    // Save/settings/cache files must retain ordinary stdio behavior.
    FILE* data = std::tmpfile();
    assert(data);
    assert(std::fprintf(data, "%s:%d", "save", 42) == 7);
    assert(print(data, ":%.1f", 2.5) == 4);
    assert(std::fputs(":", data) >= 0);
    assert(std::fputc('A', data) == 'A');
    assert(std::putc('B', data) == 'B');
    assert(std::fwrite("CD", 1, 2, data) == 2);
    std::rewind(data);
    char readback[32] = {};
    assert(std::fread(readback, 1, sizeof(readback), data) == 16);
    assert(std::strcmp(readback, "save:42:2.5:ABCD") == 0);
    std::fclose(data);
}
