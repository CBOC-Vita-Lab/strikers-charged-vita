/* np/live_settings.h: the three settings that apply to a running match (1.3).
 *
 * LESS SHAKE (MSC_REDUCED_MOTION), STADIUM CROWD (MSC_PERFORMANCE_MODE) and REAR TOUCH (MSC_REAR_TOUCH) used to be read once, from the environment, by a
 * function-local static (some of them in a header, one copy per source file). Now each has ONE shared cell. A cell starts "unread" and takes its value from
 * the environment on the first read, by exactly the rule the old reader had, so a start with an untouched file behaves bit for bit as before. After that a
 * save in the settings screen stores the new value with NpLiveSettingsSet (np_live_settings.cpp). The environment is never read again and never written
 * (setenv while other threads call getenv is not safe).
 *
 * All three only change what is drawn (shake size, crowd passes) or which local touch panel is read (rear touch); none is in the lockstep (so_net.cpp
 * lists them as never compared) and none is read by the simulation.
 *
 * The cells are function-local statics of inline functions, so there is one cell in the program whatever number of files include this header
 * (the game is built as C++11: no inline variables).
 */
#ifndef NP_LIVE_SETTINGS_H
#define NP_LIVE_SETTINGS_H

#ifdef __cplusplus
#include <stdlib.h>   /* (no <atomic>: it pulls <new>, which the game's own headers redefine) */

enum NpLiveCellId { NP_LIVE_SHAKE = 0, NP_LIVE_CROWD = 1, NP_LIVE_REAR = 2, NP_LIVE_CELLS = 3 };
enum { NP_LIVE_UNREAD = -2 };

/* The three cells (plain ints used through the compiler's atomic builtins). Shake: 0 full, 1 quarter. Crowd: 0 shown, 1 hidden. Rear: -1 follows the layout, 0 off, 1 on. */
inline int* NpLiveCell(int id)
{
    static int cells[NP_LIVE_CELLS] = {NP_LIVE_UNREAD, NP_LIVE_UNREAD, NP_LIVE_UNREAD};
    return &cells[id];
}

/* What the old readers took from the environment (the first read of a start). */
inline int NpLiveFromEnvironment(int id)
{
    const char* e = getenv(id == NP_LIVE_SHAKE ? "MSC_REDUCED_MOTION" : id == NP_LIVE_CROWD ? "MSC_PERFORMANCE_MODE" : "MSC_REAR_TOUCH");
    if (id == NP_LIVE_REAR) return (e && *e) ? (*e != '0' ? 1 : 0) : -1;
    return (e && *e && *e != '0') ? 1 : 0;
}

inline int NpLiveGet(int id)
{
    int* cell = NpLiveCell(id);
    int v = __atomic_load_n(cell, __ATOMIC_RELAXED);
    if (v == NP_LIVE_UNREAD)
    {
        const int fromEnvironment = NpLiveFromEnvironment(id);
        int expected = NP_LIVE_UNREAD;
        v = __atomic_compare_exchange_n(cell, &expected, fromEnvironment, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ? fromEnvironment : expected;   /* a Set that came first wins */
    }
    return v;
}

inline int NpLiveShakeQuarter(void) { return NpLiveGet(NP_LIVE_SHAKE); }   /* 1: camera shake is a quarter of its size */
inline int NpLiveCrowdHidden(void) { return NpLiveGet(NP_LIVE_CROWD); }    /* 1: the crowd passes are skipped */
inline int NpLiveRear(void) { return NpLiveGet(NP_LIVE_REAR); }            /* -1 follow the layout, 0 off, 1 on */

extern "C" {
#endif
/* Store the value of a row's choice: key is the environment name the choice writes (MSC_REDUCED_MOTION, MSC_PERFORMANCE_MODE, MSC_REAR_TOUCH), value its
 * text, or NULL for "the key is not in the file" (the unset rule: full shake, crowd shown, rear follows the layout). Any other key is ignored.
 * Never calls setenv. Returns 1 when a cell was set. */
int NpLiveSettingsSet(const char* key, const char* valueOrNull);
#ifdef __cplusplus
}
#endif

#endif /* NP_LIVE_SETTINGS_H */
