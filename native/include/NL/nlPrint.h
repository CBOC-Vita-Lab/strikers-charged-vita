#ifndef _NLPRINT_H_
#define _NLPRINT_H_

#ifdef NP_NATIVE
#include <stdarg.h>
#else
struct __va_list_struct;
#endif

inline int nlPrintf(const char* format, ...)
{
    return 0;
}

#ifdef NP_NATIVE
int nlVSNPrintf(char* buffer, unsigned long size, const char* format, va_list args);
#else
int nlVSNPrintf(char* buffer, unsigned long size, const char* format, __va_list_struct* args);
#endif
int nlSNPrintf(char* buffer, unsigned long size, const char* format, ...);
int nlSNPrintf(unsigned short* buffer, unsigned long size, const unsigned short* format, ...);

#endif // _NLPRINT_H_
