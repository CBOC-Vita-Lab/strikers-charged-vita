// POSIX stand-ins for the sceIo calls the seed copy uses (test only)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#undef st_mtime
typedef int SceUID; typedef unsigned SceSize;
struct SceDateTime { int year, month, day, hour, minute, second; };
struct SceIoStat { int st_mode; long long st_size; SceDateTime st_mtime; };
struct SceIoDirent { SceIoStat d_stat; char d_name[256]; };
#define SCE_O_RDONLY O_RDONLY
#define SCE_O_WRONLY O_WRONLY
#define SCE_O_CREAT O_CREAT
#define SCE_O_TRUNC O_TRUNC
#define SCE_S_ISDIR(m) S_ISDIR(m)
static std::string g_root; static unsigned g_ops[8]; // open, mkdir ok, mkdir fail, stat, read, write, dopen, remove
static std::string Map(const char* p) { std::string s(p); size_t c = s.find(':'); return g_root + "/" + s.substr(0, c) + "/" + s.substr(c + 1); }
static int sceIoOpen(const char* p, int fl, int mode) { ++g_ops[0]; int f = open(Map(p).c_str(), fl, mode ? mode : 0666); return f < 0 ? -1 : f; }
static int sceIoClose(int f) { return f < 0 ? -1 : close(f); }
static int sceIoRead(int f, void* b, unsigned n) { ++g_ops[4]; return f < 0 ? -1 : (int)read(f, b, n); }
static int sceIoWrite(int f, const void* b, unsigned n) { ++g_ops[5]; return f < 0 ? -1 : (int)write(f, b, n); }
static int sceIoMkdir(const char* p, int) { int r = mkdir(Map(p).c_str(), 0777); ++g_ops[r == 0 ? 1 : 2]; return r == 0 ? 0 : -1; }
static int sceIoRemove(const char* p) { ++g_ops[7]; return unlink(Map(p).c_str()); }
static int sceIoGetstat(const char* p, SceIoStat* st) { ++g_ops[3]; struct stat s; if (stat(Map(p).c_str(), &s) != 0) return -1; st->st_mode = s.st_mode; st->st_size = s.st_size; struct tm t; time_t mt = s.st_mtim.tv_sec; gmtime_r(&mt, &t); st->st_mtime = {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec}; return 0; }
static std::vector<DIR*> g_dirs; static std::vector<std::string> g_dirPaths;
static int sceIoDopen(const char* p) { ++g_ops[6]; DIR* d = opendir(Map(p).c_str()); if (!d) return -1; g_dirs.push_back(d); g_dirPaths.push_back(Map(p)); return (int)g_dirs.size() - 1; }
static int sceIoDread(int h, SceIoDirent* e) { dirent* de = readdir(g_dirs[h]); if (!de) return 0; std::strncpy(e->d_name, de->d_name, 255); struct stat s; stat((g_dirPaths[h] + "/" + de->d_name).c_str(), &s); e->d_stat.st_mode = s.st_mode; return 1; }
static int sceIoDclose(int h) { closedir(g_dirs[h]); g_dirs[h] = nullptr; return 0; }
static uint64_t NowUs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000ull + t.tv_nsec / 1000; }
static bool Exists(const char* path) { SceIoStat st; return sceIoGetstat(path, &st) >= 0; }
typedef unsigned GLuint; typedef int GLsizei; typedef char GLchar; typedef int GLint; typedef unsigned GLenum;
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
static GLuint g_next = 1; static GLuint glCreateShader(GLenum) { return g_next++; }
static void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
extern "C" { char vgl_shader_cache_path[256]; }
