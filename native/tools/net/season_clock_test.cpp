// The game's own season / day calendar (src/Game/NetworkSeasonCalendar.cpp, unchanged) against the online service's (server/src/core/calendar.js).
// For each UTC time given on stdin (ms since the epoch, one per line) it prints "<ms> <season index> <year>-<month>-<day>" as the game works them
// out: DWC_GetDateTime answers the UTC time, the US region's hour shift is decided as NetworkUpdateTask::Init does (np/online_clock.h), then
// GetAdjustedNetworkDate and FindNetworkSeasonBoundary. tests/test_online_calendar.py compares the lines with the service's windows.
// Built by tests/test_online_calendar.py: g++ -Itools/net/season_shim (before -Iinclude) so the calendar's tweak includes resolve to stubs.
#include <dwc/dwc_nastime.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "Game/NetworkSeasonCalendar.h"
#include "np/online_clock.h"

static long long g_nowMs;

extern "C" BOOL DWC_GetDateTime(DWCDate* date, DWCTime* time)
{
    // the port's DWC clock (np_dwc_login.cpp): UTC, month 0-based, wday / yday filled
    const time_t s = (time_t)(g_nowMs / 1000);
    struct tm t;
    gmtime_r(&s, &t);
    date->mday = t.tm_mday;
    date->month = t.tm_mon;
    date->year = t.tm_year + 1900;
    date->wday = t.tm_wday;
    date->yday = t.tm_yday;
    time->sec = t.tm_sec;
    time->min = t.tm_min;
    time->hour = t.tm_hour;
    time->msec = (int)(g_nowMs % 1000);
    time->usec = 0;
    return 1;
}

int main(int argc, char** argv)
{
    const int retail = argc > 1 ? atoi(argv[1]) : -8;   // the region's retail shift (US -8, Japan +9)
    g_nAddHoursTime = NpOnlineHourShift(retail);
    char line[64];
    while (fgets(line, sizeof line, stdin))
    {
        g_nowMs = atoll(line);
        DWCDate date;
        DWCTime time;
        GetAdjustedNetworkDate(&date, &time);
        NetworkSeasonDate current;
        current.mMonth = date.month;
        current.mDay = date.mday;
        const int season = FindNetworkSeasonBoundary(&sNetworkSeasonDateTable, current);
        printf("%lld %d %d-%d-%d\n", g_nowMs, season, (int)date.year, (int)date.month, (int)date.mday);
    }
    return 0;
}
