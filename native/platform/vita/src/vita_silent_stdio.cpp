// Link-time interception also silences prebuilt game/SDK logging before formatting.
#include "np_vita_internal.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if !NP_LOGGING
namespace {
bool LogStream(FILE* stream) { return stream == stdout || stream == stderr; }
}
extern "C" {
int __real_vfprintf(FILE*, const char*, va_list);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_putc(int, FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);

int __wrap_printf(const char*, ...) { return 0; }
int __wrap_vprintf(const char*, va_list) { return 0; }
int __wrap_vfprintf(FILE* stream, const char* format, va_list args) {
    return LogStream(stream) ? 0 : __real_vfprintf(stream, format, args);
}
int __wrap_fprintf(FILE* stream, const char* format, ...) {
    if (LogStream(stream)) return 0;
    va_list args;
    va_start(args, format);
    const int result = __real_vfprintf(stream, format, args);
    va_end(args);
    return result;
}
int __wrap_puts(const char*) { return 0; }
int __wrap_putchar(int value) { return static_cast<unsigned char>(value); }
int __wrap_fputs(const char* text, FILE* stream) {
    return LogStream(stream) ? 0 : __real_fputs(text, stream);
}
int __wrap_fputc(int value, FILE* stream) {
    return LogStream(stream) ? static_cast<unsigned char>(value) : __real_fputc(value, stream);
}
int __wrap_putc(int value, FILE* stream) {
    return LogStream(stream) ? static_cast<unsigned char>(value) : __real_putc(value, stream);
}
size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* stream) {
    return LogStream(stream) ? (size ? count : 0) : __real_fwrite(data, size, count, stream);
}
void __wrap_np_log(const char*, const char*, ...) {}
void __wrap_OSReport(const char*, ...) {}
void __wrap_OSVReport(const char*, va_list) {}
}
#elif defined(__vita__) || defined(NP_LOGSTDIO_HOST_TEST)
// crash26b (logging builds; vita.mk wraps the same names): the log streams' calls never touch stdout / stderr's FILE, which newlib does not lock
// (np_log_stdio.h). MSC_STDIO_LOCK=0 (VitaLog::StdioLockOn) sends them to the real functions as in 1.2.
#include "np_log_stdio.h"
namespace {
bool LogStream(FILE* stream) { return (stream == stdout || stream == stderr) && VitaLog::StdioLockOn() && NpLogStdio::G().sink; }   // (before InstallStdioHooks: the FILE)
}
extern "C" {
int __real_vfprintf(FILE*, const char*, va_list);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_putc(int, FILE*);
int __real_puts(const char*);
int __real_putchar(int);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);

int __wrap_vfprintf(FILE* stream, const char* format, va_list args) {
    return LogStream(stream) ? NpLogStdio::VPrint(format, args) : __real_vfprintf(stream, format, args);
}
int __wrap_vprintf(const char* format, va_list args) { return __wrap_vfprintf(stdout, format, args); }
int __wrap_fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = __wrap_vfprintf(stream, format, args);
    va_end(args);
    return result;
}
int __wrap_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = __wrap_vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
int __wrap_fputs(const char* text, FILE* stream) {
    if (!LogStream(stream)) return __real_fputs(text, stream);
    return text ? NpLogStdio::Put(text, std::strlen(text)) : 0;
}
int __wrap_puts(const char* text) {
    if (!LogStream(stdout)) return __real_puts(text);
    return NpLogStdio::Print("%s\n", text ? text : "(null)");   // (one text with its newline)
}
int __wrap_fputc(int value, FILE* stream) {
    if (!LogStream(stream)) return __real_fputc(value, stream);
    const char c = static_cast<char>(value);
    NpLogStdio::Put(&c, 1);
    return static_cast<unsigned char>(value);
}
int __wrap_putc(int value, FILE* stream) {
    if (!LogStream(stream)) return __real_putc(value, stream);
    return __wrap_fputc(value, stream);
}
int __wrap_putchar(int value) {
    if (!LogStream(stdout)) return __real_putchar(value);
    return __wrap_fputc(value, stdout);
}
size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* stream) {
    if (!LogStream(stream)) return __real_fwrite(data, size, count, stream);
    if (!data || !size || !count) return 0;
    NpLogStdio::Put(static_cast<const char*>(data), size * count);
    return count;
}
}
#endif
