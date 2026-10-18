#ifndef GAME_PHYSICS_PHYSICS_H
#define GAME_PHYSICS_PHYSICS_H

#include "Game/Physics/PhysicsWorld.h"
#include "NL/nlListContainer.h"

class CollisionSpace;
class PhysicsMesh;
class PhysicsObject;

void InitializeODEAllocators();
void RegisterUserGeomClasses();
void CreatePhysicsWorld();
void CreateStaticPhysicsPrimitives();
void DestroyStaticPhysicsPrimitives();
void DestroyPhysicsWorld();
void PhysicsUpdate(PhysicsWorld*, float);
void ODEFree(void*, size_t);
void* ODERealloc(void*, size_t, size_t);
void* ODEAlloc(size_t);

extern PhysicsMesh* g_TerrainMesh;
extern PhysicsWorld* g_PhysicsWorld;
extern CollisionSpace* g_CollisionSpace;
extern nlListContainer<PhysicsObject*> g_StaticPhysicsPrimitives;
extern nlListContainer<PhysicsObject*> g_NetPhysicsObjects;

#endif // GAME_PHYSICS_PHYSICS_H
