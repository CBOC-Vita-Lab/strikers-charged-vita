#include "Game/NetworkMessageRegistry.h"
#include "Game/Sys/debug.h"
#include "NL/nlMemory.h"
#include "platform/sdk/net/np_lan.h"  // np (STR-144)


MessageRegistry* gNetworkMessageRegistry;

void InitializeNetworkMessageRegistry()
{
    gNetworkMessageRegistry = new (8, false) MessageRegistry;
}

int MessageRegistry::Serialize(
    NetworkMessage* message, u8* buffer, unsigned long size)
{
    NetworkMessageSerializer serializer(NETWORK_SERIALIZE_WRITE, buffer, size);
    u8 type = message->GetType();
    memcpy(serializer.mPosition, &type, sizeof(type));
    serializer.mPosition += sizeof(type);
    message->Serialize(&serializer);
    return serializer.GetLength();
}

void MessageRegistry::Dispatch(
    int source, u8* buffer, unsigned long size)
{
    if (buffer == 0 || size < sizeof(u8))
    {
        return;   // np (STR-144): an empty datagram carries no type byte
    }
    NetworkMessageSerializer serializer(NETWORK_SERIALIZE_READ, buffer, size);
    u8 type = 0;
    memcpy(&type, serializer.mPosition, sizeof(type));
    serializer.mPosition += sizeof(type);

    NetworkMessageFactoryBase* factory = mFactories[type];
    if (factory == 0)
    {
        tDebugPrintManager::Print(DC_NETWORK,
            "WARNING NO MESSAGE <FACTORY> FOR TYPE %d..DISCARDING MESSAGE\n",
            type);
        return;
    }

    NetworkMessage* message = factory->Create(&serializer);
    if (serializer.mFailed)
    {
        // np (STR-144): the message claims more than the datagram holds. Nothing from it is delivered.
        NpLanLog("malformed network message type %d (%lu bytes) discarded\n", type, size);
        delete message;
        return;
    }
    message->mSource = source;
    NetworkMessageReceiver* receiver = mReceivers[type];
    if (receiver != 0)
    {
        if (receiver->ProcessMessage(message))
        {
            delete message;
        }
    }
    else
    {
        tDebugPrintManager::Print(DC_NETWORK,
            "WARNING NO MESSAGE HANDLER FOR TYPE %d..DISCARDING MESSAGE\n",
            type);
        delete message;
    }
}

void MessageRegistry::RegisterReceiver(
    u8 type, NetworkMessageReceiver* receiver)
{
    mReceivers[type] = receiver;
}

void MessageRegistry::UnregisterReceiver(u8 type)
{
    mReceivers[type] = 0;
}

void MessageRegistry::RegisterFactory(
    u8 type, NetworkMessageFactoryBase* factory)
{
    mFactories[type] = factory;
}
