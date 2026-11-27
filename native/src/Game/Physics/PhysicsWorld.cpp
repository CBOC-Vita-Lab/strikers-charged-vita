#include "Game/Physics/PhysicsWorld.h"

#include "Game/DebugWriteCache.h"
#include "Game/Physics/CollisionSpace.h"
#include "Game/Physics/PhysicsObject.h"
#include "ode/NLGAdditions.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "np/gsplit.h"
#include "np/physpose.h"   // MSC_GSPLIT phase markers (no-ops unless NP_NATIVE and MSC_GSPLIT)

PhysicsWorld::PhysicsWorld()
{
    m_World = dWorldCreate();
    m_ContactGroup = dJointGroupCreate(0);
    m_SpaceList = 0;
    m_SyncLogContext = 0;
    m_SyncLogCache = 0;
    dWorldSetAutoDisableFlag(m_World, 0);
}

PhysicsWorld::~PhysicsWorld()
{
    dJointGroupDestroy(m_ContactGroup);
    dWorldDestroy(m_World);
}

void PhysicsWorld::SetCFM(float cfm)
{
    dWorldSetCFM(m_World, cfm);
}

void PhysicsWorld::SetERP(float erp)
{
    dWorldSetERP(m_World, erp);
}

void PhysicsWorld::AddCollisionSpace(CollisionSpace* collisionSpace)
{
    collisionSpace->m_nextCollisionSpace = m_SpaceList;
    m_SpaceList = collisionSpace;
}

void PhysicsWorld::PreCollide(CollisionSpace* collisionSpace)
{
    collisionSpace->PreCollide();
}

void PhysicsWorld::PreCollide(PhysicsObject* object)
{
    object->PreCollide();
}

void PhysicsWorld::PreUpdate(CollisionSpace* collisionSpace)
{
    collisionSpace->PreUpdate();
}

void PhysicsWorld::PreUpdate(PhysicsObject* object)
{
    object->PreUpdate();
}

void PhysicsWorld::PostUpdate(PhysicsObject* object)
{
    object->PostUpdate();
}

void PhysicsWorld::DoCollide(CollisionSpace* collisionSpace)
{
    NP_GS(NPGS_PH_BROAD);
    collisionSpace->DoCollide(this, SpaceCollideCallback);
}

PhysicsWorld::PhysicsObjectCallback s_PreCollideObjectCallback = &PhysicsWorld::PreCollide;
PhysicsWorld::CollisionSpaceCallback s_PreCollideSpaceCallback = &PhysicsWorld::PreCollide;
PhysicsWorld::CollisionSpaceCallback s_DoCollideCallback = &PhysicsWorld::DoCollide;
PhysicsWorld::CollisionSpaceCallback s_PreUpdateSpaceCallback = &PhysicsWorld::PreUpdate;
PhysicsWorld::PhysicsObjectCallback s_PreUpdateObjectCallback = &PhysicsWorld::PreUpdate;
PhysicsWorld::PhysicsObjectCallback s_PostUpdateObjectCallback = &PhysicsWorld::PostUpdate;
PhysicsWorld::PhysicsObjectCallback s_LogBodyCallback = &PhysicsWorld::LogBody;
PhysicsWorld::CollisionSpaceCallback s_LogSpaceCallback = &PhysicsWorld::LogSpace;

void PhysicsWorld::Collide()
{
    NP_GS(NPGS_PH_PRECOLLIDE);
    PhysicsObjectCallback bodyCallback = s_PreCollideObjectCallback;
    for (dBodyID body = dWorldGetFirstBody(m_World); body != 0; body = dBodyGetNextBody(body))
    {
        (this->*bodyCallback)((PhysicsObject*)dBodyGetData(body));
    }

    CollisionSpaceCallback spaceCallback = s_PreCollideSpaceCallback;
    for (CollisionSpace* space = m_SpaceList; space != 0; space = space->m_nextCollisionSpace)
    {
        (this->*spaceCallback)(space);
    }

    CollisionSpaceCallback collideCallback = s_DoCollideCallback;
    for (CollisionSpace* space = m_SpaceList; space != 0; space = space->m_nextCollisionSpace)
    {
        (this->*collideCallback)(space);
    }
}

void PhysicsWorld::PreUpdate()
{
    CollisionSpaceCallback spaceCallback = s_PreUpdateSpaceCallback;
    for (CollisionSpace* space = m_SpaceList; space != 0; space = space->m_nextCollisionSpace)
    {
        (this->*spaceCallback)(space);
    }

    PhysicsObjectCallback bodyCallback = s_PreUpdateObjectCallback;
    NP_GS(NPGS_PH_PREUPD);
    for (dBodyID body = dWorldGetFirstBody(m_World); body != 0; body = dBodyGetNextBody(body))
    {
        NP_GSC(NPGC_BODIES, 1);
        PhysicsObject* const object = (PhysicsObject*)dBodyGetData(body);
        NP_GS(g_npGsOn ? NpGsPairId(1, object->m_geomID ? dGeomGetClass(object->m_geomID) : 15, 0) : 0);
        (this->*bodyCallback)(object);
    }
}

void PhysicsWorld::PostUpdate()
{
    PhysicsObjectCallback bodyCallback = s_PostUpdateObjectCallback;
    NP_GS(NPGS_PH_POSTUPD);
#ifdef NP_NATIVE
    if (NpPhysPoseMode() != 0)
        NpPhysPoseRun(this);
#endif
    for (dBodyID body = dWorldGetFirstBody(m_World); body != 0; body = dBodyGetNextBody(body))
    {
        PhysicsObject* const object = (PhysicsObject*)dBodyGetData(body);
        NP_GS(g_npGsOn ? NpGsPairId(2, object->m_geomID ? dGeomGetClass(object->m_geomID) : 15, 0) : 0);
        (this->*bodyCallback)(object);
    }
}

void PhysicsWorld::Update(float quickStepSize, bool clearAccumulators)
{
    dWorldSetClearAccumulators(m_World, clearAccumulators ? 1 : 0);
    dWorldQuickStep(m_World, quickStepSize);
    {
        NP_GS(NPGS_PH_JOINTS);
        dJointGroupEmpty(m_ContactGroup);
    }
}

// MSC_PHYS27_PLANE (ode/collision_kernel.cpp NpOdeNoContact)
int NpOdeNoContact(dxGeom* o1, dxGeom* o2);
namespace
{
int sP27Mode = -1, sP27Break = 0;
unsigned long long sP27Pairs, sP27Skipped, sP27Checked, sP27Wrong;
unsigned long long sP27Left[32][32];
int NpPhys27PlaneMode()
{
    if (sP27Mode < 0)
    {
        const char* e = getenv("MSC_PHYS27_PLANE");
        sP27Mode = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
        const char* b = getenv("MSC_PHYS27_PLANE_BREAK");
        sP27Break = (b && b[0] == '1') ? 1 : 0;
    }
    return sP27Mode;
}
bool NpPhys27Reject(dxGeom* g1, dxGeom* g2)
{
    sP27Pairs++;
    bool r = NpOdeNoContact(g1, g2) != 0;
    if (sP27Break == 1 && !r && (sP27Pairs & 7) == 0 && (dGeomGetClass(g1) == dPlaneClass || dGeomGetClass(g2) == dPlaneClass))
        r = true;   // proof mutant
    if (r)
        sP27Skipped++;
    else
    {
        // what is left: class pairs of the callbacks that were run (a diagnostic of the first 8 x 8 class numbers; planes and transforms show their inner class)
        int c1 = dGeomGetClass(g1), c2 = dGeomGetClass(g2);
        if (c1 == dGeomTransformClass && dGeomTransformGetGeom(g1)) c1 = 100 + dGeomGetClass(dGeomTransformGetGeom(g1));
        if (c2 == dGeomTransformClass && dGeomTransformGetGeom(g2)) c2 = 100 + dGeomGetClass(dGeomTransformGetGeom(g2));
        if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
        c1 = c1 < 100 ? c1 : c1 - 100 + 16;
        c2 = c2 < 100 ? c2 : c2 - 100 + 16;
        if (c1 < 32 && c2 < 32)
            sP27Left[c1][c2]++;
    }
    if ((sP27Pairs % 200000) == 0)
    {
        printf("[phys27plane] mode=%d break=%d pairs=%llu skipped=%llu verify checked=%llu wrong=%llu\n", sP27Mode, sP27Break, sP27Pairs, sP27Skipped, sP27Checked, sP27Wrong);
        for (int a = 0; a < 32; a++)
            for (int b = 0; b < 32; b++)
                if (sP27Left[a][b] * 200 > sP27Pairs)
                    printf("[phys27plane]   left: class %d%s x %d%s: %llu\n", a < 16 ? a : a - 16, a < 16 ? "" : "(in transform)", b < 16 ? b : b - 16, b < 16 ? "" : "(in transform)", sP27Left[a][b]);
        fflush(stdout);
    }
    return r;
}
}

void PhysicsWorld::SpaceCollideCallback(void* data, dxGeom* geom1, dxGeom* geom2)
{
    if (dGeomIsSpace(geom1) || dGeomIsSpace(geom2))
    {
        dSpaceCollide2(geom1, geom2, data, SpaceCollideCallback);
        if (dGeomIsSpace(geom1))
        {
            dSpaceCollide((dSpaceID)geom1, data, SpaceCollideCallback);
        }
        if (dGeomIsSpace(geom2))
        {
            dSpaceCollide((dSpaceID)geom2, data, SpaceCollideCallback);
        }
        return;
    }

    // MSC_PHYS27_PLANE: a pair that dCollide surely answers with 0 contacts (no collider for the classes: plane-plane; a plane and a transformed sphere / box /
    // capsule whose AABB is clear of the plane) has no effect in this callback (the body test, SetContactInfo and the narrow phase only fill locals), so it
    // is not run. =2 verify: runs the callback as before and checks that a pair the test calls empty gave 0 contacts (counted, printed every 20000 pairs).
    // MSC_PHYS27_PLANE_BREAK=1: proof mutant (a reject also for a plane and a transform that has no clearance: every 8th pair).
    if (NpPhys27PlaneMode() == 1 && NpPhys27Reject(geom1, geom2))
        return;
    NP_GS(NPGS_PH_NEAR);
    NP_GSC(NPGC_CALLBACK, 1);
    dContact* contact;
    dContact contacts[20];
    dBodyID body1;

    body1 = dGeomGetBody(geom1);
    dBodyID body2 = dGeomGetBody(geom2);

    if (body1 != 0 && body2 != 0)
    {
        if (dAreConnectedExcluding(body1, body2, 4) != 0)
        {
            NP_GSC(NPGC_CB_CONNECTED, 1);
            return;
        }
    }

    PhysicsObject* object1 = (PhysicsObject*)dGeomGetData(geom1);
    PhysicsObject* object2 = (PhysicsObject*)dGeomGetData(geom2);

    bool canCollide1 = object1->SetContactInfo(&contacts[0], object2, true);
    bool canCollide2 = object2->SetContactInfo(&contacts[0], object1, false);
    if (!canCollide1 || !canCollide2)
    {
        NP_GSC(NPGC_CB_NOCOLL, 1);
        return;
    }

    int numContacts;
    {
        int gsC1 = 0, gsC2 = 0;
        if (g_npGsOn)
        {
            gsC1 = dGeomGetClass(geom1);
            gsC2 = dGeomGetClass(geom2);
            if (gsC1 > gsC2) { int t = gsC1; gsC1 = gsC2; gsC2 = t; }
        }
        NP_GS(g_npGsOn ? NpGsPairId(0, gsC1, gsC2) : 0);
        NP_GSC(NPGC_NARROW, 1);
        numContacts = dCollide(geom1, geom2, 20, &contacts[0].geom, sizeof(dContact));
    }
    if (sP27Mode == 2)
    {
        // MSC_PHYS27_PLANE=2: the pair the test calls empty must have made no contact
        if (NpPhys27Reject(geom1, geom2))
        {
            sP27Checked++;
            if (numContacts > 0)
                sP27Wrong++;
        }
    }
    if (numContacts <= 0)
    {
        return;
    }
    NP_GSC(NPGC_NARROW_HIT, 1);
    NP_GSC(NPGC_CONTACTS, numContacts);

    ContactType result1, result2;
    {
        NP_GS(NPGS_PH_CONTACTCB);
        result1 = object1->Contact(object2, contacts, numContacts);
        result2 = object2->Contact(object1, contacts, numContacts);
    }

    ContactType transformedResult2;
    if (result2 == ONE_WAY_CONTACT_OTHER)
    {
        transformedResult2 = ONE_WAY_CONTACT_THIS;
    }
    else if (result2 == ONE_WAY_CONTACT_THIS)
    {
        transformedResult2 = ONE_WAY_CONTACT_OTHER;
    }
    else
    {
        transformedResult2 = result2;
    }

    int combinedResult = result1 & transformedResult2;
    if (combinedResult == 0)
    {
        return;
    }
    if (combinedResult == ONE_WAY_CONTACT_OTHER)
    {
        body1 = 0;
    }
    if (combinedResult == ONE_WAY_CONTACT_THIS)
    {
        body2 = 0;
    }

    int i;
    PhysicsWorld* world; // declared before the goto: C++ forbids jumping over initialisations
    dJointID joint;
    NP_GS(NPGS_PH_JOINTS);
    NP_GSC(NPGC_JOINTS, numContacts);
    contact = &contacts[0];
    i = 0;
    goto checkContact;

processContact:
    if (i != 0)
    {
        contact->surface.mode = contacts[0].surface.mode;
        contact->surface.mu = contacts[0].surface.mu;
        contact->surface.mu2 = contacts[0].surface.mu2;
        contact->surface.bounce = contacts[0].surface.bounce;
        contact->surface.bounce_vel = contacts[0].surface.bounce_vel;
        contact->surface.soft_erp = contacts[0].surface.soft_erp;
        contact->surface.soft_cfm = contacts[0].surface.soft_cfm;
        contact->surface.motion1 = contacts[0].surface.motion1;
        contact->surface.motion2 = contacts[0].surface.motion2;
        contact->surface.slip1 = contacts[0].surface.slip1;
        contact->surface.slip2 = contacts[0].surface.slip2;
    }

    world = (PhysicsWorld*)data;
    joint = dJointCreateContact(world->m_World, world->m_ContactGroup, contact);
    dJointAttach(joint, body1, body2);
    contact++;
    i++;

checkContact:
    if (i < numContacts)
    {
        goto processContact;
    }
}

void PhysicsWorld::LogBody(PhysicsObject* object)
{
    dBodySyncLog(object->m_bodyID, m_SyncLogContext, m_SyncLogCache);
    object->SyncLog(m_SyncLogContext, m_SyncLogCache);
}

void PhysicsWorld::LogSpace(CollisionSpace* collisionSpace)
{
    collisionSpace->SyncLogSpace();
}

struct GenGeomTypeState
{
    unsigned short type;
    unsigned short padding;
};

GenGeomTypeState s_GenGeomType = { 0xFFFF, 0 };
char s_CollideBitsName[12] = "m_collBits";
char s_CategoryBitsName[12] = "m_catBits";
char s_GeomFlagsName[9] = "m_gflags";
char s_RotationName[4] = "m_R";
char s_PositionName[8] = "m_pos";
char s_TypeName[8] = "m_type";
char s_GenGeomName[8] = "GenGeom";

void PhysicsWorld::LogGeom(PhysicsObject* object)
{
    void* context;
    DebugWriteCache* cache;

    struct GenGeom
    {
        int m_type;
        int m_gflags;
        dVector3 m_pos;
        dMatrix3 m_R;
        unsigned long m_catBits;
        unsigned long m_collBits;
    } geomData;

    dGeomID geom = object->m_geomID;
    geomData.m_type = dGeomGetClass(geom);
    geomData.m_gflags = dGeomGetGFlags(geom);

    if (!dGeomIsSpace(geom) && dGeomIsPlaceable(geom))
    {
        dGeomGetPosition(geom, geomData.m_pos);
        dGeomGetRotation(geom, geomData.m_R);
        geomData.m_catBits = dGeomGetCategoryBits(geom);
        geomData.m_collBits = dGeomGetCollideBits(geom);
    }
    else
    {
        memset(geomData.m_pos, 0, sizeof(geomData.m_pos));
        memset(geomData.m_R, 0, sizeof(geomData.m_R));
        geomData.m_catBits = 0;
        geomData.m_collBits = 0;
    }

    cache = m_SyncLogCache;
    context = m_SyncLogContext;
    if (s_GenGeomType.type == 0xFFFF)
    {
        s_GenGeomType.type = cache->BeginType(s_GenGeomName);
        cache->AddField(8, gDebugFieldTypes[8].size, 0, s_TypeName);
        cache->AddField(8, gDebugFieldTypes[8].size, (unsigned char*)&geomData.m_gflags - (unsigned char*)&geomData, s_GeomFlagsName);
        cache->AddField(27, gDebugFieldTypes[27].size, (unsigned char*)&geomData.m_pos - (unsigned char*)&geomData, s_PositionName);
        cache->AddField(30, gDebugFieldTypes[30].size, (unsigned char*)&geomData.m_R - (unsigned char*)&geomData, s_RotationName);
        cache->AddField(13, gDebugFieldTypes[13].size, (unsigned char*)&geomData.m_catBits - (unsigned char*)&geomData, s_CategoryBitsName);
        cache->AddField(13, gDebugFieldTypes[13].size, (unsigned char*)&geomData.m_collBits - (unsigned char*)&geomData, s_CollideBitsName);
        cache->EndType();
    }

    cache->ChecksumData(s_GenGeomType.type, &geomData, context);
    cache->WriteData(s_GenGeomType.type, &geomData, sizeof(geomData));
}

void PhysicsWorld::SyncLog(void* context, DebugWriteCache* cache)
{
    m_SyncLogContext = context;
    m_SyncLogCache = cache;

    PhysicsObjectCallback bodyCallback = s_LogBodyCallback;
    for (dBodyID body = dWorldGetFirstBody(m_World); body != 0; body = dBodyGetNextBody(body))
    {
        (this->*bodyCallback)((PhysicsObject*)dBodyGetData(body));
    }

    CollisionSpaceCallback spaceCallback = s_LogSpaceCallback;
    for (CollisionSpace* space = m_SpaceList; space != 0; space = space->m_nextCollisionSpace)
    {
        (this->*spaceCallback)(space);
    }
}

void PhysicsWorld::DoCollisions(
    PhysicsObject* object, nlListContainer<PhysicsObject*>& container)
{
    ListEntry<PhysicsObject*>* entry = container.m_Head;
    dGeomID geom = object->m_geomID;
    for (; entry != 0; entry = entry->next)
    {
        dGeomCollideAABBs(geom, entry->entry->m_geomID, this, SpaceCollideCallback);
    }
}

#ifdef NP_NATIVE
// MSC_GSPLIT census, printed with the physics line: the geoms of every collision space by class and by kind (dynamic = on a body, static = none), the enabled ones, and the bodies.
// Read-only walk of the world on the game thread at the line (one picture in 300).
extern "C" int NpGsPhysCensus(char* out, int cap)
{
    extern PhysicsWorld* g_PhysicsWorld;
    if (g_PhysicsWorld == 0 || cap < 64) return 0;
    static const char* const names[] = {"sphere", "box", "capsule", "cylinder", "plane", "ray", "transform", "trimesh", "simple", "hash", "quad", "user0", "user1", "user2", "user3", "?"};
    int dyn[16] = {0}, sta[16] = {0}, enabledGeoms = 0, spaces = 0;
    for (CollisionSpace* space = g_PhysicsWorld->m_SpaceList; space != 0; space = space->m_nextCollisionSpace)
    {
        ++spaces;
        const int n = dSpaceGetNumGeoms(space->m_spaceID);
        for (int i = 0; i < n; i++)
        {
            dGeomID geom = dSpaceGetGeom(space->m_spaceID, i);
            const int c = dGeomGetClass(geom);
            if (c < 0 || c > 15) continue;
            if (dGeomGetBody(geom) != 0) dyn[c]++; else sta[c]++;
            if (dGeomIsEnabled(geom)) enabledGeoms++;
        }
    }
    int bodies = 0, bodiesOn = 0;
    for (dBodyID body = dWorldGetFirstBody(g_PhysicsWorld->m_World); body != 0; body = dBodyGetNextBody(body))
    {
        ++bodies;
        if (dBodyIsEnabled(body)) ++bodiesOn;
    }
    int m = snprintf(out, cap, "spaces %d geoms-enabled %d bodies %d enabled %d geoms (dynamic/static):", spaces, enabledGeoms, bodies, bodiesOn);
    for (int c = 0; c < 16 && m < cap - 40; c++)
        if (dyn[c] + sta[c] > 0) m += snprintf(out + m, cap - m, " %s %d/%d", names[c], dyn[c], sta[c]);
    return m < cap ? m : cap - 1;
}
#endif
