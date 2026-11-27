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

#ifdef NP_NATIVE
#include <stdlib.h>
#include <string.h>

// The wide formatter of the native port. The old path narrowed the format and handed it to libc, whose "%ls" reads a 32-bit wchar_t string:
// with the 16-bit strings of the game ("%ls" of a player name) the argument was misread and the text came out empty (the ranking rows lost
// their names). This one reads "%ls" as a string of 16-bit units and leaves everything else to snprintf one conversion at a time.
// MSC_WIDE_PRINTF=0 restores the old path.
static int WideFormat(unsigned short* out, unsigned long size, const unsigned short* fmt, va_list args)
{
    unsigned long o = 0;
    for (const unsigned short* p = fmt; *p;)
    {
        if (*p != '%')
        {
            if (o + 1 < size) out[o] = *p;
            ++o;
            ++p;
            continue;
        }
        char spec[40];
        int sl = 0;
        spec[sl++] = '%';
        ++p;
        int width = 0, precision = -1;
        bool left = false;
        while (*p && sl < 24 && *p < 0x80 && strchr("-+ #0", (char)*p))
        {
            if (*p == '-') left = true;
            spec[sl++] = (char)*p++;
        }
        if (*p == '*')
        {
            width = va_arg(args, int);
            if (width < 0) { left = true; width = -width; }
            sl += snprintf(spec + sl, 12, "%d", width);
            ++p;
        }
        else
        {
            while (*p >= '0' && *p <= '9' && sl < 30)
            {
                width = width * 10 + (*p - '0');
                spec[sl++] = (char)*p++;
            }
        }
        if (*p == '.')
        {
            spec[sl++] = '.';
            ++p;
            precision = 0;
            if (*p == '*')
            {
                precision = va_arg(args, int);
                sl += snprintf(spec + sl, 12, "%d", precision < 0 ? 0 : precision);
                ++p;
            }
            else
            {
                while (*p >= '0' && *p <= '9' && sl < 36)
                {
                    precision = precision * 10 + (*p - '0');
                    spec[sl++] = (char)*p++;
                }
            }
        }
        int longs = 0;
        bool shortMod = false;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 't')
        {
            if (*p == 'l') ++longs;
            if (*p == 'h') shortMod = true;
            ++p;
        }
        const unsigned short conv = *p ? *p++ : 0;
        char tmp[512];
        int n = 0;
        if (conv == '%')
        {
            tmp[n++] = '%';
        }
        else if (conv == 's' && longs)
        {
            const unsigned short* str = va_arg(args, const unsigned short*);
            if (!str) { static const unsigned short nul[] = { '(', 'n', 'u', 'l', 'l', ')', 0 }; str = nul; }
            int len = 0;
            while (str[len] && (precision < 0 || len < precision)) ++len;
            const int pad = width > len ? width - len : 0;
            if (!left) for (int k = 0; k < pad; ++k) { if (o + 1 < size) out[o] = ' '; ++o; }
            for (int k = 0; k < len; ++k) { if (o + 1 < size) out[o] = str[k]; ++o; }
            if (left) for (int k = 0; k < pad; ++k) { if (o + 1 < size) out[o] = ' '; ++o; }
            continue;
        }
        else if (conv == 'c' && longs)
        {
            const unsigned short c = (unsigned short)va_arg(args, int);
            if (o + 1 < size) out[o] = c;
            ++o;
            continue;
        }
        else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'c')
        {
            spec[sl] = 0;
            char full[48];
            int fl = 0;
            for (int k = 0; spec[k] && fl < 40; ++k) full[fl++] = spec[k];
            if (shortMod) full[fl++] = 'h';
            else if (longs >= 2) { full[fl++] = 'l'; full[fl++] = 'l'; }
            else if (longs == 1) full[fl++] = 'l';
            full[fl++] = (char)conv;
            full[fl] = 0;
            if (longs >= 2) n = snprintf(tmp, sizeof tmp, full, va_arg(args, long long));
            else if (longs == 1) n = snprintf(tmp, sizeof tmp, full, va_arg(args, long));
            else n = snprintf(tmp, sizeof tmp, full, va_arg(args, int));
        }
        else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G')
        {
            spec[sl++] = (char)conv;
            spec[sl] = 0;
            n = snprintf(tmp, sizeof tmp, spec, va_arg(args, double));
        }
        else if (conv == 's')
        {
            spec[sl++] = 's';
            spec[sl] = 0;
            const char* str = va_arg(args, const char*);
            n = snprintf(tmp, sizeof tmp, spec, str ? str : "(null)");
        }
        else if (conv == 'p')
        {
            n = snprintf(tmp, sizeof tmp, "%p", va_arg(args, void*));
        }
        else
        {
            tmp[n++] = '?';
        }
        if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
        for (int k = 0; k < n; ++k)
        {
            if (o + 1 < size) out[o] = (unsigned char)tmp[k];
            ++o;
        }
    }
    if (size) out[o < size ? o : size - 1] = 0;
    return (int)o;
}

static bool WideFormatEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char* e = getenv("MSC_WIDE_PRINTF");
        enabled = !(e && e[0] == '0');
    }
    return enabled != 0;
}
#endif

int nlSNPrintf(unsigned short* buffer, unsigned long size, const unsigned short* format, ...)
{
    va_list args;
    va_start(args, format);
#ifdef NP_NATIVE
    if (WideFormatEnabled())
    {
        int wide = WideFormat(buffer, size, format, args);
        va_end(args);
        return wide;
    }
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
