// os_thread.c: adapted from the decomp's ref/RVL_SDK/os/OSThread.c (the SDK's own scheduler:
// run queues per priority, priority inheritance through mutexes, join/detach/suspend semantics).
//
// What changed: the PowerPC context switch. Every OSThread owns a host thread; exactly one of them
// (the one OS_CURRENT_THREAD names) runs at a time, the others are parked on their own semaphore.
// "Switching" is a semaphore hand-off (np_switch_to). Code running on helper threads in interrupt
// context (np_irq_enter..np_irq_leave) never switches: it only marks threads ready; the running
// thread notices at the next point where it re-enables interrupts (np_os_preempt_check) and an idle
// scheduler (nothing ready) is woken through g_idle_sem. This keeps the console's behaviour: strict
// priorities, no preemption between threads except at interrupt time, deterministic order.
#include <revolution/os.h>

#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

static void DefaultSwitchThreadCallback(OSThread* currThread,
                                        OSThread* newThread);

static OSSwitchThreadCallback SwitchThreadCallback =
    DefaultSwitchThreadCallback;

static OSThread DefaultThread;
static OSThreadQueue RunQueue[32];

typedef struct NpOsInfo {
    NpSem sem;          // posted to give this thread the CPU
    NpThread* host;
    volatile int cancelled;
    volatile int exiting;
    OSThread* os;
    void* arg;
    void* (*func)(void*);
} NpOsInfo;

// The OSContext of a thread is not used for registers here: gprs[0] holds its NpOsInfo.
#define INFO_OF(t) ((NpOsInfo*)(uintptr_t)((t)->context.gprs[0]))

static __thread OSThread* tl_self_os = NULL;   // the OSThread this host thread is (NULL: helper)

// 1 on a host thread that is an OSThread (the game's main thread or one it created), 0 on helpers and foreign threads.
int np_on_os_thread(void) { return tl_self_os != NULL; }
static NpSem g_idle_sem;
static int g_idle_waiting = 0;

volatile static s32 Reschedule = 0;
volatile static BOOL RunQueueHint = FALSE;
volatile static u32 RunQueueBits = 0;

static void DefaultSwitchThreadCallback(OSThread* currThread,
                                        OSThread* newThread){
#pragma unused(currThread)
#pragma unused(newThread)
}

OSSwitchThreadCallback OSSetSwitchThreadCallback(OSSwitchThreadCallback newCb) {
    OSSwitchThreadCallback oldCb;
    BOOL enabled = OSDisableInterrupts();

    oldCb = SwitchThreadCallback;
    SwitchThreadCallback =
        (newCb != NULL) ? newCb : DefaultSwitchThreadCallback;

    OSRestoreInterrupts(enabled);
    return (oldCb == DefaultSwitchThreadCallback) ? NULL : oldCb;
}

void __OSThreadInit(void) {
    int i;
    OSThread* thread = &DefaultThread;
    OSThread* tail;
    NpOsInfo* info = (NpOsInfo*)calloc(1, sizeof(NpOsInfo));
    void* low;
    void* high;

    np_sem_init(&info->sem, 0);
    np_sem_init(&g_idle_sem, 0);
    info->os = thread;
    thread->context.gprs[0] = (u32)(uintptr_t)info;
    tl_self_os = thread;

    thread->state = OS_THREAD_STATE_RUNNING;
    thread->flags = OS_THREAD_DETACHED;
    thread->base = 16;
    thread->priority = 16;
    thread->suspend = 0;
    thread->val = 0xFFFFFFFF;
    thread->mutex = NULL;

    OSInitThreadQueue(&thread->joinQueue);
    OSInitMutexQueue(&thread->mutexQueue);

    OS_CURRENT_FPU_CONTEXT = &thread->context;
    OS_CURRENT_CONTEXT = &thread->context;

    np_thread_stack_bounds(&low, &high);
    thread->stackBegin = (u32*)high;
    thread->stackEnd = (u32*)low;

    OSSetCurrentThread(thread);

    RunQueueBits = 0;
    RunQueueHint = FALSE;
    for (i = 0; i < 32; i++) {
        OSInitThreadQueue(&RunQueue[i]);
    }

    OSInitThreadQueue(&OS_THREAD_QUEUE);

    tail = OS_THREAD_QUEUE.tail;
    if (tail == NULL) {
        OS_THREAD_QUEUE.head = thread;
    } else {
        tail->nextActive = thread;
    }

    thread->prevActive = tail;
    thread->nextActive = NULL;
    OS_THREAD_QUEUE.tail = thread;

    Reschedule = 0;
}

void np_os_threads_init(void) { __OSThreadInit(); }

void OSSetCurrentThread(OSThread* thread) {
    SwitchThreadCallback(OSGetCurrentThread(), thread);
    OS_CURRENT_THREAD = thread;
}

void OSInitMutexQueue(OSMutexQueue* queue) {
    queue->tail = NULL;
    queue->head = NULL;
}

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->tail = NULL;
    queue->head = NULL;
}

OSThread* OSGetCurrentThread(void) {
    return OS_CURRENT_THREAD;
}

static void np_thread_exit_now(void) {
    int saved;
    np_irq_release_for_wait(&saved);
    np_thread_exit();
}

// Gives the CPU to `next` (already removed from its run queue, state RUNNING) and parks the calling
// host thread until it is given the CPU back. Called with interrupts disabled.
static void __OSSwitchThread(OSThread* next) {
    OSThread* self = tl_self_os;
    OSSetCurrentThread(next);
    if (next == self) {
        return;
    }
    np_sem_post(&INFO_OF(next)->sem);
    if (self != NULL) {
        NpOsInfo* si = INFO_OF(self);
        if (si->exiting) {
            return;  // the thread is ending: the caller never comes back
        }
        int saved;
        np_irq_release_for_wait(&saved);
        np_sem_wait(&si->sem);
        np_irq_reacquire_after_wait(saved);
        if (si->cancelled) {
            np_thread_exit_now();
        }
    }
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return thread->state == OS_THREAD_STATE_MORIBUND ||
                   thread->state == OS_THREAD_STATE_EXITED
               ? TRUE
               : FALSE;
}

static BOOL __OSIsThreadActive(OSThread* thread) {
    OSThread* iter;

    if (thread->state == OS_THREAD_STATE_EXITED) {
        return FALSE;
    }

    for (iter = OS_THREAD_QUEUE.head; iter != NULL; iter = iter->nextActive) {
        if (thread == iter) {
            return TRUE;
        }
    }

    return FALSE;
}

s32 OSDisableScheduler(void) {
    s32 old;
    BOOL enabled = OSDisableInterrupts();

    old = Reschedule++;

    OSRestoreInterrupts(enabled);

    return old;
}

s32 OSEnableScheduler(void) {
    s32 old;
    BOOL enabled = OSDisableInterrupts();

    old = Reschedule--;

    OSRestoreInterrupts(enabled);

    return old;
}

void OSClearStack(u8 val) { (void)val; }

static void SetRun(OSThread* thread) {
    OSThreadQueue* queue;
    OSThread* tail;

    queue = &RunQueue[thread->priority];
    thread->queue = queue;
    tail = thread->queue->tail;

    if (tail == NULL) {
        thread->queue->head = thread;
    } else {
        tail->next = thread;
    }

    thread->prev = tail;
    thread->next = NULL;
    thread->queue->tail = thread;

    RunQueueBits |= (1 << OS_PRIORITY_MAX - thread->priority);
    RunQueueHint = TRUE;
    if (g_idle_waiting) {
        np_sem_post(&g_idle_sem);
    }
}

static void UnsetRun(OSThread* thread) DECOMP_DONT_INLINE {
    OSThreadQueue* queue;
    OSThread* next;
    OSThread* prev;

    next = thread->next;
    queue = thread->queue;
    prev = thread->prev;

    if (next == NULL) {
        queue->tail = prev;
    } else {
        next->prev = prev;
    }

    if (prev == NULL) {
        queue->head = next;
    } else {
        prev->next = next;
    }

    if (queue->head == NULL) {
        RunQueueBits &= ~(1 << OS_PRIORITY_MAX - thread->priority);
    }

    thread->queue = NULL;
}

s32 __OSGetEffectivePriority(OSThread* thread) {
    s32 prio = thread->base;

    OSMutex* mutex;
    for (mutex = thread->mutexQueue.head; mutex != NULL;
         mutex = mutex->link.next) {
        OSThread* mutexThread = mutex->queue.head;
        if (mutexThread != NULL && mutexThread->priority < prio) {
            prio = mutexThread->priority;
        }
    }

    return prio;
}

static OSThread* SetEffectivePriority(OSThread* thread, s32 prio) {
    OSThread* iter;
    OSThread* iterPrev;
    OSThread* next;
    OSThread* prev;

    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        UnsetRun(thread);
        thread->priority = prio;
        thread->queue = &RunQueue[prio];
        SetRun(thread);
        break;
    case OS_THREAD_STATE_SLEEPING:
        next = thread->next;
        prev = thread->prev;
        if (next == NULL) {
            thread->queue->tail = prev;
        } else {
            next->prev = prev;
        }

        if (prev == NULL) {
            thread->queue->head = next;
        } else {
            prev->next = next;
        }

        thread->priority = prio;

        for (iter = thread->queue->head;
             iter != NULL && iter->priority <= thread->priority;
             iter = iter->next) {
        }

        if (iter == NULL) {
            iter = thread->queue->tail;
            if (iter == NULL) {
                thread->queue->head = thread;
            } else {
                iter->next = thread;
            }
            thread->prev = iter;
            thread->next = NULL;
            thread->queue->tail = thread;
        } else {
            thread->next = iter;
            iterPrev = iter->prev;
            iter->prev = thread;
            thread->prev = iterPrev;

            if (iterPrev == NULL) {
                thread->queue->head = thread;
            } else {
                iterPrev->next = thread;
            }
        }

        if (thread->mutex != NULL) {
            return thread->mutex->thread;
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        thread->priority = prio;
        break;
    default:
        break;
    }

    return NULL;
}

static void UpdatePriority(OSThread* thread) {
    s32 prio = 0;

    while (TRUE) {
        if (thread->suspend > 0) {
            return;
        }

        prio = __OSGetEffectivePriority(thread);
        if (thread->priority == prio) {
            return;
        }

        thread = SetEffectivePriority(thread, prio);
        if (thread == NULL) {
            return;
        }
    }
}

void __OSPromoteThread(OSThread* thread, s32 prio) {
    while (TRUE) {
        if (thread->suspend > 0) {
            return;
        }

        if (thread->priority <= prio) {
            return;
        }

        thread = SetEffectivePriority(thread, prio);
        if (thread == NULL) {
            return;
        }
    }
}

static OSThread* SelectThread(BOOL b) {
    OSThreadQueue* queue;
    OSThread* currThread;
    OSThread* head;
    OSThread* next;
    s32 prio;
    OSThread* self = tl_self_os;

    if (Reschedule > 0) {
        return NULL;
    }

    // Interrupt context (helper threads) never switches; the running thread reschedules itself.
    if (self == NULL || np_in_irq()) {
        return NULL;
    }

    currThread = OSGetCurrentThread();
    if (currThread != self) {
        return NULL;
    }

    if (currThread->state == OS_THREAD_STATE_RUNNING) {
        if (!b) {
            prio = __cntlzw(RunQueueBits);
            if (currThread->priority <= prio) {
                return NULL;
            }
        }

        currThread->state = OS_THREAD_STATE_READY;
        SetRun(currThread);
    }

    if (RunQueueBits == 0) {
        // Nothing is ready: the scheduler idles (interrupts enabled) until a helper thread makes
        // something ready. The caller is not runnable itself (it sleeps, suspended or ended).
        OSSetCurrentThread(NULL);
        do {
            int saved;
            int ran_event = 0;
            if (np_vt_on) {
                // Virtual time: with everything blocked the clock warps to the next event, which runs here (np_vic_idle).
                // g_idle_waiting stays 0 meanwhile: the event's own SetRun must not leave a stale post on the idle semaphore.
                np_irq_release_for_wait(&saved);
                ran_event = np_vic_idle();
                if (!ran_event) g_idle_waiting = 1;   // nothing armed: wait for real, a helper thread may wake us
            } else {
                // g_idle_waiting is set while the lock is still held: a SetRun after the release must see it and post
                g_idle_waiting = 1;
                np_irq_release_for_wait(&saved);
            }
            if (!ran_event) {
                np_sem_wait(&g_idle_sem);
                g_idle_waiting = 0;
            }
            np_irq_reacquire_after_wait(saved);
        } while (RunQueueBits == 0);
    }

    RunQueueHint = FALSE;

    prio = __cntlzw(RunQueueBits);
    queue = &RunQueue[prio];
    head = queue->head;
    next = head->next;

    if (next == NULL) {
        queue->tail = NULL;
    } else {
        next->prev = NULL;
    }

    queue->head = next;
    if (next == NULL) {
        RunQueueBits &= ~(1 << OS_PRIORITY_MAX - prio);
    }

    head->queue = NULL;
    head->state = OS_THREAD_STATE_RUNNING;
    __OSSwitchThread(head);
    return head;
}

void np_os_preempt_check(void) {
    static __thread int busy = 0;
    if (!RunQueueHint || busy || tl_self_os == NULL || OS_CURRENT_THREAD != tl_self_os) {
        return;
    }
    busy = 1;
    {
        BOOL enabled = OSDisableInterrupts();
        __OSReschedule();
        OSRestoreInterrupts(enabled);
    }
    busy = 0;
}

void __OSReschedule(void) {
    if (RunQueueHint) {
        SelectThread(FALSE);
    }
}

void OSYieldThread(void) {
    BOOL enabled = OSDisableInterrupts();
    SelectThread(TRUE);
    OSRestoreInterrupts(enabled);
    np_vt_poll_api();   // virtual time: a yield costs a little time, then what fell due runs (a loop that yields waits for an event)
    // The game polls in loops of OSYieldThread (disc reads, retrace waits): give the host's worker
    // threads (disc, audio, timers) the processor instead of spinning at full speed.
    np_yield();
}

static void* thread_trampoline(void* p) {
    NpOsInfo* info = (NpOsInfo*)p;
    OSThread* thread = info->os;
    void* ret;
    tl_self_os = thread;
    np_sem_wait(&info->sem);        // parked until the scheduler gives this thread the CPU
    if (info->cancelled) {
        np_thread_exit();
    }
    ret = info->func(info->arg);
    OSExitThread((OSThread*)ret);   // never returns
    return NULL;
}

BOOL OSCreateThread(OSThread* thread, OSThreadFunc func, void* funcArg,
                    void* stackBegin, u32 stackSize, s32 prio, u16 flags) {
    BOOL enabled;
    int i;
    OSThread* tail;
    NpOsInfo* info;

    if (prio < OS_PRIORITY_MIN || prio > OS_PRIORITY_MAX) {
        return FALSE;
    }

    thread->state = OS_THREAD_STATE_READY;
    thread->flags = flags & OS_THREAD_DETACHED;
    thread->base = prio;
    thread->priority = prio;
    thread->suspend = 1;
    thread->val = 0xFFFFFFFF;
    thread->mutex = NULL;
    OSInitThreadQueue(&thread->joinQueue);
    OSInitMutexQueue(&thread->mutexQueue);

    info = (NpOsInfo*)calloc(1, sizeof(NpOsInfo));
    if (info == NULL) {
        return FALSE;
    }
    np_sem_init(&info->sem, 0);
    info->os = thread;
    info->func = (void* (*)(void*))func;
    info->arg = funcArg;
    memset(&thread->context, 0, sizeof(thread->context));
    thread->context.gprs[0] = (u32)(uintptr_t)info;

    thread->stackBegin = (u32*)stackBegin;
    thread->stackEnd = (u32*)((char*)stackBegin - stackSize);

    thread->error = 0;
    for (i = 0; i < 2; i++) {
        thread->specific[i] = NULL;
    }

    // The host thread gets its own stack (the game's buffer is far smaller than what the host
    // C library needs); the OSThread keeps the game's bounds for code that inspects them.
    info->host = np_thread_create("os-thread", NP_ROLE_OSTHREAD, thread_trampoline, info, NULL,
                                  stackSize < 262144 ? 262144 : stackSize);
    if (info->host == NULL) {
        np_sem_destroy(&info->sem);
        free(info);
        return FALSE;
    }
    np_thread_detach(info->host);

    enabled = OSDisableInterrupts();

    tail = OS_THREAD_QUEUE.tail;
    if (tail == NULL) {
        OS_THREAD_QUEUE.head = thread;
    } else {
        tail->nextActive = thread;
    }

    thread->prevActive = tail;
    thread->nextActive = NULL;
    OS_THREAD_QUEUE.tail = thread;
    tail = thread;

    OSRestoreInterrupts(enabled);
    return TRUE;
}

void OSExitThread(OSThread* thread) {
    BOOL enabled;
    OSThread* currThread;
    OSThread* next;
    OSThread* prev;

    enabled = OSDisableInterrupts();
    currThread = OSGetCurrentThread();
    INFO_OF(currThread)->exiting = 1;

    if ((currThread->flags & OS_THREAD_DETACHED)) {
        next = currThread->nextActive;
        prev = currThread->prevActive;

        if (next == NULL) {
            OS_THREAD_QUEUE.tail = prev;
        } else {
            next->prevActive = prev;
        }

        if (prev == NULL) {
            OS_THREAD_QUEUE.head = next;
        } else {
            prev->nextActive = next;
        }

        currThread->state = OS_THREAD_STATE_EXITED;
    } else {
        currThread->state = OS_THREAD_STATE_MORIBUND;
        currThread->val = (u32)thread;
    }

    __OSUnlockAllMutex(currThread);
    OSWakeupThread(&currThread->joinQueue);
    RunQueueHint = TRUE;
    __OSReschedule();
    // The CPU now belongs to another thread (or to the idle scheduler): this host thread ends.
    (void)enabled;
    np_thread_exit_now();
}

void OSCancelThread(OSThread* thread) {
    BOOL enabled;
    OSThread* next;
    OSThread* prev;

    enabled = OSDisableInterrupts();

    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (thread->suspend <= 0) {
            UnsetRun(thread);
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = 1;
        break;
    case OS_THREAD_STATE_SLEEPING:
        next = thread->next;
        prev = thread->prev;
        if (next == NULL) {
            thread->queue->tail = prev;
        } else {
            next->prev = prev;
        }

        if (prev == NULL) {
            thread->queue->head = next;
        } else {
            prev->next = next;
        }

        thread->queue = NULL;
        if (thread->suspend <= 0 && thread->mutex != NULL) {
            UpdatePriority(thread->mutex->thread);
        }
        break;
    default:
        OSRestoreInterrupts(enabled);
        return;
        break;
    }

    if (thread != OSGetCurrentThread()) {
        INFO_OF(thread)->cancelled = 1;      // the parked host thread ends when it is woken
        np_sem_post(&INFO_OF(thread)->sem);
    }
    if (thread->flags & OS_THREAD_DETACHED) {
        next = thread->nextActive;
        prev = thread->prevActive;

        if (next == NULL) {
            OS_THREAD_QUEUE.tail = prev;
        } else {
            next->prevActive = prev;
        }

        if (prev == NULL) {
            OS_THREAD_QUEUE.head = next;
        } else {
            prev->nextActive = next;
        }

        thread->state = OS_THREAD_STATE_EXITED;
    } else {
        thread->state = OS_THREAD_STATE_MORIBUND;
    }

    __OSUnlockAllMutex(thread);
    OSWakeupThread(&thread->joinQueue);
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSJoinThread(OSThread* thread, void* val) {
    BOOL enabled;
    OSThread* next;
    OSThread* prev;

    enabled = OSDisableInterrupts();

    if (!(thread->flags & OS_THREAD_DETACHED) &&
        thread->state != OS_THREAD_STATE_MORIBUND &&
        thread->joinQueue.head == NULL) {
        OSSleepThread(&thread->joinQueue);
        if (!__OSIsThreadActive(thread)) {
            OSRestoreInterrupts(enabled);
            return FALSE;
        }
    }

    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        if (val != NULL) {
            *(u32*)val = thread->val;
        }

        next = thread->nextActive;
        prev = thread->prevActive;

        if (next == NULL) {
            OS_THREAD_QUEUE.tail = prev;
        } else {
            next->prevActive = prev;
        }

        if (prev == NULL) {
            OS_THREAD_QUEUE.head = next;
        } else {
            prev->nextActive = next;
        }

        thread->state = OS_THREAD_STATE_EXITED;

        OSRestoreInterrupts(enabled);
        return TRUE;
    }

    OSRestoreInterrupts(enabled);
    return FALSE;
}

void OSDetachThread(OSThread* thread) {
    BOOL enabled;
    OSThread* next;
    OSThread* prev;

    enabled = OSDisableInterrupts();
    thread->flags |= OS_THREAD_DETACHED;

    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        next = thread->nextActive;
        prev = thread->prevActive;

        if (next == NULL) {
            OS_THREAD_QUEUE.tail = prev;
        } else {
            next->prevActive = prev;
        }

        if (prev == NULL) {
            OS_THREAD_QUEUE.head = next;
        } else {
            prev->nextActive = next;
        }

        thread->state = OS_THREAD_STATE_EXITED;
    }

    OSWakeupThread(&thread->joinQueue);
    OSRestoreInterrupts(enabled);
}

s32 OSResumeThread(OSThread* thread) {
    BOOL enabled;
    s32 suspend;
    OSThread* iter;
    OSThread* iterPrev;
    OSThread* next;
    OSThread* prev;

    enabled = OSDisableInterrupts();
    suspend = thread->suspend--;

    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_READY:
            thread->priority = __OSGetEffectivePriority(thread);
            SetRun(thread);
            break;
        case OS_THREAD_STATE_SLEEPING:
            next = thread->next;
            prev = thread->prev;
            if (next == NULL) {
                thread->queue->tail = prev;
            } else {
                next->prev = prev;
            }

            if (prev == NULL) {
                thread->queue->head = next;
            } else {
                prev->next = next;
            }

            thread->priority = __OSGetEffectivePriority(thread);

            for (iter = thread->queue->head;
                 iter != NULL && iter->priority <= thread->priority;
                 iter = iter->next) {
            }

            if (iter == NULL) {
                iter = thread->queue->tail;
                if (iter == NULL) {
                    thread->queue->head = thread;
                } else {
                    iter->next = thread;
                }
                thread->prev = iter;
                thread->next = NULL;
                thread->queue->tail = thread;
            } else {
                thread->next = iter;
                iterPrev = iter->prev;
                iter->prev = thread;
                thread->prev = iterPrev;

                if (iterPrev == NULL) {
                    thread->queue->head = thread;
                } else {
                    iterPrev->next = thread;
                }
            }

            if (thread->mutex != NULL) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        default:
            break;
        }

        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return suspend;
}

s32 OSSuspendThread(OSThread* thread) {
    BOOL enabled;
    s32 suspend;
    OSThread* next;
    OSThread* prev;
    OSThread* tail;

    enabled = OSDisableInterrupts();
    suspend = thread->suspend++;

    if (suspend == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_RUNNING:
            RunQueueHint = TRUE;
            thread->state = OS_THREAD_STATE_READY;
            break;
        case OS_THREAD_STATE_READY:
            UnsetRun(thread);
            break;
        case OS_THREAD_STATE_SLEEPING:
            next = thread->next;
            prev = thread->prev;
            if (next == NULL) {
                thread->queue->tail = prev;
            } else {
                next->prev = prev;
            }

            if (prev == NULL) {
                thread->queue->head = next;
            } else {
                prev->next = next;
            }

            thread->priority = OS_PRIORITY_MAX + 1;

            tail = thread->queue->tail;
            if (tail == NULL) {
                thread->queue->head = thread;
            } else {
                tail->next = thread;
            }

            thread->prev = tail;
            thread->next = NULL;
            thread->queue->tail = thread;
            tail = thread;

            if (thread->mutex != NULL) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        default:
            break;
        }

        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return suspend;
}

void OSSleepThread(OSThreadQueue* queue) {
    BOOL enabled;
    OSThread* currThread;
    OSThread* iter;
    OSThread* iterPrev;

    enabled = OSDisableInterrupts();
    currThread = OSGetCurrentThread();

    currThread->state = OS_THREAD_STATE_SLEEPING;
    currThread->queue = queue;

    for (iter = queue->head;
         iter != NULL && iter->priority <= currThread->priority;
         iter = iter->next) {
    }

    if (iter == NULL) {
        iter = queue->tail;
        if (iter == NULL) {
            queue->head = currThread;
        } else {
            iter->next = currThread;
        }
        currThread->prev = iter;
        currThread->next = NULL;
        queue->tail = currThread;
    } else {
        currThread->next = iter;
        iterPrev = iter->prev;
        iter->prev = currThread;
        currThread->prev = iterPrev;

        if (iterPrev == NULL) {
            queue->head = currThread;
        } else {
            iterPrev->next = currThread;
        }
    }

    RunQueueHint = TRUE;
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

void OSWakeupThread(OSThreadQueue* queue) {
    BOOL enabled;
    OSThread* head;
    OSThread* next;

    enabled = OSDisableInterrupts();

    while (queue->head != NULL) {
        head = queue->head;
        next = head->next;

        if (next == NULL) {
            queue->tail = NULL;
        } else {
            next->prev = NULL;
        }

        queue->head = next;

        head->state = OS_THREAD_STATE_READY;
        if (head->suspend <= 0) {
            SetRun(head);
        }
    }

    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSSetThreadPriority(OSThread* thread, s32 prio) {
    BOOL enabled;

    if (prio < OS_PRIORITY_MIN || prio > OS_PRIORITY_MAX) {
        return FALSE;
    }

    enabled = OSDisableInterrupts();

    if (thread->base != prio) {
        thread->base = prio;
        UpdatePriority(thread);
        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return TRUE;
}

static void SleepAlarmHandler(OSAlarm* alarm, OSContext* ctx) {
#pragma unused(ctx)

    OSResumeThread((OSThread*)OSGetAlarmUserData(alarm));
}

void OSSleepTicks(s64 ticks) {
    BOOL enabled;
    OSAlarm alarm;
    OSThread* thread;

    enabled = OSDisableInterrupts();

    thread = OS_CURRENT_THREAD;
    if (thread == NULL || tl_self_os == NULL || np_in_irq()) {
        OSRestoreInterrupts(enabled);
        return;
    }

    OSCreateAlarm(&alarm);
    OSSetAlarmTag(&alarm, (u32)thread);
    OSSetAlarmUserData(&alarm, thread);
    OSSetAlarm(&alarm, ticks, SleepAlarmHandler);

    OSSuspendThread(thread);
    OSCancelAlarm(&alarm);
    OSRestoreInterrupts(enabled);
}


