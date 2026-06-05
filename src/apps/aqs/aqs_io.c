#include "aqs.h"
#include <unistd.h>     /* write -- host libc or mini-libc; both write(1,...) */

/* Platform output. Normally bytes go straight to stdout (fd 1). Host unit tests
 * set a capture buffer via aqs_capture() so they can assert on `print` output
 * in-process; the same aqs_emit then appends there instead of writing. */
static char *g_cap = NULL;
static int   g_cap_len = 0, g_cap_cap = 0;

void aqs_capture(char *buf, int cap)   /* buf==NULL restores normal stdout output */
{
    g_cap = buf; g_cap_cap = cap; g_cap_len = 0;
    if (buf && cap > 0) buf[0] = 0;
}

void aqs_emit(const char *s, int n)
{
    if (g_cap) {
        for (int i = 0; i < n && g_cap_len < g_cap_cap - 1; i++) g_cap[g_cap_len++] = s[i];
        if (g_cap_cap > 0) g_cap[g_cap_len] = 0;
    } else {
        write(1, s, (size_t)n);
    }
}

void aqs_emit_cstr(const char *s)
{
    int n = 0; while (s[n]) n++;
    aqs_emit(s, n);
}
