/* np/online_clock.h: the hour shift the game applies to the online clock (g_nAddHoursTime, NetworkSeasonCalendar.cpp).
 *
 * Retail (NetworkUpdateTask::Init): a US console shifts the network time by -8 h and a Japanese one by +9 h before it works out the
 * weekly season and the day (GetAdjustedNetworkDate): when its season and daily totals start again, the date written into every
 * leaderboard submission, the season filter of the rows it shows and the "days left" of Online Central.
 * This port's service keeps one pooled board whose season and daily windows are UTC (server/src/core/calendar.js, PROTOCOL.md section
 * on rankings), and a row only counts while its last update lies inside the service's window. With the retail shift a US console went on
 * putting its old season / day total for 8 hours after the service had started the new window (clamped to the credit earned since),
 * then reset to 0 and put a smaller total, which the service stores as it comes: the points of those 8 hours were lost every week and
 * every day (Japan: 9 hours the other way, at 15:00 UTC). With no shift the game's windows are the service's windows.
 *
 *   MSC_ONLINE_RETAIL_TZ=1   keep the retail shift (default off: no shift on any region).
 *
 * native/tools/net/season_clock_test.cpp (tests/test_online_calendar.py) compares the game's own calendar code with the service's.
 */
#ifndef NP_ONLINE_CLOCK_H
#define NP_ONLINE_CLOCK_H

#include <stdlib.h>

static inline int NpOnlineHourShift(int retailHours)
{
    const char* e = getenv("MSC_ONLINE_RETAIL_TZ");
    return (e != 0 && e[0] == '1') ? retailHours : 0;
}

#endif /* NP_ONLINE_CLOCK_H */
