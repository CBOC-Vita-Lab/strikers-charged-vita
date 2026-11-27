#include "Game/LANMessages.h"

// np: the sizes below come off the wire of a console nobody vetted (any console on the same Wi-Fi or in radio range, with the public fingerprint) and index fields of 8
// bytes and 7 entries: a larger value would write past the message. Refused like a truncated message (mFailed: MessageRegistry::Dispatch discards it).
static inline void TransferUserMatchData(
    NetworkMessageSerializer* serializer, u8* data, u8* size, unsigned long capacity)
{
    if (serializer->mDirection == NETWORK_SERIALIZE_READ && *size > capacity)
    {
        serializer->mFailed = true;
        *size = 0;
        return;
    }
    serializer->Transfer(data, *size);
}

static inline void SerializeLANPeerMessageInfo(
    NetworkMessageSerializer* serializer, LANPeerMessageInfo& info)
{
    serializer->Transfer(info.mAddress, sizeof(info.mAddress));
    serializer->Transfer(&info.mPort, sizeof(info.mPort));
    serializer->Transfer(info.mName, sizeof(info.mName));
    serializer->Transfer(&info.mPeerIndex, sizeof(info.mPeerIndex));
    serializer->Transfer(&info.mUserMatchDataSize, sizeof(info.mUserMatchDataSize));
    TransferUserMatchData(serializer, info.mUserMatchData, &info.mUserMatchDataSize, sizeof(info.mUserMatchData));
}

NetMessageJoinRequest::NetMessageJoinRequest()
    : mUserMatchDataSize(0)
{
}

void NetMessageJoinRequest::Serialize(NetworkMessageSerializer* serializer)
{
    serializer->Transfer(mAddress, sizeof(mAddress));
    serializer->Transfer(&mPort, sizeof(mPort));
    serializer->Transfer(mName, sizeof(mName));
    serializer->Transfer(&mUserMatchDataSize, sizeof(mUserMatchDataSize));
    TransferUserMatchData(serializer, mUserMatchData, &mUserMatchDataSize, sizeof(mUserMatchData));
}

NetMessageJoinResponse::NetMessageJoinResponse()
    : mUserMatchDataSize(0)
{
}

void NetMessageJoinResponse::Serialize(NetworkMessageSerializer* serializer)
{
    serializer->Transfer(mAddress, sizeof(mAddress));
    serializer->Transfer(&mPort, sizeof(mPort));
    serializer->Transfer(&mAccepted, sizeof(mAccepted));
    serializer->Transfer(mName, sizeof(mName));
    serializer->Transfer(&mUserMatchDataSize, sizeof(mUserMatchDataSize));
    TransferUserMatchData(serializer, mUserMatchData, &mUserMatchDataSize, sizeof(mUserMatchData));
    serializer->Transfer(&mPeerCount, sizeof(mPeerCount));
    if (serializer->mDirection == NETWORK_SERIALIZE_READ && mPeerCount > 6)   // the joiner's mPeerInfoList holds 8: the host's record, these, its own
    {
        serializer->mFailed = true;
        mPeerCount = 0;
        return;
    }
    for (u8 index = 0; index < mPeerCount; ++index)
    {
        SerializeLANPeerMessageInfo(
            serializer, mPeers[index]);
    }
}

NetMessageGamePeerAdded::NetMessageGamePeerAdded()
{
}

void NetMessageGamePeerAdded::Serialize(NetworkMessageSerializer* serializer)
{
    SerializeLANPeerMessageInfo(serializer, mPeer);
}

void NetMessageTransportType6::Serialize(NetworkMessageSerializer* serializer)
{
}

void NetMessageReadyToLaunchRequest::Serialize(NetworkMessageSerializer* serializer)
{
}

void NetMessageReadyToLaunchConfirm::Serialize(NetworkMessageSerializer* serializer)
{
    serializer->Transfer(&mConfirmed, sizeof(mConfirmed));
}

void NetMessageClientConfirmedJoin::Serialize(NetworkMessageSerializer* serializer)
{
    serializer->Transfer(&mMachineIndex, sizeof(mMachineIndex));
}

NetMessageTransportType6::~NetMessageTransportType6()
{
}

int NetMessageClientConfirmedJoin::GetType()
{
    return NETMSG_CLIENT_CONFIRMED_JOIN;
}

int NetMessageReadyToLaunchConfirm::GetType()
{
    return NETMSG_READY_TO_LAUNCH_CONFIRM;
}

int NetMessageReadyToLaunchRequest::GetType()
{
    return NETMSG_READY_TO_LAUNCH_REQUEST;
}

int NetMessageTransportType6::GetType()
{
    return 6;
}

int NetMessageGamePeerAdded::GetType()
{
    return NETMSG_GAME_PEER_ADDED;
}

int NetMessageJoinResponse::GetType()
{
    return NETMSG_JOIN_RESPONSE;
}

int NetMessageJoinRequest::GetType()
{
    return NETMSG_JOIN_REQUEST;
}
