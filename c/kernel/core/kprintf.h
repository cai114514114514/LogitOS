#ifndef LOGIT_KPRINTF_H
#define LOGIT_KPRINTF_H

#include <stdarg.h>

/* Minimal formatted output. Writes to both the VGA text screen and COM1 --
 * and, since the log ring landed, into klog as well, so everything printed
 * here can be read back later (`cat /dev/kmsg`) instead of only being visible
 * to whoever happened to be watching the serial port at the time.
 *
 * Console bytes and their order are UNCHANGED by that: the ring is fed one
 * character at a time from the same emit path, into a per-CPU line buffer, and
 * only the finished line is copied into the ring under a lock. See klog.h.
 *
 * Conversions: %s %c %d %u %x %p %% with '-' / '0' flags, a field width, and
 * the 'l' / 'll' length modifiers (%lu %llx %ld ...). */

void kprintf(const char *fmt, ...);

/* snprintf-alike over the same formatter. Writes at most `max-1` bytes plus a
 * NUL and returns the number of bytes actually written (never >= max), so
 * callers can chain `n += ksnprintf(buf + n, max - n, ...)`. Touches nothing
 * but the caller's buffer -- in particular it does NOT feed the log ring, so
 * it is what the log renderer and the panic path format with. */
int ksnprintf(char *buf, int max, const char *fmt, ...);
int kvsnprintf(char *buf, int max, const char *fmt, va_list ap);

/* Callers that entered kprintf with a stack the SysV ABI says is impossible
 * (rsp not 16-byte aligned at the call). Counted rather than acted on -- the
 * logger tolerates it, but it is a real bug in the caller's entry path, and a
 * tolerated bug nobody can see is a bug nobody fixes. Surfaced by /dev/kstat. */
unsigned long kprintf_misaligned_calls(void);
void         *kprintf_misaligned_caller(void);

/* Internal: klog.c's levelled path. Emits `prefix` then the formatted text at
 * severity `level`, to the console only if `console` is non-zero, and always
 * into the ring. Appends a newline if the format did not end with one. */
void kvlog_out(int level, int console, const char *prefix,
               const char *fmt, va_list ap);

#endif /* LOGIT_KPRINTF_H */
