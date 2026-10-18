#include "NL/nlPrint.h"

#include <stdarg.h>
#include <stdio.h>

int nlSNPrintf(char* buffer, unsigned long size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    buffer[size - 1] = '\0';
    return result;
}

int nlSNPrintf(unsigned short* buffer, unsigned long size, const unsigned short* format, ...)
{
    va_list args;
    va_start(args, format);
#ifdef NP_NATIVE
    // wchar_t is 16 bit here (-fshort-wchar, like Metrowerks), so libc's vswprintf cannot be used:
    // narrow the (ASCII) format, format with vsnprintf, widen the result.
    char narrowFormat[512];
    char narrowOut[1024];
    unsigned int n = 0;
    while (format[n] != 0 && n < sizeof(narrowFormat) - 1)
    {
        narrowFormat[n] = format[n] < 0x80 ? (char)format[n] : '?';
        n++;
    }
    narrowFormat[n] = 0;
    int result = vsnprintf(narrowOut, sizeof(narrowOut), narrowFormat, args);
    {
        unsigned int k = 0;
        while (narrowOut[k] != 0 && k + 1 < size)
        {
            buffer[k] = (unsigned char)narrowOut[k];
            k++;
        }
        buffer[k] = 0;
    }
#else
    int result = vswprintf(
        (wchar_t*)buffer, size, (const wchar_t*)format, args);
#endif
    va_end(args);
    buffer[size - 1] = '\0';
    return result;
}

int nlVSNPrintf(char* buffer, unsigned long size, const char* format, va_list args)
{
    int result = vsnprintf(buffer, size - 1, format, args);
    buffer[size - 1] = '\0';
    return result;
}
