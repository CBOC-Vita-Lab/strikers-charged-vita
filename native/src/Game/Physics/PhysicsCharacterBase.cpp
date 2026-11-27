#include "NL/nlDLListContainer.inl"
#include "Game/Physics/PhysicsCharacterBase.h"
#include "Game/Physics/PhysicsCharacterBaseData.h"

#include "Game/DebugWriteCache.h"
#include "Game/Physics/CharacterPhysicsElement.h"
#include "Game/Physics/CollisionSpace.h"
#include "Game/Physics/PhysicsWorld.h"
#include "Game/PoseAccumulator.h"
#include "Game/SHierarchy.h"
#include "NL/nlMemory.h"
#include "NL/platvmath.h"
#include "ode/NLGAdditions.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct BoneVolumeTypeState
{
    unsigned short type;
    unsigned short padding;
};

extern BoneVolumeTypeState s_BoneVolumeType;
extern char s_BoneIDName[5];
extern char s_BoneVolumeName[8];

PhysicsCharacterBase::PhysicsCharacterBase(
    CollisionSpace*, PhysicsWorld* world, float centreOfMassHeight)
    : PhysicsCompositeObject(world)
    , m_CentreOfMassHeight(centreOfMassHeight)
    , m_Unknown94(0)
{
    dBodySetUpdateMode(m_bodyID, 1, 0);
    dBodySetGravityMode(m_bodyID, 0);

    m_CharacterJoint = dJointCreateCharacter(world->m_World, 0);
    dJointAttach(m_CharacterJoint, m_bodyID, 0);

    float direction[3];
    direction[0] = 0.0f;
    direction[1] = 0.0f;
    direction[2] = 1.0f;
    dJointSetCharacterNoMotionDirection(m_CharacterJoint, direction);

    dBodySetAutoDisableFlag(m_bodyID, 0);
}

PhysicsCharacterBase::~PhysicsCharacterBase()
{
    dJointDestroy(m_CharacterJoint);

    ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head;
    while (entry != 0)
    {
        delete entry->entry;
        entry = entry->next;
    }
}

void PhysicsCharacterBase::SetCharacterPosition(const nlVector3& position)
{
    nlVector3 adjustedPosition;
    adjustedPosition.x = position.x;
    adjustedPosition.y = position.y;
    adjustedPosition.z = position.z + m_CentreOfMassHeight;
    SetPosition(adjustedPosition, WORLD_COORDINATES);
}

static inline PhysicsCharacterBase* GetRootCharacter(PhysicsObject* object)
{
    PhysicsObject* root = object;
    int type = root->GetObjectType();

    if (type == PHYSOBJ_CAPSULE_BONE || type == PHYSOBJ_SPHERE_BONE)
    {
        PhysicsObject* parent;
        while ((parent = root->m_parentObject) != 0)
        {
            root = parent;
        }
    }

    return root->GetObjectType() == PHYSOBJ_CHARACTER ? (PhysicsCharacterBase*)root : 0;
}

bool PhysicsCharacterBase::BaseSetContactInfo(
    dContact* contact, PhysicsObject* other, bool first)
{
    int objectType = other->GetObjectType();

    if (first)
    {
        SetDefaultContactInfo(contact);
        PhysicsCharacterBase* owner = GetRootCharacter(other);
        if (owner != 0)
        {
            contact->surface.mode &= ~dContactBounce;
            contact->surface.bounce = 0.0f;
            contact->surface.bounce_vel = 0.0f;
        }
    }

    contact->surface.mode |= dContactSoftERP | dContactSoftCFM;
    contact->surface.soft_erp = 0.2f;
    contact->surface.soft_cfm = 0.00001f;
    contact->surface.mu = 0.0f;

    if (objectType == PHYSOBJ_PLANE || objectType == 0xB)
    {
        contact->surface.mode &= ~dContactBounce;
        contact->surface.bounce = 0.0f;
        contact->surface.bounce_vel = 0.0f;
    }

    return true;
}

void PhysicsCharacterBase::BasePreCollide()
{
}

void PhysicsCharacterBase::ContainObject(PhysicsObject* object)
{
    if (object->m_parentObject != this)
    {
        m_SubObject.Attach(object, this);
    }
}

void PhysicsCharacterBase::ReleaseObject()
{
    m_SubObject.Release();
}

void PhysicsCharacterBase::SetFacingDirection(unsigned short angle)
{
    float radians = (float)angle * 0.0000958738f;
    nlMatrix4 rotation;
    nlMakeRotationMatrixZ(rotation, radians);
    SetRotation(rotation);
}

void PhysicsCharacterBase::Unknown0()
{
    PhysicsCompositeObject::Unknown0();
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head;
        entry != 0;
        entry = entry->next)
    {
        PhysicsBoneVolume* volume = entry->entry;
        volume->m_pObject->GetPosition(&volume->m_PrevPosition);
    }
}

void PhysicsCharacterBase::SyncLog(void* context, DebugWriteCache* cache)
{
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head;
        entry != 0;
        entry = entry->next)
    {
        PhysicsBoneVolume* volume = entry->entry;
        if (s_BoneVolumeType.type == 0xFFFF)
        {
            s_BoneVolumeType.type = cache->BeginType(s_BoneVolumeName);
            cache->AddField(9, gDebugFieldTypes[9].size, (unsigned char*)&volume->m_BoneIndex - (unsigned char*)volume, s_BoneIndexName);
            cache->AddField(26, gDebugFieldTypes[26].size, (unsigned char*)&volume->m_Transform - (unsigned char*)volume, s_TransformName);
            cache->AddField(9, gDebugFieldTypes[9].size, (unsigned char*)&volume->m_TransformHandle - (unsigned char*)volume, s_TransformHandleName);
            cache->AddField(22, gDebugFieldTypes[22].size, (unsigned char*)&volume->m_PrevPosition - (unsigned char*)volume, s_PrevPositionName);
            cache->AddField(8, gDebugFieldTypes[8].size, (unsigned char*)&volume->m_ID - (unsigned char*)volume, s_BoneIDName);
            cache->EndType();
        }

        cache->ChecksumData(s_BoneVolumeType.type, volume, context);
        cache->WriteData(s_BoneVolumeType.type, volume, sizeof(PhysicsBoneVolume));
    }
}

void PhysicsCharacterBase::UpdatePose(
    cPoseAccumulator* pose, float heightOffset, bool offsetCentreOfMass)
{
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head;
        entry != 0;
        entry = entry->next)
    {
        PhysicsBoneVolume* volume = entry->entry;
        volume->m_PrevPosition = volume->m_pObject->GetPosition();

        nlMatrix4 worldMatrix = pose->GetNodeMatrix(volume->m_BoneIndex);
        nlMultMatrices(worldMatrix, volume->m_Transform, pose->GetNodeMatrix(volume->m_BoneIndex));

        if (offsetCentreOfMass)
        {
            worldMatrix.m43 -= m_CentreOfMassHeight;
            worldMatrix.m43 += heightOffset;
        }

        nlMatrix4 normalizedMatrix = worldMatrix;
        for (int row = 0; row < 3; ++row)
        {
            nlVector3& matrixRow = *(nlVector3*)&normalizedMatrix.e[row * 4];
            nlVector3 basis;
            nlVec3Set(basis, matrixRow.x, matrixRow.y, matrixRow.z);
            nlVec3Scale(basis,
                nlRecipSqrt(nlVec3LengthSquared(basis), true));
            matrixRow.x = basis.x;
            matrixRow.y = basis.y;
            matrixRow.z = basis.z;
        }
        worldMatrix = normalizedMatrix;

        AdjustTransform(volume->m_TransformHandle, worldMatrix, offsetCentreOfMass != false);
    }
}

// ---- MSC_SIM26_BONES (wave 12, sim26): the arithmetic of UpdatePose on the pose lanes (np/physpose.h), the writes in pass 3 ----
// UpdatePose made, per bone volume, the normalised world matrix of the bone (node matrix x volume transform) and set it on the volume's object through
// SetRotation / SetPosition in world coordinates: the parent's rotation transposed and multiplied, the parent's rot-trans matrix inverted and applied.
// With MSC_SIM26_BONES=1 the pose lane that made the character's pose also does that arithmetic (NpSim26Precompute, the same statements on the same
// operands), and pass 3 on the game thread, in body order where UpdatePose was, only checks and writes (NpSim26Apply): the volume's previous position
// is read as before, then for each bone every input of the arithmetic is compared bit for bit with what the lane used (the node matrix, the volume
// transform, the component, object and parent pointers, a parent without a parent, the parent's ODE rotation before the rotation write and its rotation
// and position before the position write); a bone whose inputs differ is done the old way. So each write gets the bits the old code would have computed.
// =2 verify: the arithmetic is redone on the game thread from the live inputs and compared (a [sim26bones] line: wrong must be 0), the writes are the old code's.
// MSC_SIM26_BONES_BREAK=1: proof mutant (the lane's position of every 4th bone nudged).
void NpSim26LocalRotation(const nlMatrix4& m4_in, const float* dRot, float* parentOdeRotation);
void NpSim26LocalPosition(const nlVector3& pos, const float* dRot, const float* ppos, nlVector3* out);
const float* NpSim26ObjRotation(const PhysicsObject* o);
const float* NpSim26ObjPosition(const PhysicsObject* o);
void NpSim26SetLocalRotation(PhysicsObject* o, const float* R);
void NpSim26SetLocalPosition(PhysicsObject* o, const nlVector3& p);

namespace
{
enum
{
    kNpBonesMax = 32,
    kNpBonesSlots = 10
};
struct NpBonePre
{
    PhysicsTransform* comp;
    PhysicsObject* sub;
    PhysicsObject* parent;
    nlMatrix4 node;   // inputs, compared before use
    nlMatrix4 xform;
    float prot[12];   // the parent's ODE rotation and position as read by the lane
    float ppos[3];
    nlMatrix4 world;  // the normalised world matrix (the old code's input to SetSubObjectTransform)
    float R[12];      // what SetRotation / SetPosition would write
    nlVector3 pos;
};
struct NpBonesPre
{
    const PhysicsCharacterBase* owner;
    const cPoseAccumulator* pose;
    int n;
    NpBonePre b[kNpBonesMax];
};
NpBonesPre sNpBones[kNpBonesSlots];
int sNpBonesMode = -1, sNpBonesBreak = 0;
unsigned long long sNpBonesChars, sNpBonesApplied, sNpBonesFallback, sNpBonesNoPre, sNpBonesVerified, sNpBonesWrong, sNpBonesSteps;

// bit-for-bit equality of n 32-bit words (floats compared as their bits, so -0 / +0 and NaN payloads count as different: stricter than ==); the
// library memcmp was most of the check's cost (a call per array)
inline bool NpWordsEqual(const void* a, const void* b, int n)
{
    const uint32_t* x = (const uint32_t*)a;
    const uint32_t* y = (const uint32_t*)b;
    uint32_t d = 0;
    for (int i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

// the rest of UpdatePose's loop body after the previous-position read, unchanged (the fallback)
inline void NpUpdateBoneOld(PhysicsCharacterBase* self, PhysicsCharacterBase::PhysicsBoneVolume* volume, cPoseAccumulator* pose, float heightOffset, bool offsetCentreOfMass,
    float centreOfMassHeight)
{
    nlMatrix4 worldMatrix = pose->GetNodeMatrix(volume->m_BoneIndex);
    nlMultMatrices(worldMatrix, volume->m_Transform, pose->GetNodeMatrix(volume->m_BoneIndex));

    if (offsetCentreOfMass)
    {
        worldMatrix.m43 -= centreOfMassHeight;
        worldMatrix.m43 += heightOffset;
    }

    nlMatrix4 normalizedMatrix = worldMatrix;
    for (int row = 0; row < 3; ++row)
    {
        nlVector3& matrixRow = *(nlVector3*)&normalizedMatrix.e[row * 4];
        nlVector3 basis;
        nlVec3Set(basis, matrixRow.x, matrixRow.y, matrixRow.z);
        nlVec3Scale(basis,
            nlRecipSqrt(nlVec3LengthSquared(basis), true));
        matrixRow.x = basis.x;
        matrixRow.y = basis.y;
        matrixRow.z = basis.z;
    }
    worldMatrix = normalizedMatrix;

    self->AdjustTransform(volume->m_TransformHandle, worldMatrix, offsetCentreOfMass != false);
}

// the lane's arithmetic for one bone (UpdatePose with offsetCentreOfMass false, then SetSubObjectTransform's two calls on a depth-1 parent)
inline bool NpBoneCompute(PhysicsCharacterBase* self, PhysicsCharacterBase::PhysicsBoneVolume* volume, cPoseAccumulator* pose, NpBonePre& b)
{
    nlMatrix4 worldMatrix = pose->GetNodeMatrix(volume->m_BoneIndex);
    nlMultMatrices(worldMatrix, volume->m_Transform, pose->GetNodeMatrix(volume->m_BoneIndex));

    nlMatrix4 normalizedMatrix = worldMatrix;
    for (int row = 0; row < 3; ++row)
    {
        nlVector3& matrixRow = *(nlVector3*)&normalizedMatrix.e[row * 4];
        nlVector3 basis;
        nlVec3Set(basis, matrixRow.x, matrixRow.y, matrixRow.z);
        nlVec3Scale(basis,
            nlRecipSqrt(nlVec3LengthSquared(basis), true));
        matrixRow.x = basis.x;
        matrixRow.y = basis.y;
        matrixRow.z = basis.z;
    }
    worldMatrix = normalizedMatrix;

    PhysicsTransform* comp = self->GetComponent(volume->m_TransformHandle);
    if (comp == 0)
        return false;
    PhysicsObject* sub = comp->m_SubObject;
    if (sub == 0)
        return false;
    PhysicsObject* par = sub->m_parentObject;
    if (par == 0 || par->m_parentObject != 0)
        return false;
    b.comp = comp;
    b.sub = sub;
    b.parent = par;
    memcpy(&b.node, &pose->GetNodeMatrix(volume->m_BoneIndex), sizeof(nlMatrix4));
    memcpy(&b.xform, &volume->m_Transform, sizeof(nlMatrix4));
    memcpy(&b.world, &worldMatrix, sizeof(nlMatrix4));
    memcpy(b.prot, NpSim26ObjRotation(par), sizeof(b.prot));
    memcpy(b.ppos, NpSim26ObjPosition(par), sizeof(b.ppos));
    NpSim26LocalRotation(worldMatrix, b.prot, b.R);
    nlVector3 position;
    float z = worldMatrix.e2[3][2];
    float y = worldMatrix.e2[3][1];
    float x = worldMatrix.e2[3][0];
    nlVec3Set(position, x, y, z);
    NpSim26LocalPosition(position, b.prot, b.ppos, &b.pos);
    return true;
}
} // namespace

int NpSim26BonesMode()
{
    if (sNpBonesMode < 0)
    {
        const char* e = getenv("MSC_SIM26_BONES");
        sNpBonesMode = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
        const char* k = getenv("MSC_SIM26_BONES_BREAK");
        sNpBonesBreak = (k && k[0] == '1') ? 1 : 0;
    }
    return sNpBonesMode;
}

void NpSim26BonesClear()
{
    for (int s = 0; s < kNpBonesSlots; s++)
        sNpBones[s].owner = 0;
}

void NpSim26BonesReport()
{
    if (++sNpBonesSteps % 300 != 0 || sNpBonesMode <= 0)
        return;
    printf("[sim26bones] mode=%d break=%d steps=%llu characters=%llu bones precomputed+written=%llu done the old way=%llu characters without a lane result=%llu verify bones=%llu wrong=%llu\n", sNpBonesMode,
        sNpBonesBreak, sNpBonesSteps, sNpBonesChars, sNpBonesApplied, sNpBonesFallback, sNpBonesNoPre, sNpBonesVerified, sNpBonesWrong);
    fflush(stdout);
}

int PhysicsCharacterBase::NpSim26BoneCount() const
{
    int n = 0;
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head; entry != 0; entry = entry->next)
        n++;
    return n;
}

// lane side: the character's pose is done; slot = its place in the lane list (one writer per slot)
void PhysicsCharacterBase::NpSim26Precompute(cPoseAccumulator* pose, int slot)
{
    if (slot < 0 || slot >= kNpBonesSlots)
        return;
    NpBonesPre& P = sNpBones[slot];
    P.owner = 0;
    P.pose = pose;
    int n = 0;
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head; entry != 0; entry = entry->next)
    {
        if (n == kNpBonesMax || !NpBoneCompute(this, entry->entry, pose, P.b[n]))
            return;
        if (sNpBonesBreak == 1 && (n & 3) == 0)
            P.b[n].pos.x += 0.0009765625f;   // proof mutant
        n++;
    }
    P.n = n;
    P.owner = this;
}

// pass 3 (game thread), in place of UpdatePose(pose, h, false) after a lane pose: false = nothing was done (the caller runs UpdatePose)
bool PhysicsCharacterBase::NpSim26Apply(cPoseAccumulator* pose)
{
    NpBonesPre* P = 0;
    for (int s = 0; s < kNpBonesSlots; s++)
        if (sNpBones[s].owner == this)
        {
            P = &sNpBones[s];
            break;
        }
    if (P == 0 || P->pose != pose)
    {
        sNpBonesNoPre++;
        return false;
    }
    P->owner = 0;
    int count = 0;
    // MSC_PREFETCH bit 16 (gt18): this walk already touches every list node, so it also starts the loads of what the bone loop below reads (the volumes, the lane's
    // results of this character: written by another core). A prefetch changes no value.
    const bool prefetchBones = NpPrefetchOn(16);
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head; entry != 0; entry = entry->next)
    {
        if (prefetchBones && count < P->n)
        {
            NpPrefetchLines(entry->entry, sizeof(PhysicsBoneVolume), 0);
            NpPrefetchLines(&P->b[count], sizeof(NpBonePre), 0);
        }
        count++;
    }
    if (count != P->n)
    {
        sNpBonesNoPre++;
        return false;
    }
    sNpBonesChars++;
    int k = 0;
    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head; entry != 0; entry = entry->next, k++)
    {
        PhysicsBoneVolume* volume = entry->entry;
        NpBonePre& b = P->b[k];
        if (prefetchBones && entry->next != 0)
        {
            // the next bone: its physics object and node matrix (the volume was prefetched by the walk above)
            PhysicsBoneVolume* nv = entry->next->entry;
            NpPrefetchLines(nv->m_pObject, 64, 0);
            NpPrefetchLines(&pose->GetNodeMatrix(nv->m_BoneIndex), sizeof(nlMatrix4), 0);
        }
        volume->m_PrevPosition = volume->m_pObject->GetPosition();

        PhysicsTransform* comp = GetComponent(volume->m_TransformHandle);
        bool ok = NpWordsEqual(&pose->GetNodeMatrix(volume->m_BoneIndex), &b.node, 16) && NpWordsEqual(&volume->m_Transform, &b.xform, 16)
                  && comp == b.comp && comp != 0 && comp->m_SubObject == b.sub && b.sub->m_parentObject == b.parent && b.parent->m_parentObject == 0;
        if (sNpBonesMode == 2)
        {
            // verify: the lane's results against the same arithmetic on this thread from the live inputs; the writes are the old code's
            if (ok)
            {
                float R2[12];
                nlVector3 p2;
                NpSim26LocalRotation(b.world, NpSim26ObjRotation(b.parent), R2);
                nlVector3 position;
                nlVec3Set(position, b.world.e2[3][0], b.world.e2[3][1], b.world.e2[3][2]);
                NpSim26LocalPosition(position, NpSim26ObjRotation(b.parent), NpSim26ObjPosition(b.parent), &p2);
                sNpBonesVerified++;
                if (!NpWordsEqual(R2, b.R, 12) || !NpWordsEqual(&p2, &b.pos, 3))
                    sNpBonesWrong++;
            }
            NpUpdateBoneOld(this, volume, pose, 0.0f, false, m_CentreOfMassHeight);
            sNpBonesFallback++;
            continue;
        }
        if (!ok)
        {
            NpUpdateBoneOld(this, volume, pose, 0.0f, false, m_CentreOfMassHeight);
            sNpBonesFallback++;
            continue;
        }
        // SetSubObjectTransform: SetRotation, then SetPosition, each on the parent's live ODE state
        if (NpWordsEqual(NpSim26ObjRotation(b.parent), b.prot, 12))
            NpSim26SetLocalRotation(b.sub, b.R);
        else
        {
            b.sub->SetRotation(b.world, PhysicsObject::WORLD_COORDINATES);
            sNpBonesFallback++;
        }
        const float* dp = NpSim26ObjPosition(b.parent);
        if (NpWordsEqual(NpSim26ObjRotation(b.parent), b.prot, 12) && NpWordsEqual(dp, b.ppos, 3))
        {
            nlVec3Set(b.parent->m_position, dp[0], dp[1], dp[2]);   // SetPosition's write of the parent's position
            NpSim26SetLocalPosition(b.sub, b.pos);
            sNpBonesApplied++;
        }
        else
        {
            nlVector3 position;
            float z = b.world.e2[3][2];
            float y = b.world.e2[3][1];
            float x = b.world.e2[3][0];
            nlVec3Set(position, x, y, z);
            b.sub->SetPosition(position, (PhysicsObject::CoordinateType)0);
            sNpBonesFallback++;
        }
    }
    return true;
}

void PhysicsCharacterBase::GetBonePositions(
    PhysicsBoneID id, nlVector3& currentPosition, nlVector3& previousPosition)
{
    nlVec3Set(currentPosition, 0.0f, 0.0f, 0.0f);
    nlVec3Set(previousPosition, 0.0f, 0.0f, 0.0f);

    for (ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head;
        entry != 0;
        entry = entry->next)
    {
        PhysicsBoneVolume* volume = entry->entry;
        if (id == volume->m_ID)
        {
            currentPosition = volume->m_pObject->GetPosition();
            previousPosition = volume->m_PrevPosition;
        }
    }
}

inline PhysicsCharacterBase::PhysicsBoneVolume* PhysicsCharacterBase::AddBoneVolume(
    PhysicsObject* object, unsigned int boneIndex, unsigned int transformHandle,
    const nlMatrix4& transform, PhysicsBoneID id)
{
    return new (nlMalloc(sizeof(PhysicsBoneVolume), 8, false))
        PhysicsBoneVolume(object, boneIndex, transformHandle, transform, id);
}

void PhysicsCharacterBase::AddBoneVolumes(PhysicsWorld* world,
    CollisionSpace* collisionSpace, cPoseAccumulator* pose,
    const CharacterPhysicsData* data, unsigned long categoryBits,
    unsigned long collideBits)
{
    for (unsigned int i = 0; i < data->physicsElementCount; ++i)
    {
        CharacterPhysicsElement* element = &data->pPhysicsElements[i];
        PhysicsObject* object = 0;

        switch (element->uPrimitiveType)
        {
        case PHYS_PRIMITIVE_SPHERE:
            object = new (nlMalloc(sizeof(PhysicsSphereBone), 8, false))
                PhysicsSphereBone(collisionSpace, world, element->fRadius);
            break;
        case PHYS_PRIMITIVE_CAPSULE:
            object = new (nlMalloc(sizeof(PhysicsCapsuleBone), 8, false))
                PhysicsCapsuleBone(collisionSpace, world, element->fRadius, element->fHeight);
            break;
        case PHYS_PRIMITIVE_CYLINDER:
            object = new (nlMalloc(sizeof(PhysicsCylinderBone), 8, false))
                PhysicsCylinderBone(collisionSpace, world, element->fRadius, element->fHeight);
            break;
        default:
            continue;
        }

        object->SetCategory(categoryBits);
        object->SetCollide(collideBits);

        unsigned int parentNodeIndex = pose->m_BaseSHierarchy->GetNodeIndexByID(element->uParentHashID);
        int transformHandle = AddObject(object);
        PhysicsBoneID id = ResolvePhysicsBoneIDFromName(element->szName);
        PhysicsBoneVolume* volume = AddBoneVolume(object, parentNodeIndex, transformHandle, element->matLocalToParent, id);
        m_BoneVolumes.AddEntry(volume);

        switch (element->uPrimitiveType)
        {
        case PHYS_PRIMITIVE_SPHERE:
            ((PhysicsSphereBone*)object)->m_pBoneVolume = volume;
            break;
        case PHYS_PRIMITIVE_CAPSULE:
            ((PhysicsCapsuleBone*)object)->m_pBoneVolume = volume;
            break;
        case PHYS_PRIMITIVE_CYLINDER:
            ((PhysicsCylinderBone*)object)->m_pBoneVolume = volume;
            break;
        }
    }
}

PhysicsBoneID PhysicsCharacterBase::GetBoneIDForSubObject(
    const PhysicsObject* object) const
{
    switch (object->GetObjectType())
    {
    case PHYSOBJ_SPHERE_BONE:
    {
        const PhysicsSphereBone* bone = (const PhysicsSphereBone*)object;
        if (bone->m_pBoneVolume != 0)
        {
            return bone->m_pBoneVolume->m_ID;
        }
        break;
    }
    case PHYSOBJ_CAPSULE_BONE:
    {
        const PhysicsCapsuleBone* bone = (const PhysicsCapsuleBone*)object;
        if (bone->m_pBoneVolume != 0)
        {
            return bone->m_pBoneVolume->m_ID;
        }
        break;
    }
    case PHYSOBJ_CYLINDER_BONE:
    {
        const PhysicsCylinderBone* bone = (const PhysicsCylinderBone*)object;
        if (bone->m_pBoneVolume != 0)
        {
            return bone->m_pBoneVolume->m_ID;
        }
        break;
    }
    }
    return PHYSBONE_SINGLE_CAPSULE;
}

void PhysicsCharacterBase::SetBoneVolumeScale(float scale)
{
    PhysicsBoneVolume* volume;
    ListEntry<PhysicsBoneVolume*>* entry = m_BoneVolumes.m_Head;
    while (entry != 0)
    {
        volume = entry->entry;
        switch (volume->m_pObject->GetObjectType())
        {
        case PHYSOBJ_SPHERE_BONE:
        {
            ((PhysicsSphereBone*)volume->m_pObject)
                ->SetRadius(scale * ((PhysicsSphereBone*)volume->m_pObject)->m_Radius);
            break;
        }
        case PHYSOBJ_CAPSULE_BONE:
        {
            PhysicsCapsuleBone* capsule = (PhysicsCapsuleBone*)volume->m_pObject;
            capsule->SetRadius(scale * capsule->m_Radius);
            capsule->SetLength(scale * capsule->m_Length);
            break;
        }
        case PHYSOBJ_CYLINDER_BONE:
        {
            PhysicsCylinderBone* cylinder = (PhysicsCylinderBone*)volume->m_pObject;
            cylinder->SetRadius(scale * cylinder->m_Radius);
            cylinder->SetLength(scale * cylinder->m_Length);
            break;
        }
        }
        entry = entry->next;
    }
}

template <>
void ListContainerBase<PhysicsCharacterBase::PhysicsBoneVolume*,
    NewAdapter<ListEntry<PhysicsCharacterBase::PhysicsBoneVolume*> > >::DeleteEntry(ListEntry<PhysicsCharacterBase::PhysicsBoneVolume*>* entry)
{
    NewAdapter<ListEntry<PhysicsCharacterBase::PhysicsBoneVolume*> >::DeleteEntry(
        entry);
}

PhysicsCharacterBase::PhysicsSphereBone::~PhysicsSphereBone()
{
}

PhysicsCharacterBase::PhysicsCapsuleBone::~PhysicsCapsuleBone()
{
}

PhysicsCharacterBase::PhysicsCylinderBone::~PhysicsCylinderBone()
{
}

int PhysicsCharacterBase::GetObjectType() const
{
    return PHYSOBJ_CHARACTER;
}

int PhysicsCharacterBase::PhysicsCylinderBone::GetObjectType() const
{
    return PHYSOBJ_CYLINDER_BONE;
}

int PhysicsCharacterBase::PhysicsCapsuleBone::GetObjectType() const
{
    return PHYSOBJ_CAPSULE_BONE;
}

int PhysicsCharacterBase::PhysicsSphereBone::GetObjectType() const
{
    return PHYSOBJ_SPHERE_BONE;
}

BoneVolumeTypeState s_BoneVolumeType = { 0xFFFF, 0 };
char s_BoneIDName[5] = "m_ID";
char s_BoneVolumeName[8] = "BoneVol";
