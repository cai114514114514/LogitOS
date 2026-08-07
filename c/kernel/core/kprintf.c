#include <stdint.h>
#include <stdarg.h>
#include "kprintf.h"
#include "vga.h"
#include "serial.h"

/* Fan every character out to both sinks. */
static void emit(char c)
{
    vga_putc(c);
    serial_putc(c);
}

/* Render an unsigned value into buf (NUL-terminated); return its length. */
static int num_to_buf(char *buf, uint64_t value, unsigned base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int i = 0;

    if (value == 0)
        tmp[i++] = '0';
    while (value) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    int n = 0;
    while (i)
        buf[n++] = tmp[--i];
    buf[n] = '\0';
    return n;
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(*fmt);
            continue;
        }
        fmt++;

        /* flags */
        int left = 0, zero = 0;
        for (;; fmt++) {
            if (*fmt == '-')      left = 1;
            else if (*fmt == '0') zero = 1;
            else                  break;
        }
        /* width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        char buf[64];
        const char *out = buf;
        int len = 0;

        switch (*fmt) {
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p)
                p = "(null)";
            out = p;
            while (p[len])
                len++;
            break;
        }
        case 'c':
            buf[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 'd': {
            long long v = va_arg(ap, int);
            char *o = buf;
            uint64_t mag;
            if (v < 0) { *o++ = '-'; mag = (uint64_t)(-v); }
            else       { mag = (uint64_t)v; }
            len = (int)(o - buf) + num_to_buf(o, mag, 10, 0);
            break;
        }
        case 'u':
            len = num_to_buf(buf, va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            len = num_to_buf(buf, va_arg(ap, unsigned), 16, 0);
            break;
        case 'p':
            buf[0] = '0';
            buf[1] = 'x';
            len = 2 + num_to_buf(buf + 2, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0);
            break;
        case '%':
            buf[0] = '%';
            len = 1;
            break;
        case '\0':
            va_end(ap);
            return;
        default:
            emit('%');
            emit(*fmt);
            continue;
        }

        int pad = width - len;
        if (pad < 0)
            pad = 0;
        char padc = (zero && !left) ? '0' : ' ';

        if (!left)
            while (pad-- > 0)
                emit(padc);
        for (int i = 0; i < len; i++)
            emit(out[i]);
        if (left)
            while (pad-- > 0)
                emit(' ');
    }

    va_end(ap);
}
