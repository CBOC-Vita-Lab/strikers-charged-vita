// Unit tests of the OS layer: time, interrupts, threads and their strict-priority scheduling,
// mutexes, message queues, alarms, sleeping, arenas and the OS heap.
#include <revolution/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

// ---- time ---------------------------------------------------------------------------------------
static void test_time() {
    OSTime t0 = OSGetTime();
    OSSleepTicks(OSMillisecondsToTicks(20));
    OSTime t1 = OSGetTime();
    long ms = (long)OSTicksToMilliseconds(t1 - t0);
    printf("  slept %ld ms\n", ms);
    CHECK(ms >= 19 && ms < 400);
    OSCalendarTime cal;
    OSTicksToCalendarTime(OSGetTime(), &cal);
    printf("  now: %04ld-%02ld-%02ld %02ld:%02ld:%02ld\n", cal.year, cal.month + 1, cal.mday, cal.hour,
           cal.min, cal.sec);
    CHECK(cal.year >= 2025 && cal.month >= 0 && cal.month < 12);
    OSTime back = OSCalendarTimeToTicks(&cal);
    CHECK(OSTicksToSeconds(back) == OSTicksToSeconds(OSGetTime()) ||
          OSTicksToSeconds(back) + 1 == OSTicksToSeconds(OSGetTime()));
    CHECK(OS_TIME_SPEED == 60750000ul);
}

// ---- interrupts ---------------------------------------------------------------------------------
static void test_interrupts() {
    BOOL a = OSDisableInterrupts();
    CHECK(a == TRUE);
    BOOL b = OSDisableInterrupts();
    CHECK(b == FALSE);
    OSRestoreInterrupts(b);
    BOOL c = OSRestoreInterrupts(a);   // returns the previous state: disabled
    CHECK(c == FALSE);
    BOOL d = OSDisableInterrupts();
    CHECK(d == TRUE);   // enabled again after the restore
    OSRestoreInterrupts(d);
}

// ---- threads: strict priorities -----------------------------------------------------------------
static OSThread th[4];
static u8 stk[4][4096] __attribute__((aligned(32)));
static volatile int order[16];
static volatile int nord = 0;

static void* worker(void* arg) {
    int id = (int)(intptr_t)arg;
    order[nord++] = id;
    return (void*)(intptr_t)(id * 10);
}

static void test_threads() {
    // main runs at priority 16. A higher priority thread (8) runs as soon as it is resumed; a lower
    // one (24) only when main waits.
    CHECK(OSCreateThread(&th[0], worker, (void*)1, stk[0] + sizeof(stk[0]), sizeof(stk[0]), 24, 0));
    CHECK(OSCreateThread(&th[1], worker, (void*)2, stk[1] + sizeof(stk[1]), sizeof(stk[1]), 8, 0));
    CHECK(OSCreateThread(&th[2], worker, (void*)3, stk[2] + sizeof(stk[2]), sizeof(stk[2]), 16, 0));
    nord = 0;
    OSResumeThread(&th[0]);      // lower than main: waits
    CHECK(nord == 0);
    OSResumeThread(&th[1]);      // higher: runs now, to completion
    CHECK(nord == 1 && order[0] == 2);
    OSResumeThread(&th[2]);      // same priority: waits for a yield
    CHECK(nord == 1);
    OSYieldThread();             // main yields to the equal priority thread (th[2]) only
    CHECK(nord == 2 && order[1] == 3);
    void* val = 0;
    CHECK(OSJoinThread(&th[0], &val));   // main sleeps: th[0] runs
    CHECK(nord == 3 && order[2] == 1);
    CHECK((intptr_t)val == 10);
    CHECK(OSJoinThread(&th[1], &val) && (intptr_t)val == 20);
    CHECK(OSJoinThread(&th[2], &val) && (intptr_t)val == 30);
    CHECK(OSIsThreadTerminated(&th[0]));
}

// ---- mutex + message queue between threads ------------------------------------------------------
static OSMutex mtx;
static OSMessageQueue mq;
static OSMessage mqbuf[4];
static volatile int shared = 0;

static void* producer(void*) {
    for (int i = 1; i <= 8; ++i) {
        OSLockMutex(&mtx);
        shared += i;
        OSUnlockMutex(&mtx);
        OSSendMessage(&mq, (OSMessage)(intptr_t)i, OS_MESSAGE_BLOCK);   // blocks when 4 are queued
    }
    return 0;
}

static void test_mutex_queue() {
    OSInitMutex(&mtx);
    OSInitMessageQueue(&mq, mqbuf, 4);
    CHECK(OSCreateThread(&th[0], producer, 0, stk[0] + sizeof(stk[0]), sizeof(stk[0]), 20, 0));
    OSResumeThread(&th[0]);
    int sum = 0;
    for (int i = 0; i < 8; ++i) {
        OSMessage m;
        CHECK(OSReceiveMessage(&mq, &m, OS_MESSAGE_BLOCK));
        sum += (int)(intptr_t)m;
    }
    CHECK(sum == 36);
    OSJoinThread(&th[0], 0);
    OSLockMutex(&mtx);
    CHECK(shared == 36);
    CHECK(OSTryLockMutex(&mtx));   // recursive
    OSUnlockMutex(&mtx);
    OSUnlockMutex(&mtx);
    OSMessage m;
    CHECK(!OSReceiveMessage(&mq, &m, OS_MESSAGE_NO_FLAGS));   // empty, non-blocking
}

// ---- alarms -------------------------------------------------------------------------------------
static volatile int fired = 0;
static volatile OSTick last_tick;
static void on_alarm(OSAlarm* a, OSContext*) {
    fired++;
    last_tick = OSGetTick();
    // interrupt context: the lock is held, nesting is visible as "disabled"
    BOOL e = OSDisableInterrupts();
    if (e) fired += 1000;
    OSRestoreInterrupts(e);
    (void)a;
}
static void test_alarms() {
    OSAlarm one;
    OSCreateAlarm(&one);
    OSSetAlarm(&one, OSMillisecondsToTicks(10), on_alarm);
    OSSleepTicks(OSMillisecondsToTicks(40));
    CHECK(fired == 1);

    OSAlarm per;
    OSCreateAlarm(&per);
    fired = 0;
    OSSetPeriodicAlarm(&per, OSMillisecondsToTicks(5), OSMillisecondsToTicks(5), on_alarm);
    OSSleepTicks(OSMillisecondsToTicks(100));
    OSCancelAlarm(&per);
    int n = fired;
    printf("  periodic 5 ms alarm fired %d times in 100 ms\n", n);
    CHECK(n >= 8 && n <= 21);
    OSSleepTicks(OSMillisecondsToTicks(30));
    CHECK(fired == n);   // cancelled

    // an alarm cancelled before it is due never fires
    fired = 0;
    OSSetAlarm(&one, OSMillisecondsToTicks(20), on_alarm);
    OSCancelAlarm(&one);
    OSSleepTicks(OSMillisecondsToTicks(40));
    CHECK(fired == 0);
}

// ---- arenas and the OS heap ---------------------------------------------------------------------
static void test_arenas() {
    void* lo1 = OSGetMEM1ArenaLo();
    void* hi1 = OSGetMEM1ArenaHi();
    void* lo2 = OSGetMEM2ArenaLo();
    void* hi2 = OSGetMEM2ArenaHi();
    printf("  MEM1 %p-%p MEM2 %p-%p\n", lo1, hi1, lo2, hi2);
    CHECK((char*)hi1 - (char*)lo1 == 24 * 1024 * 1024);
    CHECK((char*)hi2 - (char*)lo2 == 64 * 1024 * 1024);
    CHECK(((unsigned long)lo1 & 0x80000000ul) != 0 || ((unsigned long)lo1) != 0);
    void* a = OSAllocFromMEM2ArenaLo(0x1001, 32);
    CHECK(((unsigned long)a & 31) == 0);
    CHECK(OSGetMEM2ArenaLo() >= (char*)a + 0x1001);
    // the game's start-up sequence
    void* lo = OSGetMEM2ArenaLo();
    void* hi = OSGetMEM2ArenaHi();
    void* heapLo = OSInitAlloc(lo, hi, 1);
    OSHeapHandle h = OSCreateHeap(heapLo, hi);
    CHECK(h >= 0);
    OSSetCurrentHeap(h);
    void* p = OSAllocFromHeap(h, 1000);
    CHECK(p != 0);
    OSFreeToHeap(h, p);
}

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    OSInit();
    printf("time\n");       test_time();
    printf("interrupts\n"); test_interrupts();
    printf("threads\n");    test_threads();
    printf("mutex/queue\n"); test_mutex_queue();
    printf("alarms\n");     test_alarms();
    printf("arenas\n");     test_arenas();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_os: ok\n");
    return 0;
}
