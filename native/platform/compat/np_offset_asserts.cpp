// Compile-time checks of the places where the decomp reads game objects through Wii BYTE OFFSETS instead of members (PhysicsEventQueue's event
// handlers, the banner captain in StadiumWorldDrawable, the prop and packet flags of CharacterTriggers). np builds ILP32 (host32, armhf, Vita), so those offsets are right as long as the object layout
// equals the Wii's; nothing said so. A layout drift (a member added or retyped upstream, a target whose pointer or double alignment differs) now stops
// the build here instead of reading another field. The values are the Wii offsets the handlers use.
//
// Found by the PC port (a 64-bit build reads other fields at these offsets and crashed in Birdo's egg, wrong banners): yannicksuter/mscharged-port
// (CC0), patches 0699 and 0703 (the list of sites). The asserts are new.
#include <stddef.h>

#include "Game/AI/Fielder.h"
#include "Game/AI/Powerups.h"
#include "Game/Character.h"
#include "Game/EventDataTypes.h"
#include "Game/Physics/PhysicsPatch.h"
#include "Game/Render/BirdoEgg.h"
#include "Game/Render/KoopaShellObject.h"

#define NP_OFFSET(T, member, off) static_assert(offsetof(T, member) == (off), #T "::" #member ": not at the Wii offset " #off " the decomp reads it at")

// HandleCollisionFireballPowerup / HandleDestroyPowerup / HandleCollisionHammerPowerup: the PowerupBase behind a void*
NP_OFFSET(PowerupBase, m_bShouldDestroy, 0x04);
NP_OFFSET(PowerupBase, m_eType, 0x1C);
NP_OFFSET(PowerupBase, meSize, 0xA4);
// HandleCollisionWaluigiWall / HandleCollisionCrackEgg / StadiumWorldDrawable::GetCaptainCharacter: the cFielder behind a void*
static_assert(offsetof(cFielder, m_DetChar) == 0x24, "cFielder::m_DetChar: not at the Wii offset 0x24");
static_assert(offsetof(DetChar, m_eCharacterClass) == 0x00, "DetChar::m_eCharacterClass");
static_assert(offsetof(cFielder, m_DetChar) + offsetof(DetChar, m_aActualFacingDirection) == 0x62, "cFielder facing direction: not at the Wii offset 0x62");
NP_OFFSET(cFielder, m_eClassType, 0xF0);
// CharacterTriggers.cpp: SetCharacterPacketAVisible / PacketBVisible / LeftPropAnimated / RightPropAnimated write single bytes of a cCharacter
NP_OFFSET(cCharacter, m_bLeftPropAnimated, 0x17E);
NP_OFFSET(cCharacter, m_bRightPropAnimated, 0x17F);
NP_OFFSET(cCharacter, m_bPacketAVisible, 0x181);
NP_OFFSET(cCharacter, m_bPacketBVisible, 0x182);
// the event data structs
NP_OFFSET(CollisionEggData, pFielder, 0x04);
NP_OFFSET(CollisionKoopaShellEndData, cracked, 0x04);
NP_OFFSET(CollisionBirdoEggEndData, cracked, 0x04);
NP_OFFSET(CollisionPatchData, pPatch, 0x10);
NP_OFFSET(KoopaShellObject, mOwner, 0x2C);
NP_OFFSET(BirdoEggObject, mShooter, 0x3C);
NP_OFFSET(PhysicsPatch, m_Type, 0x48);
NP_OFFSET(PhysicsPatch, m_bKillMe, 0x65);
