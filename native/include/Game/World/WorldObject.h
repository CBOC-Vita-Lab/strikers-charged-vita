#ifndef GAME_WORLD_WORLD_OBJECT_H
#define GAME_WORLD_WORLD_OBJECT_H

class nlMatrix4;
class World;
struct WorldObjectLoadContext;

// Polymorphic base of the objects World creates from its object stream.
// Every derived vtable shares the slot 0x18 body at 0x80278E94, which
// World::InitializeObjects calls with the owning world. Only the slot
// layout is known; derived classes keep their own data.
class WorldObject
{
public:
    virtual ~WorldObject() { }
    // NP: pure (retail's vtables prove the base has no bodies); declared-only virtuals left the base vtable with undefined entries
    virtual void ReleaseResources() = 0;
    virtual nlMatrix4* GetWorldMatrix() = 0;
    virtual void SetWorldMatrix(const nlMatrix4& transform) = 0;
    virtual void OnWorldLoaded(World* world);
};

#endif // GAME_WORLD_WORLD_OBJECT_H
