/* Host leak/ASan harness for c/lib/image/gif.c.
 * kmalloc/kfree are backed by malloc/free with an outstanding-allocation
 * counter: after every decode (success OR failure) the count must be back
 * to the value it had before, and ASan/UBSan watch for memory errors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "img.h"

static long outstanding;
void *kmalloc(unsigned long n) { outstanding++; return malloc(n ? n : 1); }
void  kfree(void *p) { if (p) outstanding--; free(p); }

/* gif.c is compiled for this test with -Dstatic= so the file-local decoder
 * is linkable from here. */
int gif_decode(const uint8_t *p, int n, struct image *out);

/* registration plumbing is unused in this harness */
void img_register(img_detect_fn d, img_decode_fn f) { (void)d; (void)f; }
/* gif.c registers through the animated entry point now (frames + disposal). */
void img_register_anim(img_detect_fn d, img_decode_fn f, img_anim_fn a)
{ (void)d; (void)f; (void)a; }

static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz ? sz : 1);
    if (fread(buf, 1, sz, f) != (size_t)sz && sz) { fclose(f); free(buf); return 1; }
    fclose(f);

    /* decode at every truncation length too: malformed inputs are where
     * failure-path leaks live */
    int fails = 0;
    for (long cut = 0; cut <= sz; cut += (sz > 200 ? 37 : 1)) {
        struct image im = {0, 0, 0};
        long before = outstanding;
        int rc = gif_decode(buf, (int)cut, &im);
        if (rc == 0) kfree(im.rgba);          /* caller ownership, like img_free */
        if (outstanding != before) {
            printf("LEAK %s cut=%ld rc=%d delta=%ld\n", path, cut, rc,
                   outstanding - before);
            fails++;
        }
    }
    free(buf);
    if (outstanding != 0) { printf("LEAK %s final delta=%ld\n", path, outstanding); fails++; }
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) bad += run_file(argv[i]);
    if (bad) { printf("gif leak test: %d file(s) with leaks\n", bad); return 1; }
    printf("gif leak test: all inputs balanced (no leaks, ASan/UBSan clean)\n");
    return 0;
}
