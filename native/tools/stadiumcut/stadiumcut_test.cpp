// Host test of np/stadiumcut.h: the MSC_STADIUM_CUT parse, the table, the far-size rule.
#include <stdio.h>
#include "np/stadiumcut.h"

static int sFail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++sFail; } } while (0)

int main()
{
    CHECK(NpStadiumCutParse(nullptr, 4) == 0);
    CHECK(NpStadiumCutParse("", 4) == 0);
    CHECK(NpStadiumCutParse("off", 4) == 0);
    CHECK(NpStadiumCutParse("0", 4) == 0);
    CHECK(NpStadiumCutParse("3", 4) == 3);
    CHECK(NpStadiumCutParse("0x9", 7) == 9);
    CHECK(NpStadiumCutParse("3", -1) == 0);                       // no stadium: nothing
    CHECK(NpStadiumCutParse("32768", 4) == 0);                    // unknown bits dropped
    CHECK(NpStadiumCutParse("table", 4) == NpStadiumCutTableMask(4));
    CHECK(NpStadiumCutParse("table", 7) == NP_CUT_WARBLE && NpStadiumCutParse("table", 4) == (NP_CUT_RAINHULL | NP_CUT_MATFAR));
    CHECK(NpStadiumCutParse("table", 13) == 0);                    // Vice has no row
    CHECK(NpStadiumCutParse("4:3,7:5", 4) == 3);
    CHECK(NpStadiumCutParse("4:3,7:5", 7) == 5);
    CHECK(NpStadiumCutParse("4:3,7:5", 8) == 0);
    CHECK(NpStadiumCutParse("table,13:1", 13) == 1);
    CHECK(NpStadiumCutParse("table,13:1", 4) == NpStadiumCutTableMask(4));
    CHECK(NpStadiumCutParse("4:0x80", 4) == NP_CUT_TEST);
    CHECK(NpStadiumCutParse("4:64", 4) == NP_CUT_DUMPFX);
    CHECK(NpStadiumCutParse("64", 7) == NP_CUT_DUMPFX);
    CHECK(NpStadiumCutParse("junk", 4) == 0);
    CHECK(NpStadiumCutFarParse(nullptr) == 24.0f);
    CHECK(NpStadiumCutFarParse("0") == 1.0f && NpStadiumCutFarParse("999") == 200.0f);
    CHECK(NpStadiumCutFarSmall(10.0f, 24.0f) && !NpStadiumCutFarSmall(24.0f, 24.0f) && !NpStadiumCutFarSmall(100.0f, 24.0f));
    if (sFail) { printf("%d failures\n", sFail); return 1; }
    printf("stadiumcut test OK\n");
    return 0;
}
