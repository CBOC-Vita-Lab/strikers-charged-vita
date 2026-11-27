#ifndef GAME_MESSAGE_SERIALIZER_H
#define GAME_MESSAGE_SERIALIZER_H

#include <string.h>

#include "types.h"

enum eNetworkSerializationDirection
{
    NETWORK_SERIALIZE_READ = 0,
    NETWORK_SERIALIZE_WRITE = 1,
};

class NetworkMessageSerializer
{
public:
    NetworkMessageSerializer(int direction, u8* buffer, unsigned long size);
    ~NetworkMessageSerializer();

    int GetLength() const { return mPosition - mBuffer; }

    // np (STR-144): never reads or writes past the buffer. A transfer that does not fit sets mFailed (a received message that ended early or
    // claims more than it holds, a send that would overrun its buffer); the destination of a read is zeroed and the position stays, so the
    // caller's loops end at their next count and the receiver can discard the message (MessageRegistry::Dispatch, TransportMessage).
    void Transfer(void* value, unsigned long size)
    {
        if (!Fits(size))
        {
            mFailed = true;
            if (mDirection == NETWORK_SERIALIZE_READ)
                memset(value, 0, size);
            return;
        }
        if (mDirection == NETWORK_SERIALIZE_READ)
        {
            memcpy(value, mPosition, size);
            mPosition += size;
        }
        else
        {
            memcpy(mPosition, value, size);
            mPosition += size;
        }
    }

    // True when `size` more bytes fit before mEnd (always true for a serializer without an end).
    bool Fits(unsigned long size) const
    {
        return !mFailed && (mEnd == 0 || (mPosition <= mEnd && (unsigned long)(mEnd - mPosition) >= size));
    }

    /* 0x00 */ int mDirection;
    /* 0x04 */ u8* mPosition;
    /* 0x08 */ u8* mBuffer;
    /* 0x0C */ u8* mEnd;
    /* 0x10 */ bool mOwnsBuffer;
    /* 0x11 */ bool mFailed;   // np: a transfer did not fit (see Transfer); fits the old padding, the size stays 0x14
}; // size: 0x14

#endif // GAME_MESSAGE_SERIALIZER_H
