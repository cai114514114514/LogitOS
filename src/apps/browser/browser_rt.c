/* Ring-3 runtime shims for the render pipeline (M17 L1).
 *
 * net/{dom,css,layout}.c are compiled into browser.aex unchanged; they reference
 * kernel symbols (kmalloc/kfree/text_measure/res_fetch/img_*). Map those onto the
 * app's mini-libc + the new render syscalls here. */
#include "aqua.h"
#include <stddef.h>
#include "img.h"

void *malloc(size_t);
void  free(void *);

void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

/* Layout measures every word; route to the kernel font engine. */
int text_measure(const char *s, int len, int px, int mono)
{
    return text_measure_px(s, len, px, mono);
}

/* Sub-resource (image) fetch: pull the raw encoded bytes via the kernel, then
 * hand the layout engine a malloc'd copy it can kfree (matches net/http.c's ABI). */
int res_fetch(const char *src, unsigned char **buf, int *len)
{
    static unsigned char raw[262144];           /* 256 KiB single-image cap */
    int n = res_fetch_raw(src, raw, sizeof raw);
    if (n <= 0) return -1;
    unsigned char *b = malloc((size_t)n);
    if (!b) return -1;
    for (int i = 0; i < n; i++) b[i] = raw[i];
    *buf = b; *len = n;
    return 0;
}
