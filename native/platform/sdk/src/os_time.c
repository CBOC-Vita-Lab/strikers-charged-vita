// os_time.c: adapted from the decomp's ref/RVL_SDK/os/OSTime.c (calendar conversion unchanged);
// OSGetTime/OSGetTick, which read the PowerPC time base register, are native (np_plat clock).
#include <time.h>
#include "np_internal.h"
#include <revolution/os.h>

#define USEC_MAX 1000
#define MSEC_MAX 1000
#define MONTH_MAX 12
#define WEEK_DAY_MAX 7
#define YEAR_DAY_MAX 365

#define SECS_IN_MIN 60
#define SECS_IN_HOUR (SECS_IN_MIN * 60)
#define SECS_IN_DAY (SECS_IN_HOUR * 24)
#define SECS_IN_YEAR (SECS_IN_DAY * 365)

#define BIAS 0xB2575

// End of each month in standard year
static s32 YearDays[MONTH_MAX] = {0,   31,  59,  90,  120, 151,
                                  181, 212, 243, 273, 304, 334};
// End of each month in leap year
static s32 LeapYearDays[MONTH_MAX] = {0,   31,  60,  91,  121, 152,
                                      182, 213, 244, 274, 305, 335};

// ---- native time base (np_plat clock) ----------------------------------------------------------
// The time base counts ticks of the 60.75 MHz timer since 2000-01-01 (the console's RTC epoch);
// np_time_init() anchors it to the wall clock, so OSTicksToCalendarTime(OSGetTime()) is the date.
static uint64_t g_t0_ns;
static uint64_t g_base_ticks;

uint64_t np_os_ticks_from_ns(uint64_t ns) {
    const uint64_t clock = OS_TIME_SPEED;
    return (ns / 1000000000ull) * clock + (ns % 1000000000ull) * clock / 1000000000ull;
}

// The inverse: the np_time_ns() instant at which the time base reads `ticks` (alarm deadlines become event times).
uint64_t np_os_ns_from_ticks(uint64_t ticks) {
    const uint64_t clock = OS_TIME_SPEED;
    if (ticks <= g_base_ticks) return g_t0_ns;
    uint64_t dt = ticks - g_base_ticks;
    // rounded UP: a deadline of 12555000 ticks is 206666666.67 ns away; at 206666666 ns the time base still reads
    // 12554999, the alarm is not due and the idle scheduler would warp to the same instant forever
    return g_t0_ns + (dt / clock) * 1000000000ull + ((dt % clock) * 1000000000ull + clock - 1) / clock;
}

void np_time_init(void) {
    time_t now = time(0);
    const int64_t kUnixToY2k = 946684800;
    int64_t secs = (int64_t)now - kUnixToY2k;
    if (secs < 0) secs = 0;
    g_t0_ns = np_time_ns();
    g_base_ticks = (uint64_t)secs * (uint64_t)OS_TIME_SPEED;
}

uint64_t np_os_ticks(void) { return g_base_ticks + np_os_ticks_from_ns(np_time_ns() - g_t0_ns); }

// (Virtual time: reading the time base is a poll point, a game that spins on it with interrupts enabled sees the retrace.)
s64 OSGetTime(void) {
    np_vic_poll_hint();
    return (s64)np_os_ticks();
}
u32 OSGetTick(void) {
    np_vic_poll_hint();
    return (u32)np_os_ticks();
}
s64 __OSGetSystemTime(void) {
    BOOL enabled = OSDisableInterrupts();
    s64 time = OSGetTime() + OS_SYSTEM_TIME;
    OSRestoreInterrupts(enabled);
    return time;
}

s64 __OSTimeToSystemTime(s64 time) {
    BOOL enabled = OSDisableInterrupts();
    s64 sysTime = OS_SYSTEM_TIME + time;
    OSRestoreInterrupts(enabled);
    return sysTime;
}

static BOOL IsLeapYear(s32 year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static s32 GetYearDays(s32 year, s32 mon) {
    return (IsLeapYear(year) ? LeapYearDays : YearDays)[mon];
}

static s32 GetLeapDays(s32 year) {
    if (year < 1) {
        return 0;
    }
    return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
}

static void GetDates(s32 days, OSCalendarTime* cal) DECOMP_DONT_INLINE {
    s32 year;
    s32 totalDays;
    s32* p_days;
    s32 month;

    cal->wday = (days + 6) % WEEK_DAY_MAX;

    // WTF??
    for (year = days / YEAR_DAY_MAX;
         days < (totalDays = year * YEAR_DAY_MAX + GetLeapDays(year)); year--) {
        ;
    }
    days -= totalDays;
    cal->year = year;
    cal->yday = days;

    p_days = IsLeapYear(year) ? LeapYearDays : YearDays;
    for (month = MONTH_MAX; days < p_days[--month];) {
        ;
    }
    cal->month = month;
    cal->mday = days - p_days[month] + 1;
}

void OSTicksToCalendarTime(s64 ticks, OSCalendarTime* cal) {
    s32 days, secs;
    s64 d;

    d = ticks % OS_SEC_TO_TICKS(1);
    if (d < 0) {
        d += OS_SEC_TO_TICKS(1);
    }

    cal->usec = OS_TICKS_TO_USEC(d) % USEC_MAX;
    cal->msec = OS_TICKS_TO_MSEC(d) % MSEC_MAX;
    ticks -= d;

    days = (OS_TICKS_TO_SEC(ticks) / SECS_IN_DAY) + BIAS;
    secs = OS_TICKS_TO_SEC(ticks) % SECS_IN_DAY;
    if (secs < 0) {
        days -= 1;
        secs += SECS_IN_DAY;
    }

    GetDates(days, cal);
    cal->hour = secs / 60 / 60;
    cal->min = secs / 60 % 60;
    cal->sec = secs % 60;
}

s64 OSCalendarTimeToTicks(const OSCalendarTime* cal) {
    s64 seconds;
    s32 month;
    s32 ovMon;
    s32 year;

    ovMon = cal->month / MONTH_MAX;
    month = cal->month - (ovMon * MONTH_MAX);

    if (month < 0) {
        month += MONTH_MAX;
        ovMon--;
    }

    year = cal->year + ovMon;

    seconds = (s64)SECS_IN_YEAR * year +
              (s64)SECS_IN_DAY * (cal->mday + GetLeapDays(year) + GetYearDays(year, month) - 1) +
              (s64)SECS_IN_HOUR * cal->hour +
              (s64)SECS_IN_MIN * cal->min +
              cal->sec -
              (s64)0xEB1E1BF80ULL;

    return OS_SEC_TO_TICKS(seconds) + OS_MSEC_TO_TICKS((s64)cal->msec) +
           OS_USEC_TO_TICKS((s64)cal->usec);
}
