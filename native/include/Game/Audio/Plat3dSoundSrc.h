#ifndef GAME_AUDIO_PLAT3DSOUNDSRC_H
#define GAME_AUDIO_PLAT3DSOUNDSRC_H

#include "port/bitfield.h"
#include "Game/Audio/AudioSystem.h"
#include "Game/Audio/XSoundHandle.h"

class PlatAudioListener;

extern float sSpeedOfSound;

class Plat3dSoundSrc : public XSoundOwner
{
public:
    virtual inline ~Plat3dSoundSrc();
    virtual void Update(PlatAudioListener* listener, float deltaTime);

    /* 0x20 */ float m_Pan;
    /* 0x24 */ float m_SurroundPan;
    /* 0x28 */ nlVector3 m_PrevPosition;
    /* 0x34 */ nlVector3 m_Velocity;
    /* 0x40 */ float m_DopplerPitch;
    /* 0x44 */ union
    {
        u32 m_SpatialBits;
        // NP: m_SpatialBits is masked as a whole word elsewhere (|= 0x00800000, >> 24); reversed on
        // little-endian targets so the named fields are the same bits (port/bitfield.h).
        struct
        {
#if NP_BITFIELDS_LSB_FIRST
            u32 m_UnknownFlags09 : 23;
            u32 m_SquareRootPan : 1;
            u32 m_InterauralDelay : 8;
#else
            u32 m_InterauralDelay : 8;
            u32 m_SquareRootPan : 1;
            u32 m_UnknownFlags09 : 23;
#endif
        } m_Spatial;
    };
};

class PlatAudioListener : public AudioListener
{
public:
    virtual void Update(float deltaTime);

    /* 0x2C */ nlVector3 m_Right;
    /* 0x38 */ nlVector3 m_Velocity;
    /* 0x44 */ nlVector3 m_PrevPosition;
};

#endif // GAME_AUDIO_PLAT3DSOUNDSRC_H
