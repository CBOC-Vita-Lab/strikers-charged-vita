#include <revolution/dvd.h>
#include <revolution/os.h>
#include "np/disc_error.h"
#include "np_internal.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static pthread_t gameThread;
static std::atomic<int> completed{0};
static std::atomic<int> result{0};
static int presentation = 0;
static char expectedPath[512];

#ifndef NP_DISC_TEST_NO_HOOKS
extern "C" void NpPlatformFatal(const char* category, const char* details) {
    CHECK(pthread_equal(pthread_self(), gameThread));
    CHECK(!np_in_irq() && !np_irq_disabled());
    CHECK(completed.load() == 1);
    CHECK(std::strstr(category, "Game files") != nullptr);
    CHECK(std::strstr(details, expectedPath) != nullptr);
    CHECK(std::strstr(details, "ux0:data/mscharged/game") != nullptr);
    CHECK(presentation == 0);
    presentation = 1;
}

extern "C" void NpPlatformExit(int code) {
    CHECK(pthread_equal(pthread_self(), gameThread));
    CHECK(!np_in_irq() && !np_irq_disabled());
    CHECK(presentation == 1 && code == 1);
    presentation = 2;
}
#endif

static void on_read(s32 value, DVDFileInfo*) {
    if (value == DVD_RESULT_FATAL_ERROR) {
        CHECK(np_in_irq() && np_irq_disabled());
        CHECK(NpDiscReportReadError() == 0);
        CHECK(np_in_irq() && np_irq_disabled());
        CHECK(presentation == 0);
    }
    result.store(value);
    completed.store(1);
}

static void write_file(const char* path, size_t size) {
    FILE* file = std::fopen(path, "wb");
    CHECK(file != nullptr);
    if (!file) return;
    for (size_t i = 0; i < size; ++i) std::fputc(static_cast<int>((i + 7) & 255), file);
    CHECK(std::fclose(file) == 0);
}

static int scenario(const char* mode, bool virtualTime) {
    alarm(10);
    setenv("NP_VIRTUAL_TIME", virtualTime ? "1" : "0", 1);
    setenv("MSC_DVD_RETRY", "0", 1);   // fatal-error reporting test; retry backoff is tested by test_dvd_retry.cpp
    gameThread = pthread_self();
    char root[] = "/tmp/np_disc_error_XXXXXX";
    CHECK(mkdtemp(root) != nullptr);
    char files[512];
    std::snprintf(files, sizeof files, "%s/files", root);
    CHECK(mkdir(files, 0700) == 0);
    std::snprintf(expectedPath, sizeof expectedPath, "%s/read.bin", files);
    write_file(expectedPath, 45);
    char other[512];
    std::snprintf(other, sizeof other, "%s/other.bin", files);
    write_file(other, 64);

    NpSdkConfig config;
    NpSdk_ConfigDefaults(&config);
    config.disc_dir = root;
    CHECK(NpSdk_Init(&config) == 0);
    DVDInit();
    DVDFileInfo file;
    CHECK(DVDOpen("read.bin", &file));
    char details[1200];
    CHECK(NpDiscGetReadError(details, sizeof details) == 0 && details[0] == 0);
    CHECK(NpDiscReportReadError() == 0 && presentation == 0);

    const bool missing = std::strcmp(mode, "missing") == 0;
    const bool truncated = std::strcmp(mode, "truncated") == 0;
    if (missing) CHECK(unlink(expectedPath) == 0);
    if (truncated) CHECK(truncate(expectedPath, 9) == 0);
    alignas(32) unsigned char data[64];
    std::memset(data, 0xee, sizeof data);
    CHECK(DVDReadAsyncPrio(&file, data, sizeof data, 0, on_read, 2));
    for (int i = 0; !completed.load() && i < 2000; ++i) {
        DVDGetCommandBlockStatus(&file.block);
        if (!completed.load()) usleep(1000);
    }
    CHECK(completed.load() == 1);
    CHECK(presentation == 0);
    if (!missing && !truncated) {
        CHECK(result.load() == 64);
        CHECK(DVDGetDriveStatus() == DVD_STATE_IDLE);
        CHECK(data[0] == 7 && data[44] == 51 && data[45] == 0 && data[63] == 0);
        CHECK(NpDiscGetReadError(details, sizeof details) == 0);
        CHECK(NpDiscReportReadError() == 0 && presentation == 0);
    } else {
        CHECK(result.load() == DVD_RESULT_FATAL_ERROR);
        CHECK(DVDGetCommandBlockStatus(&file.block) == DVD_STATE_FATAL);
        CHECK(DVDGetDriveStatus() == DVD_STATE_FATAL);
        CHECK(NpDiscGetReadError(details, sizeof details) == 1);
        CHECK(std::strstr(details, expectedPath) != nullptr);
        CHECK(std::strstr(details, missing ? "open" : "short read") != nullptr);
        char tiny[8];
        CHECK(NpDiscGetReadError(tiny, sizeof tiny) == 1 && tiny[sizeof tiny - 1] == 0);
        CHECK(NpDiscGetReadError(nullptr, 0) == 1);
        char first[1200];
        std::snprintf(first, sizeof first, "%s", details);
        CHECK(DVDOpen("other.bin", &file));
        CHECK(unlink(other) == 0);
        completed.store(0);
        CHECK(DVDReadAsyncPrio(&file, data, sizeof data, 0, on_read, 2));
        for (int i = 0; !completed.load() && i < 2000; ++i) {
            DVDGetCommandBlockStatus(&file.block);
            if (!completed.load()) usleep(1000);
        }
        CHECK(completed.load() == 1);
        CHECK(NpDiscGetReadError(details, sizeof details) == 1);
        CHECK(std::strcmp(first, details) == 0);
#ifdef NP_DISC_TEST_NO_HOOKS
        CHECK(NpDiscReportReadError() == 0 && presentation == 0);
#else
        CHECK(NpDiscReportReadError() == 1 && presentation == 2);
#endif
        presentation = 0;
        const BOOL outer = OSDisableInterrupts();
        const BOOL inner = OSDisableInterrupts();
        CHECK(outer && !inner && np_irq_disabled() && !np_in_irq());
#ifdef NP_DISC_TEST_NO_HOOKS
        CHECK(NpDiscReportReadError() == 0 && presentation == 0);
#else
        CHECK(NpDiscReportReadError() == 1 && presentation == 2);
#endif
        CHECK(np_irq_disabled() && !np_in_irq());
        OSRestoreInterrupts(inner);
        CHECK(np_irq_disabled());
        OSRestoreInterrupts(outer);
        CHECK(!np_irq_disabled());
    }
    unlink(expectedPath);
    unlink(other);
    rmdir(files);
    rmdir(root);
    std::printf("disc error %s (%s): %s\n", mode, virtualTime ? "virtual" : "worker", failures ? "FAILED" : "passed");
    std::fflush(stdout);
    return failures ? 1 : 0;
}

int main() {
    const char* modes[] = {"rounded-tail", "missing", "truncated"};
    for (int virtualTime = 0; virtualTime < 2; ++virtualTime) for (const char* mode : modes) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) _exit(scenario(mode, virtualTime != 0));
        if (pid < 0) continue;
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    return failures ? 1 : 0;
}
