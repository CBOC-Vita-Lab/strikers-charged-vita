#ifndef GAME_AUDIO_XSOUND_HANDLE_H
#define GAME_AUDIO_XSOUND_HANDLE_H

#include "port/bitfield.h"
#include "NL/nlMath.h"
#include "types.h"

enum eSoundHandleState
{
    SOUND_HANDLE_INITIAL = 0,
    SOUND_HANDLE_PENDING = 1,
    SOUND_HANDLE_PREPARING = 2,
    SOUND_HANDLE_PREPARED = 3,
    SOUND_HANDLE_PLAYING = 4,
    SOUND_HANDLE_PAUSED = 5,
    SOUND_HANDLE_FAILED = 6,
    SOUND_HANDLE_STOPPING = 7,
    SOUND_HANDLE_STOPPED = 8,
    SOUND_HANDLE_RELEASED = 9,
};

struct XSoundOwner
{
    XSoundOwner()
    {
        count.references = 0;
        nlVec3Set(m_Position.m_Value, 0.0f, 0.0f, 0.0f);
        count.positionIsPointer = count.updatePending = count.zeroVelocity = count.field_1000 = false;
    }

    virtual ~XSoundOwner() { }

    void SetPosition(const nlVector3* position)
    {
        m_Position.m_Pointer = position;
        m_ReferencesAndFlags |= 0x8000;
    }

    const nlVector3& GetPosition() const
    {
        return count.positionIsPointer ? *m_Position.m_Pointer : m_Position.m_Value;
    }

    /* 0x04 */ union
    {
        nlVector3 m_Value;
        const nlVector3* m_Pointer;
    } m_Position;
    /* 0x10 */ float m_Distance;
    /* 0x14 */ float m_ScaledPan;
    /* 0x18 */ float m_Unknown18;
    /* 0x1C */ union
    {
        u32 m_ReferencesAndFlags;
        // NP: m_ReferencesAndFlags is masked as a whole word elsewhere (|= 0x8000, >> 12); the declaration
        // order is reversed on little-endian targets so the named fields are the same bits (port/bitfield.h).
        struct
        {
#if NP_BITFIELDS_LSB_FIRST
            u32 field_0FFF : 12;
            u32 field_1000 : 1;
            u32 zeroVelocity : 1;
            u32 updatePending : 1;
            u32 positionIsPointer : 1;
            s32 references : 16;
#else
            s32 references : 16;
            u32 positionIsPointer : 1;
            u32 updatePending : 1;
            u32 zeroVelocity : 1;
            u32 field_1000 : 1;
            u32 field_0FFF : 12;
#endif
        } count;
    };
};

class XSoundHandle;
class AudioSlider;
struct AudioSliderSet;
typedef void (*XSoundHitMarkerCallback)(
    void*, XSoundHandle*, void*);

class XSoundHandle
{
public:
    XSoundHandle(void* value1, XSoundOwner* owner,
        void* value2, XSoundHitMarkerCallback callback,
        void* callbackContext);
    virtual ~XSoundHandle();

    virtual bool Play(bool) = 0;
    virtual bool Prepare(bool) = 0;
    virtual void Stop(u8, void*) = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void SetCallbackEnabled(u8 enabled);
    virtual int IsCallbackEnabled();
    virtual void Release() = 0;
    virtual void Update(float dt);
    virtual bool IsValid() = 0;
    virtual void SetCue(unsigned int** slot, unsigned int cueIndex);

    void FormatState(char* buffer, u32 size);
    void OnHitMarker(void* value);
    void PrintState();

    u32** m_Slot;
    u32 m_CueIndex;
    eSoundHandleState m_State;
    u8 m_CallbackEnabled;
    u8 m_Unknown11[3];
    XSoundOwner* m_Owner;
    float m_PreviousTime;
    float m_CurrentTime;
    AudioSliderSet* m_LocalSliders;
    XSoundHitMarkerCallback m_Callback;
    void* m_CallbackContext;
};

AudioSlider* GetSoundParameter(XSoundHandle* handle, unsigned long index);

#endif // GAME_AUDIO_XSOUND_HANDLE_H
