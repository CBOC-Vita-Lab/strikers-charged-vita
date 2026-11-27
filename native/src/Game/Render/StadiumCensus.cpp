// MSC_STADIUM_CENSUS=1 (diagnostic, off): what a stadium's world file holds, one block in the log at each load. Nothing in the code is keyed on most stadiums: what makes one
// heavy is in its data, and the disc is not in the source tree. The block compares stadiums without a frame of profiling and changes nothing that is drawn. Only reads.
#include "np/scenecut.h"

#include <stdio.h>
#include <string.h>

#include "NL/nlDLListContainer.inl"
#include "Game/DB/StadiumInfo.h"
#include "Game/World.h"
#include "Game/World/WorldDrawable.h"
#include "NL/gl/glModel.h"

namespace {

bool On()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_STADIUM_CENSUS");
        v = e && *e && *e != '0';
    }
    return v != 0;
}

// (World.h: the plain objects from 257, the stadium's from 65536)
unsigned sPlain[16];
unsigned sStadium[16];

} // namespace

void NpStadiumCensusObject(unsigned long type)
{
    if (!On()) return;
    if (type >= WORLD_OBJECT_DRAWABLE && type < WORLD_OBJECT_DRAWABLE + 16) ++sPlain[type - WORLD_OBJECT_DRAWABLE];
    else if (type >= WORLD_OBJECT_STADIUM_PHYSICS && type < WORLD_OBJECT_STADIUM_PHYSICS + 16) ++sStadium[type - WORLD_OBJECT_STADIUM_PHYSICS];
}

void NpStadiumCensusReport(int stadium, const World* world)
{
    if (!On()) return;
    unsigned drawables = 0, animated = 0, packets = 0, smallPackets = 0;
    unsigned long vertices = 0, largest = 0;
    nlDLListIterator<WorldDrawable*> it;
    it = const_cast<World*>(world)->m_renderObjects.Begin();
    while (it.hasNext())
    {
        const WorldDrawable* d = *it;
        ++drawables;
        if (d->m_pAnimController != 0) ++animated;
        const glModel* model = d->GetModel();
        if (model != 0)
        {
            unsigned long modelVertices = 0;
            for (unsigned i = 0; i < model->numPackets; ++i)
            {
                modelVertices += model->packets[i].numUniqueVertices;
                if (model->packets[i].numUniqueVertices <= 256) ++smallPackets;   // (what MSC_VGX_SOFTXFORM takes to the CPU)
            }
            packets += model->numPackets;
            vertices += modelVertices;
            if (modelVertices > largest) largest = modelVertices;
        }
        it.next();
    }
    fprintf(stderr, "[stadium-census] %s: drawables %u (%u animated), packets %u (%u of 256 vertices or fewer), vertices %lu, largest model %lu\n", stadium >= 0 && stadium < 18 ? GetStadiumName(stadium) : "?", drawables,
        animated, packets, smallPackets, vertices, largest);
    fprintf(stderr, "[stadium-census]   plain drawable %u visibility %u physics %u helper %u animation %u crowd-layout %u npc %u effect %u\n", sPlain[WORLD_OBJECT_DRAWABLE - 257],
        sPlain[WORLD_OBJECT_VISIBILITY - 257], sPlain[WORLD_OBJECT_PHYSICS - 257], sPlain[WORLD_OBJECT_HELPER - 257], sPlain[WORLD_OBJECT_ANIMATION - 257],
        sPlain[WORLD_OBJECT_CROWD_LAYOUT - 257], sPlain[WORLD_OBJECT_NPC - 257], sPlain[WORLD_OBJECT_EFFECT - 257]);
    fprintf(stderr, "[stadium-census]   stadium physics %u drawable %u light %u solar-flare %u toggle %u shadow-volume %u high-range %u\n", sStadium[WORLD_OBJECT_STADIUM_PHYSICS - 65536],
        sStadium[WORLD_OBJECT_STADIUM_DRAWABLE - 65536], sStadium[WORLD_OBJECT_STADIUM_LIGHT - 65536], sStadium[WORLD_OBJECT_SOLAR_FLARE - 65536], sStadium[WORLD_OBJECT_TOGGLE - 65536],
        sStadium[WORLD_OBJECT_SHADOW_VOLUME - 65536], sStadium[WORLD_OBJECT_HIGH_RANGE - 65536]);
    memset(sPlain, 0, sizeof(sPlain));
    memset(sStadium, 0, sizeof(sStadium));
}
