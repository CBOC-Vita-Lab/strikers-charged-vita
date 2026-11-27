// Syscall wrappers for the cut tests (test_safe_file.cpp, test_nand_save_cut.cpp). Link with -Wl,--wrap=open,write,fsync,rename,unlink,close.
// A child sets g_cutAt = k and dies (Die) before the k-th file-changing call; with g_power every byte written and not fsync'ed since is first replaced by
// zeros (the file keeps its length): the power-cut model in which a rename survives without its data. rename follows newlib's on the Vita: a target that
// exists is removed first, as a second counted step (vitasdk libc.a _rename_r calls sceIoRemove(new) and then sceIoRename(old, new)).
#pragma once
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

extern "C" {
int __real_open(const char*, int, ...);
ssize_t __real_write(int, const void*, size_t);
int __real_fsync(int);
int __real_rename(const char*, const char*);
int __real_unlink(const char*);
int __real_close(int);
}

namespace {

int g_cutAt = -1;        // the call to die before; -1 none
int g_calls = 0;
bool g_power = false;
struct Pending { int fd; off_t off; size_t len; };
std::vector<Pending> g_pending;

[[noreturn]] void Die(int code = 77) {
    if (g_power) {
        for (const Pending& p : g_pending) {
            std::string zeros(p.len, '\0');
            if (pwrite(p.fd, zeros.data(), zeros.size(), p.off) < 0) {}
        }
    }
    _exit(code);
}
void Point() { if (g_cutAt >= 0 && g_calls++ == g_cutAt) Die(); }

}  // namespace

extern "C" {
int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    if (flags & (O_CREAT | O_TRUNC | O_WRONLY | O_RDWR)) Point();
    return __real_open(path, flags, mode);
}
ssize_t __wrap_write(int fd, const void* buf, size_t n) {
    Point();
    const off_t off = lseek(fd, 0, SEEK_CUR);
    const ssize_t w = __real_write(fd, buf, n);
    if (w > 0 && g_power) g_pending.push_back({fd, off, size_t(w)});
    return w;
}
int __wrap_fsync(int fd) {
    Point();
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [fd](const Pending& p) { return p.fd == fd; }), g_pending.end());
    return __real_fsync(fd);
}
int __wrap_rename(const char* from, const char* to) {
    Point();
    if (access(to, F_OK) == 0) { __real_unlink(to); Point(); }   // newlib on the Vita: the target is removed first
    return __real_rename(from, to);
}
int __wrap_unlink(const char* path) { Point(); return __real_unlink(path); }
int __wrap_close(int fd) {
    if (g_power) for (const Pending& p : g_pending) if (p.fd == fd) return 0;   // kept open: the unsynced data is still pending
    return __real_close(fd);
}
}

