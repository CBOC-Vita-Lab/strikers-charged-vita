// os_alarm.c: adapted from the decomp's ref/RVL_SDK/os/OSAlarm.c (queue, periodic alignment and
// cancel logic unchanged). The decrementer exception is a helper thread: it sleeps until the first
// alarm is due, then runs the handler in interrupt context (np_irq_enter .. np_irq_leave).
#include <revolution/os.h>

#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

static OSAlarmQueue AlarmQueue;
static NpSem g_timer_sem;
static NpThread* g_timer_thread;
static volatile int g_timer_started;

static int g_vic_id = -1;   // virtual time: the alarm queue is an event source (os_virtual.cpp)

// Virtual time: the next alarm's deadline as an event time (with the lock held).
static void vic_rearm(void) {
    if (g_vic_id < 0) return;
    if (AlarmQueue.head == NULL) {
        np_vic_arm(g_vic_id, 0);
    } else {
        uint64_t ns = np_os_ns_from_ticks((uint64_t)(AlarmQueue.head->end - OS_SYSTEM_TIME));
        // never an instant that has passed while the alarm is still not due (the idle scheduler would spin on it)
        const uint64_t now = np_vt_now();
        if (ns <= now && __OSGetSystemTime() < AlarmQueue.head->end) ns = now + 1000;
        np_vic_arm(g_vic_id, ns ? ns : 1);
    }
}

static void SetTimer(const OSAlarm* alarm) {
    (void)alarm;
    if (g_vic_id >= 0) {
        vic_rearm();
    } else if (g_timer_started) {
        np_sem_post(&g_timer_sem);   // the timer thread recomputes its deadline
    }
}

static void* timer_thread_main(void* arg);
static void vic_run(void* ctx, uint64_t due);

void __OSInitAlarm(void) {
    if (!g_timer_started) {
        AlarmQueue.head = AlarmQueue.tail = NULL;
        np_sem_init(&g_timer_sem, 0);
        g_timer_started = 1;
        if (np_vt_on) {
            g_vic_id = np_vic_add("alarm", vic_run, NULL);
            return;
        }
        g_timer_thread = np_thread_create("os-alarm", NP_ROLE_TIMER, timer_thread_main, NULL, NULL, 262144);
        if (g_timer_thread) {
            np_thread_detach(g_timer_thread);
        }
    }
}

void np_alarm_init(void) { __OSInitAlarm(); }

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = NULL;
    alarm->tag = 0;
    alarm->prev = alarm->next = NULL;
    alarm->period = 0;
}

static void InsertAlarm(OSAlarm* alarm, s64 end, OSAlarmHandler handler) {
    OSAlarm* iter;
    OSAlarm* prev;

    // Calculate periodic alarm end time (caller makes 'end' 0)
    if (alarm->period > 0) {
        s64 time = __OSGetSystemTime();
        end = alarm->start;

        if (alarm->start < time) {
            s64 periodLeft = ((time - alarm->start) / alarm->period);
            end += alarm->period * (periodLeft + 1);
        }
    }

    alarm->handler = handler;
    alarm->end = end;

    // Find position to insert alarm (sorted ascending by end time)
    for (iter = AlarmQueue.head; iter != NULL; iter = iter->next) {
        if (iter->end <= end) {
            continue;
        }

        alarm->prev = iter->prev;
        iter->prev = alarm;
        alarm->next = iter;

        prev = alarm->prev;
        if (prev != NULL) {
            prev->next = alarm;
        } else {
            AlarmQueue.head = alarm;
            SetTimer(alarm);
        }

        return;
    }

    alarm->next = NULL;
    prev = AlarmQueue.tail;
    AlarmQueue.tail = alarm;
    alarm->prev = prev;

    if (prev != NULL) {
        prev->next = alarm;
    } else {
        AlarmQueue.tail = alarm;
        AlarmQueue.head = alarm;
        SetTimer(alarm);
    }
}

void OSSetAlarm(OSAlarm* alarm, s64 tick, OSAlarmHandler handler) {
    BOOL enabled = OSDisableInterrupts();

    alarm->period = 0;
    InsertAlarm(alarm, __OSGetSystemTime() + tick, handler);

    OSRestoreInterrupts(enabled);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, s64 tick, s64 period,
                        OSAlarmHandler handler) {
    BOOL enabled = OSDisableInterrupts();

    alarm->period = period;
    alarm->start = __OSTimeToSystemTime(tick);
    InsertAlarm(alarm, 0, handler);

    OSRestoreInterrupts(enabled);
}

void OSCancelAlarm(OSAlarm* alarm) {
    OSAlarm* next;

    BOOL enabled = OSDisableInterrupts();

    if (alarm->handler == NULL) {
        OSRestoreInterrupts(enabled);
        return;
    }

    next = alarm->next;
    if (next == NULL) {
        AlarmQueue.tail = alarm->prev;
    } else {
        next->prev = alarm->prev;
    }

    if (alarm->prev != NULL) {
        alarm->prev->next = next;
    } else {
        AlarmQueue.head = next;
        if (next != NULL) {
            SetTimer(next);
        } else {
            vic_rearm();   // (virtual time) nothing queued: disarm
        }
    }

    alarm->handler = NULL;

    OSRestoreInterrupts(enabled);
}

// One iteration of the decrementer exception: runs the first alarm if it is due. Called with
// interrupts disabled (the lock held). Returns 1 if an alarm ran.
static int run_due_alarm(void) {
    OSAlarmHandler handler;
    OSContext tempCtx;
    OSAlarm* alarm;
    OSAlarm* next;
    s64 time = __OSGetSystemTime();

    alarm = AlarmQueue.head;
    if (alarm == NULL || time < alarm->end) {
        return 0;
    }

    next = alarm->next;
    AlarmQueue.head = next;
    if (next == NULL) {
        AlarmQueue.tail = NULL;
    } else {
        next->prev = NULL;
    }

    handler = alarm->handler;
    alarm->handler = NULL;

    if (alarm->period > 0) {
        InsertAlarm(alarm, 0, handler);
    }

    memset(&tempCtx, 0, sizeof(tempCtx));
    if (handler != NULL) {
        handler(alarm, &tempCtx);
    }
    return 1;
}

// Virtual time: the decrementer exception, inline.
static void vic_run(void* ctx, uint64_t due) {
    (void)ctx;
    (void)due;
    np_irq_enter();
    while (run_due_alarm()) {
    }
    vic_rearm();
    np_irq_leave();
}

static void* timer_thread_main(void* arg) {
    (void)arg;
    for (;;) {
        uint64_t wait_ns = 500000000ull;   // nothing queued: look again in half a second
        np_irq_enter();
        while (run_due_alarm()) {
        }
        if (AlarmQueue.head != NULL) {
            s64 left = AlarmQueue.head->end - __OSGetSystemTime();
            if (left < 1) left = 1;
            // ticks -> ns (60.75 MHz); real-time wait even when the clock is virtual
            wait_ns = (uint64_t)left * 1000000000ull / (uint64_t)OS_TIME_SPEED;
            if (wait_ns > 500000000ull) wait_ns = 500000000ull;
        }
        np_irq_leave();
        np_sem_timedwait_ns(&g_timer_sem, wait_ns);
    }
    return NULL;
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) {
    alarm->tag = tag;
}

void OSSetAlarmUserData(OSAlarm* alarm, void* userData) {
    alarm->userData = userData;
}

void* OSGetAlarmUserData(const OSAlarm* alarm) {
    return alarm->userData;
}
