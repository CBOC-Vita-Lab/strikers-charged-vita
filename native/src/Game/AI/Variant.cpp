#include "Game/AI/Variant.inl"

#include "NL/nlFormat.h"
#include "port/wii_numeric.h"

Variant gvNotSet;

NLString Variant::ToString() const
{
    NLString result;

    if (IsSet())
    {
        NLString valueString = "???";

        switch (GetType())
        {
        case FT_BOOL:
            valueString = mData.b ? "True" : "False";
            break;
        case FT_CHAR:
            valueString = Format(NLString("{0}"), mData.c);
            break;
        case FT_SHORT:
            valueString = Format(NLString("{0}"), (int)mData.s);
            break;
        case FT_INT:
            valueString = Format(NLString("{0}"), mData.i);
            break;
        case FT_U32:
            valueString = Format(NLString("{0}"), mData.u);
            break;
        case FT_FLOAT:
            valueString = Format(NLString("{0}"), mData.f);
            break;
        case FT_POINTER:
            valueString = Format(NLString("{0}"), (unsigned long)mData.pointer);
            break;
        case FT_VECTOR:
            valueString = Format(NLString("({0},{1},{2})"), mData.vector.x, mData.vector.y, mData.vector.z);
            break;
        case FT_STRING:
            valueString = Format(NLString("{0}"), mData.string);
            break;
        }

        result = valueString;
    }
    else
    {
        result = "N/A";
    }

    return result;
}

unsigned long Variant::GetHash() const
{
    unsigned long hash = 0;

    switch (GetType())
    {
    case FT_BOOL:
        hash = mData.b;
        break;
    case FT_CHAR:
        hash = mData.c;
        break;
    case FT_SHORT:
        hash = mData.s;
        break;
    case FT_INT:
        hash = mData.i;
        break;
    case FT_U32:
        hash = mData.u;
        break;
    case FT_FLOAT:
        hash = port_f2u32(mData.f);   // MWCC: negative -> 0 (np/port/wii_numeric.h)
        break;
    case FT_POINTER:
        hash = (unsigned long)mData.pointer;
        break;
    case FT_VECTOR:
        hash = port_f2u32(mData.vector.x)
             * port_f2u32(mData.vector.y);
        {
            unsigned long zHash = port_f2u32(mData.vector.z);
            zHash ^= hash;
            hash = zHash;
        }
        break;
    case FT_STRING:
        hash = nlStringHash(mData.string);
        break;
    }

    return hash;
}
