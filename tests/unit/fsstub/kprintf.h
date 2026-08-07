#ifndef LOGIT_KPRINTF_H
#define LOGIT_KPRINTF_H
/* Host stub. Quiet by default: the crash tests mount thousands of times and the
 * kernel's mount chatter would bury the verdict. Set fsstub_verbose to see it. */
#include <stdio.h>
#include <stdarg.h>
extern int fsstub_verbose;
static inline void kprintf(const char *fmt, ...)
{
    if (!fsstub_verbose) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
#endif
