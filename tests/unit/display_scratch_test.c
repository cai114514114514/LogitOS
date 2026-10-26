/* Per-display scratch slots: isolation, growth-failure and destroy. Built with
 * -DGFX_NCTX=2 and does not link fb.c, so it defines gfx_ctx_hook itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openlogit_display.h"
#include "display_pixels.h"

int (*gfx_ctx_hook)(void);
static int g_ctx;
static int hook(void) { return g_ctx; }

static long live, allocs, frees;
static int fail_alloc;
static void *t_alloc(unsigned long n) { if (fail_alloc) return 0; allocs++; live++; return malloc(n); }
static void t_free(void *p) { if (p) { frees++; live--; free(p); } }

static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } } while (0)

static int all_eq(const unsigned char *p, unsigned long n, unsigned char v)
{ for (unsigned long i = 0; i < n; i++) if (p[i] != v) return 0; return 1; }

int main(void)
{
    gfx_ctx_hook = hook;
    struct ol_display a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.allocate = b.allocate = t_alloc;
    a.release = b.release = t_free;

    /* Two contexts interleave use of the same slot. */
    g_ctx = 0;
    unsigned char *pa = display_scratch(&a, OL_DS_GROUP, 4096);
    CHECK(pa != 0, "alloc A");
    if (pa) memset(pa, 0xA5, 4096);
    g_ctx = 1;
    unsigned char *pb = display_scratch(&b, OL_DS_GROUP, 4096);
    CHECK(pb != 0, "alloc B");
    if (pb) memset(pb, 0x5A, 4096);
    g_ctx = 0;
    unsigned char *pa2 = display_scratch(&a, OL_DS_GROUP, 4096);
    CHECK(pa2 && all_eq(pa2, 4096, 0xA5), "interleave");
    CHECK(pb && all_eq(pb, 4096, 0x5A), "interleave B");

    /* Same-or-smaller request reuses; never shrinks. */
    long n0 = allocs;
    void *p1 = display_scratch(&a, OL_DS_GROUP, 100);
    CHECK(p1 == pa2 && allocs == n0 && a.scratch_n[OL_DS_GROUP] == 4096, "no shrink");

    /* Growth failure keeps old buffer and size, sets OL_LIMIT. */
    fail_alloc = 1; a.error = 0;
    void *pf = display_scratch(&a, OL_DS_GROUP, 1 << 20);
    CHECK(pf == 0 && a.error == OL_LIMIT, "growth failure reports");
    CHECK(a.scratch[OL_DS_GROUP] == pa2 && a.scratch_n[OL_DS_GROUP] == 4096 &&
          all_eq(a.scratch[OL_DS_GROUP], 4096, 0xA5), "growth failure keeps contents");
    fail_alloc = 0;

    /* Successful growth. */
    unsigned char *pg = display_scratch(&a, OL_DS_GROUP, 8192);
    CHECK(pg && a.scratch_n[OL_DS_GROUP] == 8192, "grow");

    /* Slots are independent. */
    CHECK(display_scratch(&a, OL_DS_PATH, 64) != (void *)pg, "slots independent");
    CHECK(display_scratch(&a, OL_DS_N, 8) == 0 && display_scratch(&a, -1, 8) == 0, "bad slot");

    /* Destroy frees everything. */
    display_scratch(&b, OL_DS_SHADOW, 32);
    ol_display_destroy(&a); ol_display_destroy(&b);
    CHECK(live == 0 && allocs == frees, "destroy frees all");
    int clean = 1;
    for (int i = 0; i < OL_DS_N; i++) if (a.scratch[i] || a.scratch_n[i] || b.scratch[i]) clean = 0;
    CHECK(clean, "destroy clears slots");

    if (!bad) puts("display_scratch_test: ok");
    return bad;
}
