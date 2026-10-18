#include "Game/NetworkInput.h"

#include <string.h>

#include "Game/InputManager.h"
#include "Game/NetworkSession.h"
#include "Game/PackedDetInput.h"
#include "NL/globalpad.h"
#include "platform/sdk/net/np_lan.h"  // multiplayer (np/patches/mp)
#include "platform/sdk/input/np_game_input.h"  // controls (np/platform/sdk/input)

NetworkPeerChannel* NetworkPeer::GetNetworkPeerChannel(int channel)
{
    return &mChannels[channel];
}

s8 NetworkPeer::GetNetworkPeerMachineId()
{
    return mMachineId;
}

NetworkPeer::NetworkPeer()
    : mMachineId(-1)
    , mPlayerCount(0)
{
}

void NetworkPeer::ResetNetworkPeerInputs()
{
    for (int channel = 0; channel < (int)mPlayerCount; ++channel)
    {
        NetworkPeerChannel* entry = &mChannels[channel];
        entry->mPrevInput.Reset();
        entry->mInput.Reset();
        entry->mPrevLocalInput.Reset();
        entry->mLocalInput.Reset();
        mChannels[channel].mPrevInput.m_pMyUser = entry;
        mChannels[channel].mInput.m_pMyUser = entry;
        mChannels[channel].mPrevLocalInput.m_pMyUser = entry;
        mChannels[channel].mLocalInput.m_pMyUser = entry;
        mChannels[channel].mInput.m_pPrevInput = &entry->mPrevInput;
        mChannels[channel].mLocalInput.m_pPrevInput = &entry->mPrevLocalInput;
    }
}

NetworkPeerChannel::NetworkPeerChannel()
    : mPeer(0)
    , mChannelIndex(-1)
    , mGlobalPadIndex(0)
{
    mPrevInput.Reset();
    mInput.Reset();
    mPrevLocalInput.Reset();
    mLocalInput.Reset();
    mPrevInput.m_pMyUser = this;
    mInput.m_pMyUser = this;
    mPrevLocalInput.m_pMyUser = this;
    mLocalInput.m_pMyUser = this;
    mInput.m_pPrevInput = &mPrevInput;
    mLocalInput.m_pPrevInput = &mPrevLocalInput;
}

void NetworkPeerChannel::Initialize(NetworkPeer* peer, s8 channelIndex, int globalPadIndex)
{
    mPeer = peer;
    mChannelIndex = channelIndex;
    mGlobalPadIndex = globalPadIndex;
    mPadding00C = false;
}

DetInput* NetworkPeerChannel::GetNetworkPeerChannelInput()
{
    return &mInput;
}

s8 NetworkPeerChannel::GetNetworkPeerChannelId()
{
    return mPeer->mMachineId * 4 + mChannelIndex;
}

cGlobalPad* NetworkPeerChannel::GetLocalChannelPad()
{
    if (GetGlobalPadIndex() == -1)
    {
        return 0;
    }
    cGlobalPad* pad;
    if (mPeer == g_pNetworkSessionBase->GetLocalPeer())
        pad = g_pPadManager->GetPad(GetGlobalPadIndex());
    else
        pad = 0;
    return pad;
}

void NetworkPeerChannel::CaptureNetworkPeerChannelInput()
{
    mPrevLocalInput.CopyState(mLocalInput);
    cGlobalPad* pad = this->GetLocalChannelPad();
    if (pad != 0 && !gInputManager->mFrameProvider->IsInPauseMenu())
    {
        mLocalInput.ReadFromPad(pad);
    }
    else
    {
        mLocalInput.Reset();
    }
    mLocalInput.m_aRemapAngle
        = gInputManager->mFrameProvider->GetInputRemapAngle();
    NpInputAfterCapture(this);       // controls: the Vita pad becomes the engine's GameCube controls here (np/platform/sdk/input)
    NpLanAfterChannelCapture(this);  // multiplayer: the record that goes on the wire is normalized here
}

void NetworkPeerChannel::ApplyNetworkPeerChannelInput(const PackedDetInput* record, u16 remapAngle, u8 connected)
{
    mPrevInput.CopyState(mInput);
    UnpackDetInput(record, &mInput);
    mInput.m_aRemapAngle = remapAngle;
    mInput.m_nConnected = connected;
    mInput.UpdatePolarAnalog();
    mInput.UpdateButtonStateTicks();
}

void NetworkPeerChannel::PackNetworkPeerChannelInput(PackedDetInput* record)
{
    PackDetInput(record, &mLocalInput);
}

u16 NetworkPeerChannel::GetNetworkPeerChannelRemapAngle()
{
    return mLocalInput.m_aRemapAngle;
}

u8 NetworkPeerChannel::GetNetworkPeerChannelConnectionStatus()
{
    return mLocalInput.GetConnectionStatus();
}

s8 GetNetworkPlayerId(s8 player, s8 machine)
{
    return machine * 4 + player;
}

NetMessageInput::NetMessageInput()
{
    mChecksum = 0;
    mFrame = 0;
    mRandomSeed = 0;
    mPlayerStates = 0;
    memset(mRecordChanges, 0, sizeof(mRecordChanges));
    mEventCount = 0;
    mFlags = 0;
    mRemapAngleHigh = 0;
    memset(mRecords, 0, sizeof(mRecords));
}

void NetMessageInput::Reset(bool clearInputs, bool clearHeader)
{
    mChecksum = 0;
    mFrame = 0;
    mRandomSeed = 0;
    mPlayerStates = 0;
    if (clearHeader)
    {
        memset(mRecordChanges, 0,
            sizeof(mRecordChanges));
        mEventCount = 0;
        mFlags = 0;
    }
    else
    {
        mFlags &= 1;
    }
    if (clearInputs)
    {
        mRemapAngleHigh = 0;
        memset(mRecords, 0,
            sizeof(mRecords));
    }
}

void NetMessageInput::CopyFrom(const NetMessageInput* source)
{
    mFlags = source->mFlags;
    mRemapAngleHigh = source->mRemapAngleHigh;
    mPlayerStates = source->mPlayerStates;
    mChecksum = source->mChecksum;
    mFrame = source->mFrame;
    mRandomSeed = source->mRandomSeed;
    mEventCount = source->mEventCount;
    memcpy(mRecordChanges, source->mRecordChanges, 4);
    memcpy(mRecords, source->mRecords, 0x40);
    memcpy(mEvents, source->mEvents, 0x90);
}

void NetMessageInput::SetNetworkInputMessagePlayerState(s8 player, u8 state)
{
    u8 shift = (player & 0x7F) * 2;
    u32 mask = 3 << shift;
    u8 inverse = ~mask;
    u8 value = mPlayerStates & inverse;
    mPlayerStates = value | (state << shift);
}

u8 NetMessageInput::GetNetworkInputMessagePlayerState(s8 player)
{
    return (mPlayerStates >> ((player & 0x7F) * 2)) & 3;
}

// Multiplayer (np/patches/mp): the whole 16-byte record travels. The retail message carries 13 of the 16 bytes per
// changed player (no analog triggers in byte 2, no pointer in bytes 9-11), which the Vita controls use. Both consoles
// run this build, so nothing is lost; the change bits keep their meaning (any difference sets all of them) and the
// receiver copies the whole record.
void NetMessageInput::SetNetworkInputMessageRecord(s8 player, const PackedDetInput* record)
{
    const u8* source = (const u8*)record;

    if (memcmp(source, mRecords[player].mData, 0x10) != 0)
        mRecordChanges[player] |= 0x7E;

    if (mRecordChanges[player] != 0)
        mFlags |= 0x10 << player;
    memcpy(mRecords[player].mData, source, 0x10);
}

void NetMessageInput::ApplyNetworkInputMessageRecord(s8 player, PackedDetInput* record)
{
    if (mRecordChanges[player] != 0)
        memcpy(record, mRecords[player].mData, 0x10);
}

void NetMessageInput::SetNetworkInputMessageRemapAngle(u16 remapAngle)
{
    u8 value = remapAngle >> 8;
    if (mRemapAngleHigh != value)
    {
        mRemapAngleHigh = value;
        mFlags |= 1;
    }
}

void NetMessageInput::GetNetworkInputMessageRemapAngle(u16* remapAngle)
{
    if (mFlags & 1)
        *remapAngle = mRemapAngleHigh << 8;
}

void NetMessageInput::SetNetworkInputMessageSyncData(u32 checksum, u32 frame, u32 randomSeed)
{
    mChecksum = checksum;
    mFrame = frame;
    mRandomSeed = randomSeed;
    mFlags |= 4;
}

void NetMessageInput::SetNetworkInputMessageCongested(bool congested)
{
    if (congested)
        mFlags |= 8;
    else
        mFlags &= ~8;
}

void NetMessageInput::AddNetworkInputMessageEvent(DetermDataEvent* event)
{
    if (mEventCount < 4)
    {
        mEvents[mEventCount] = *event;
        ++mEventCount;
    }
}

DetermDataEvent* NetMessageInput::GetNetworkInputMessageEvent(int index)
{
    return &mEvents[index];
}

namespace
{
// Multiplayer: the retail serializer trusted every count and size of a received message. This one stops at the end
// of the buffer and refuses an event count above 4 or an event larger than its storage.
struct InputSerializerGuard
{
    NetworkMessageSerializer* mSerializer;
    bool mOk;

    void Transfer(void* value, unsigned long size)
    {
        if (!mOk)
            return;
        if (mSerializer->mDirection == 0 && mSerializer->mEnd != 0 && mSerializer->mPosition + size > mSerializer->mEnd)
        {
            mOk = false;
            return;
        }
        mSerializer->Transfer(value, size);
    }
};
}

void NetMessageInput::Serialize(
    NetworkMessageSerializer* serializer)
{
    InputSerializerGuard io = { serializer, true };
    io.Transfer(&mFlags, 1);
    io.Transfer(&mPlayerStates, 1);
    if (mFlags & 1)
        io.Transfer(&mRemapAngleHigh, 1);
    if (mFlags & 4)
    {
        io.Transfer(&mChecksum, 4);
        io.Transfer(&mFrame, 4);
        io.Transfer(&mRandomSeed, 4);
    }
    else if (mFlags & 2)
    {
        io.Transfer(&mFrame, 4);
    }
    io.Transfer(&mEventCount, 1);
    if (io.mOk && mEventCount > 4)
    {
        io.mOk = false;
        mEventCount = 0;
    }
    for (int i = 0; i < mEventCount && io.mOk; ++i)
    {
        io.Transfer(&mEvents[i].mSize, 1);
        if (mEvents[i].mSize > sizeof(mEvents[i].mData))
        {
            io.mOk = false;
            break;
        }
        io.Transfer(mEvents[i].mData, mEvents[i].mSize);
    }
    for (s8 i = 0; i < 4; ++i)
    {
        if (mFlags & (0x10 << i))
            io.Transfer(&mRecordChanges[i], 1);
    }
    for (s8 i = 0; i < 4; ++i)
    {
        if (mRecordChanges[i] != 0)
            io.Transfer(mRecords[i].mData, 0x10);
    }
    if (!io.mOk && serializer->mDirection == 0)
    {
        // a malformed message from the other console: apply nothing from it
        mEventCount = 0;
        memset(mRecordChanges, 0, sizeof(mRecordChanges));
        NpLanLog("malformed input message rejected\n");
    }
}

void NetMessageInputBundle::Serialize(
    NetworkMessageSerializer* serializer)
{
    mMessage0.Serialize(serializer);
    mMessage1.Serialize(serializer);
}

int NetMessageInputBundle::GetType()
{
    return NETMSG_INPUT_BUNDLE;
}

int NetMessageInput::GetType()
{
    return NETMSG_INPUT;
}

typedef char VerifyNetworkPeerChannelSize[
    sizeof(NetworkPeerChannel) == 0x240 ? 1 : -1];
typedef char VerifyNetworkPeerSize[
    sizeof(NetworkPeer) == 0x908 ? 1 : -1];
