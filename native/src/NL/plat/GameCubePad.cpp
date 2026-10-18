#include "NL/plat/GameCubePad.h"

static unsigned char sGameCubePadStorage[8 * sizeof(GameCubePad)] NP_STORAGE_ALIGN;
nlArrayAllocator<GameCubePad> gGameCubePadAllocator(reinterpret_cast<GameCubePad*>(sGameCubePadStorage), 8);
int* gGameCubePadButtonMap;
int gGameCubePadClassID = gNextPadClassID++;
