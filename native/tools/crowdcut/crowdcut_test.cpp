// Host test of np/crowdcut.h: the step of a crowd sprite by the size of its nearest figure, and the stagger of the groups over the pictures.
#include "np/crowdcut.h"

#include <stdio.h>

static int sFailed;
#define CHECK(c) do { if (!(c)) { ++sFailed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    // 1. size to class, no history: large 1, middling 2, small 3
    CHECK(NpCrowdClass(80.0f, 0, 40.0f, 20.0f, 0.15f) == 1);
    CHECK(NpCrowdClass(30.0f, 0, 40.0f, 20.0f, 0.15f) == 2);
    CHECK(NpCrowdClass(10.0f, 0, 40.0f, 20.0f, 0.15f) == 3);
    CHECK(NpCrowdClass(0.0f, 0, 40.0f, 20.0f, 0.15f) == 3);            // no figure: the far class
    // the step of a class, in time: 1 / 2 / 3 at 60 pictures a second, 1 / 2 / 2 at 30; a cap of 2 limits it
    CHECK(NpCrowdStepOfClass(1, 1, 3) == 1 && NpCrowdStepOfClass(2, 1, 3) == 2 && NpCrowdStepOfClass(3, 1, 3) == 3);
    CHECK(NpCrowdStepOfClass(1, 0, 3) == 1 && NpCrowdStepOfClass(2, 0, 3) == 2 && NpCrowdStepOfClass(3, 0, 3) == 2);
    CHECK(NpCrowdStepOfClass(3, 1, 2) == 2);
    // 2. hysteresis: a size that stays inside the band never changes the class
    int cls = NpCrowdClass(41.0f, 0, 40.0f, 20.0f, 0.15f);
    CHECK(cls == 1);
    for (float px = 34.5f; px < 45.0f; px += 0.5f)
    {
        cls = NpCrowdClass(px, cls, 40.0f, 20.0f, 0.15f);
        CHECK(cls == 1);
    }
    cls = NpCrowdClass(33.0f, cls, 40.0f, 20.0f, 0.15f);
    CHECK(cls == 2);
    for (float px = 33.0f; px < 45.5f; px += 0.5f)
    {
        cls = NpCrowdClass(px, cls, 40.0f, 20.0f, 0.15f);
        CHECK(cls == 2 || px >= 46.0f);
    }
    // 3. every group is drawn on 1 of `step` pictures, and a window of step pictures holds each group exactly once
    for (int stepN = 1; stepN <= 3; ++stepN)
    {
        for (unsigned cls = 0; cls < 12; ++cls)
        {
            for (unsigned f0 = 0; f0 < 7; ++f0)
            {
                int n = 0;
                for (unsigned f = f0; f < f0 + (unsigned)stepN; ++f) n += NpCrowdDue(f, cls, stepN);
                CHECK(n == 1);
            }
        }
    }
    // 4. the work is even per picture: 24 groups with serials in a row at step 3 draw 8 on every picture; at step 2, 12
    for (unsigned f = 0; f < 30; ++f)
    {
        int n3 = 0, n2 = 0;
        for (unsigned g = 0; g < 24; ++g) { n3 += NpCrowdDue(f, g, 3); n2 += NpCrowdDue(f, g, 2); }
        CHECK(n3 == 8);
        CHECK(n2 == 12);
    }
    // 5. a figure's height on screen: 1.5 units at 30 units with a 1.8 projection scale on the 544-line screen
    const float px = NpCrowdFigurePx(1.5f, 30.0f, 1.8f);
    CHECK(px > 24.0f && px < 25.0f);
    CHECK(NpCrowdFigurePx(1.5f, -1.0f, 1.8f) > 1.0e8f);               // behind the camera plane: as large as it gets
    CHECK(NpCrowdGroupClass(3, 1) == 1 && NpCrowdGroupClass(2, 3) == 2);
    if (sFailed == 0) printf("crowdcut: all checks passed\n");
    return sFailed != 0;
}
