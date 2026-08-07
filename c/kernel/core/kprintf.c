#include <stdint.h>
#include <stdarg.h>
#include "kprintf.h"
#include "klog.h"
#include "vga.h"
#include "serial.h"

/* The formatter writes through a sink so the same code serves the console
 * (VGA + COM1 + the log ring) and a plain memory buffer (ksnprintf). The
 * console sink emits exactly the bytes the old char-by-char loop emitted, in
 * the same order, with no lock held -- serial_putc busy-waits on the UART, and
 * holding a lock across ~87us per character would be a multi-millisecond
 * interrupts-off window on real hardware. */
/* aligned(8), and every field assigned rather than brace-initialised, ON
 * PURPOSE.
 *
 * This struct is 32 bytes. Left to itself clang gives a local that size a
 * 16-byte alignment and initialises it with `movaps` -- an ALIGNED SSE store,
 * which #GPs if the stack is not actually 16-byte aligned. The SysV ABI says it
 * always is, and for ordinary calls it is; but kprintf is called from interrupt
 * stubs, from freshly built thread stacks and from the fault path, and at least
 * one of those entry points in this kernel arrives 8 bytes off (see
 * g_misaligned_calls below -- the old kprintf never noticed because it only
 * ever touched bytes and 8-byte words).
 *
 * A logger that requires the machine to be healthy is worthless in the one
 * situation it exists for. So this one does not: capping the alignment at 8
 * makes every access here work on any stack the caller hands us. The
 * misalignment is still a real bug in whoever's entry path it is -- so it is
 * counted and reported through /dev/kstat instead of being silently absorbed. */
struct out {
    void (*put)(struct out *o, char c);
    char *buf;          /* buffer sink */
    int   max, n;
    int   level;        /* console sink: severity fed to the ring */
    int   console;      /* console sink: 0 = ring only (debug levels) */
} __attribute__((aligned(8)));

/* Callers that arrived with a stack the ABI says cannot happen. Recorded, not
 * acted on: touching the log from here would recurse. Reported by /dev/kstat. */
static unsigned long g_misaligned_calls;
static void         *g_misaligned_caller;

unsigned long kprintf_misaligned_calls(void)  { return g_misaligned_calls; }
void         *kprintf_misaligned_caller(void) { return g_misaligned_caller; }

static inline void note_alignment(void *frame, void *caller)
{
    /* After `push rbp; mov rsp,rbp` a correctly-aligned frame has rbp % 16 == 0. */
    if (__builtin_expect(((unsigned long)frame & 15u) != 0, 0)) {
        g_misaligned_calls++;
        g_misaligned_caller = caller;
    }
}

static void out_console(struct out *o, char c)
{
    if (o->console) {
        vga_putc(c);
        serial_putc(c);
    }
    klog_putc(o->level, c);
    o->n++;
}

static void out_buf(struct out *o, char c)
{
    if (o->n < o->max - 1)      /* full: drop (truncate), never overrun */
        o->buf[o->n++] = c;
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

static void kvformat(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            o->put(o, *fmt);
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
        /* length modifier: l / ll / z (all mean "64-bit" on this target) */
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'z') { lng = 1; fmt++; }

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
            long long v = lng ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            char *o2 = buf;
            uint64_t mag;
            if (v < 0) { *o2++ = '-'; mag = (uint64_t)(-v); }
            else       { mag = (uint64_t)v; }
            len = (int)(o2 - buf) + num_to_buf(o2, mag, 10, 0);
            break;
        }
        case 'u':
            len = num_to_buf(buf, lng ? va_arg(ap, unsigned long long)
                                      : (uint64_t)va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            len = num_to_buf(buf, lng ? va_arg(ap, unsigned long long)
                                      : (uint64_t)va_arg(ap, unsigned), 16, 0);
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
            return;
        default:
            o->put(o, '%');
            o->put(o, *fmt);
            continue;
        }

        int pad = width - len;
        if (pad < 0)
            pad = 0;
        char padc = (zero && !left) ? '0' : ' ';

        if (!left)
            while (pad-- > 0)
                o->put(o, padc);
        for (int i = 0; i < len; i++)
            o->put(o, out[i]);
        if (left)
            while (pad-- > 0)
                o->put(o, ' ');
    }
}

static void out_init(struct out *o, void (*put)(struct out *, char),
                     char *buf, int max, int level, int console)
{
    o->put = put; o->buf = buf; o->max = max; o->n = 0;
    o->level = level; o->console = console;
}

void kprintf(const char *fmt, ...)
{
    note_alignment(__builtin_frame_address(0), __builtin_return_address(0));

    struct out o;
    out_init(&o, out_console, 0, 0, KL_INFO, 1);
    va_list ap;
    va_start(ap, fmt);
    kvformat(&o, fmt, ap);
    va_end(ap);
}

void kvlog_out(int level, int console, const char *prefix,
               const char *fmt, va_list ap)
{
    struct out o;
    out_init(&o, out_console, 0, 0, level, console);
    for (; prefix && *prefix; prefix++)
        o.put(&o, *prefix);
    kvformat(&o, fmt, ap);

    /* Auto-terminate: an unterminated line would sit in the per-CPU assembler
     * until something else printed, so a caller that forgets '\n' would see
     * its message glued to the next one. */
    int n = 0;
    while (fmt[n]) n++;
    if (n == 0 || fmt[n - 1] != '\n')
        o.put(&o, '\n');
}

int kvsnprintf(char *buf, int max, const char *fmt, va_list ap)
{
    if (max <= 0)
        return 0;
    struct out o;
    out_init(&o, out_buf, buf, max, KL_INFO, 0);
    kvformat(&o, fmt, ap);
    buf[o.n] = 0;
    return o.n;
}

int ksnprintf(char *buf, int max, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, max, fmt, ap);
    va_end(ap);
    return n;
}
