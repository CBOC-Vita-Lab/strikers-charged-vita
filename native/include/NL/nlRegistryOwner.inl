#ifndef NL_REGISTRY_OWNER_INL
#define NL_REGISTRY_OWNER_INL

#include "NL/nlRegistry.h"

#ifndef NP_NATIVE // the native RegistryIterator is raw storage (nlRegistry.h): these slot bodies call themselves
RegistryValue RegistryIterator::GetValue()
{
    return GetValue();
}

RegistryNode* RegistryIterator::GetNode()
{
    return GetNode();
}

void RegistryIterator::Next()
{
    Next();
}

bool RegistryIterator::IsDone()
{
    return IsDone();
}

u32 RegistryIterator::GetHash()
{
    return GetHash();
}

bool RegistryIterator::MoveNext()
{
    return MoveNext();
}

bool RegistryIterator::MovePrevious()
{
    return MovePrevious();
}

bool RegistryIterator::IsFirst()
{
    return IsFirst();
}

void RegistryIterator::ResetCursor()
{
    ResetCursor();
}

#endif // !NP_NATIVE

#endif // NL_REGISTRY_OWNER_INL
