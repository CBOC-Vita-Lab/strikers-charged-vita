// gcdonor contrib: UTF-16 printf that does not depend on libc's wide printf (patches/gcdonor/README).
// NPB's nlPrint.cpp narrows the format and calls vsnprintf, which covers %d/%s/%f but not %ls (a 16-bit
// string argument): the one use is SHOnlineRanking.cpp:421. This is a drop-in for nlSNPrintf's 16-bit
// variant: #include it in src/NL/nlPrint.cpp and call nlFormat16(buffer, size, format, args).
// Unit-tested on host32 (tests/nlformat16_test.cpp) against narrow snprintf for the formats the game uses.
#ifndef NL_FORMAT16_H
#define NL_FORMAT16_H
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

// NP: the 16-bit variant formats UTF-16 text with the game's own 16-bit characters. It used to be
// vswprintf with the buffers cast to wchar_t*, which is only right where wchar_t is 16 bits and the
// C library's wide printf agrees (MSL on the console); with a 32-bit wchar_t in the C library (glibc,
// newlib) it reads two characters as one and writes garbage. Numeric conversions are done by the
// narrow snprintf, strings are copied: %s takes a narrow char*, %ls a 16-bit string, as in C99.
static int nlFormat16(unsigned short* buffer, unsigned long size, const unsigned short* format, va_list args)
{
    unsigned long n = 0;
    int total = 0;
#define NL16_PUT(c)                                    \
    do                                                 \
    {                                                  \
        if (size != 0 && n + 1 < size)                 \
            buffer[n++] = (unsigned short)(c);         \
        total++;                                       \
    } while (0)

    while (*format != 0)
    {
        if (*format != '%')
        {
            NL16_PUT(*format++);
            continue;
        }
        format++;
        if (*format == '%')
        {
            NL16_PUT('%');
            format++;
            continue;
        }

        char spec[40];
        int sl = 0;
        spec[sl++] = '%';
        int leftAlign = 0;
        // flags
        while (*format == '-' || *format == '+' || *format == ' ' || *format == '#' || *format == '0')
        {
            if (*format == '-')
                leftAlign = 1;
            if (sl < 8)
                spec[sl++] = (char)*format;
            format++;
        }
        // width
        int width = -1;
        if (*format == '*')
        {
            width = va_arg(args, int);
            if (width < 0)
            {
                leftAlign = 1;
                width = -width;
            }
            format++;
        }
        else
        {
            while (*format >= '0' && *format <= '9')
            {
                width = (width < 0 ? 0 : width) * 10 + (*format - '0');
                format++;
            }
        }
        // precision
        int precision = -1;
        if (*format == '.')
        {
            format++;
            precision = 0;
            if (*format == '*')
            {
                precision = va_arg(args, int);
                format++;
            }
            else
            {
                while (*format >= '0' && *format <= '9')
                {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }
        }
        // length
        enum { LEN_INT, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_LD } len = LEN_INT;
        if (*format == 'h')
        {
            format++;
            len = LEN_H;
            if (*format == 'h')
            {
                format++;
                len = LEN_HH;
            }
        }
        else if (*format == 'l')
        {
            format++;
            len = LEN_L;
            if (*format == 'l')
            {
                format++;
                len = LEN_LL;
            }
        }
        else if (*format == 'z' || *format == 'j' || *format == 't')
        {
            format++;
            len = LEN_Z;
        }
        else if (*format == 'L')
        {
            format++;
            len = LEN_LD;
        }

        unsigned short conv = *format;
        if (conv == 0)
            break;
        format++;

        char tmp[512];
        const unsigned short* wstr = 0;
        const char* nstr = 0;
        int count = 0;
        int numeric = 0;

        switch (conv)
        {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        {
            // width and precision go through the narrow snprintf with the flags
            if (width >= 0)
                sl += snprintf(spec + sl, 12, "%d", width);
            if (precision >= 0)
                sl += snprintf(spec + sl, 12, ".%d", precision);
            const char* lm = len == LEN_LL ? "ll" : len == LEN_L ? "l" : len == LEN_Z ? "z" : "";
            for (int k = 0; lm[k] != 0; k++)
                spec[sl++] = lm[k];
            spec[sl++] = (char)conv;
            spec[sl] = 0;
            if (len == LEN_LL)
            {
                if (conv == 'd' || conv == 'i')
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, long long));
                else
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, unsigned long long));
            }
            else if (len == LEN_L)
            {
                if (conv == 'd' || conv == 'i')
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, long));
                else
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, unsigned long));
            }
            else if (len == LEN_Z)
            {
                if (conv == 'd' || conv == 'i')
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, ptrdiff_t));
                else
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, size_t));
            }
            else
            {
                // hh and h arrive promoted to int
                if (conv == 'd' || conv == 'i')
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, int));
                else
                    count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, unsigned int));
            }
            numeric = 1;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
        {
            if (width >= 0)
                sl += snprintf(spec + sl, 12, "%d", width);
            if (precision >= 0)
                sl += snprintf(spec + sl, 12, ".%d", precision);
            spec[sl++] = (char)conv;
            spec[sl] = 0;
            if (len == LEN_LD)
                count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, long double));
            else
                count = snprintf(tmp, sizeof(tmp), spec, va_arg(args, double));
            numeric = 1;
            break;
        }
        case 'p':
            count = snprintf(tmp, sizeof(tmp), "%p", va_arg(args, void*));
            numeric = 1;
            break;
        case 'c':
        {
            tmp[0] = 0;
            wstr = 0;
            unsigned short ch = (unsigned short)va_arg(args, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!leftAlign)
                for (int k = 0; k < pad; k++)
                    NL16_PUT(' ');
            NL16_PUT(ch);
            if (leftAlign)
                for (int k = 0; k < pad; k++)
                    NL16_PUT(' ');
            continue;
        }
        case 's':
        case 'S':
        {
            int wide = (conv == 'S') || (conv == 's' && len == LEN_L);
            // %s is a narrow string, %ls / %S a 16-bit one
            int sn = 0;
            if (wide)
            {
                wstr = va_arg(args, const unsigned short*);
                if (wstr == 0)
                    nstr = "(null)";
                else
                    while (wstr[sn] != 0 && (precision < 0 || sn < precision))
                        sn++;
            }
            else
            {
                nstr = va_arg(args, const char*);
                if (nstr == 0)
                    nstr = "(null)";
                while (nstr[sn] != 0 && (precision < 0 || sn < precision))
                    sn++;
            }
            int pad = width > sn ? width - sn : 0;
            if (!leftAlign)
                for (int k = 0; k < pad; k++)
                    NL16_PUT(' ');
            for (int k = 0; k < sn; k++)
                NL16_PUT(wstr != 0 ? wstr[k] : (unsigned short)(unsigned char)nstr[k]);
            if (leftAlign)
                for (int k = 0; k < pad; k++)
                    NL16_PUT(' ');
            continue;
        }
        default:
            // unknown conversion: emit it as text so the problem is visible
            NL16_PUT('%');
            NL16_PUT(conv);
            continue;
        }

        if (numeric)
        {
            if (count < 0)
                count = 0;
            if (count > (int)sizeof(tmp) - 1)
                count = (int)sizeof(tmp) - 1;
            for (int k = 0; k < count; k++)
                NL16_PUT((unsigned short)(unsigned char)tmp[k]);
        }
    }
#undef NL16_PUT
    if (size != 0)
        buffer[n] = 0;
    return total;
}


#endif // NL_FORMAT16_H
