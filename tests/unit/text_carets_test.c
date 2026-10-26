/* LOGIT_TEXT_CARETS_QUERY through the real SYS_TEXT_MEASURE dispatch branch,
 * the real text engine and the shipped fonts. Built like
 * text_vertical_metrics_test.c: same .inc, ordinary host usercopy.
 *
 * TEXT_CARETS_BYTEWISE (the negative control) makes text.c emit a stop at
 * every byte; the "utf8-split" check must then fail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "logit_abi.h"
#include "text.h"
#include "ttf.h"
#include "spinlock.h"

static const char *root;
static int checks, fails, scale = 100;
static int lock_depth, copy_to_locked, copy_to_calls, kmalloc_locked, kmalloc_calls;
static void check(int ok, const char *name)
{ ++checks; if (!ok) ++fails; printf("%s %s\n", ok ? "PASS" : "FAIL", name); }
void *kmalloc(unsigned long n) { ++kmalloc_calls; if (lock_depth) ++kmalloc_locked; return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
void spin_lock(spinlock_t *p) { (void)p; ++lock_depth; }
void spin_unlock(spinlock_t *p) { (void)p; --lock_depth; }
uint64_t spin_lock_irqsave(spinlock_t *p) { (void)p; ++lock_depth; return 0; }
void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; --lock_depth; }
static FILE *font_file(const char *path)
{ char name[2048]; snprintf(name, sizeof name, "%s%s", root, path); return fopen(name,"rb"); }
int vfs_size(const char *path)
{ FILE *f=font_file(path); if(!f)return -1; fseek(f,0,SEEK_END); int n=(int)ftell(f); fclose(f);return n; }
int vfs_read(const char *path, void *buf, int max)
{ FILE *f=font_file(path);if(!f)return -1;int n=(int)fread(buf,1,(size_t)max,f);fclose(f);return n; }
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{ (void)x;(void)y;(void)cov;(void)w;(void)h;(void)color; }
static int S(int x) {return x>=0?x*scale/100:-((-x*scale+99)/100);}
static int PT(int x) {int a=100*(x+1);return (a>=0?(a+scale-1)/scale:-((-a)/scale))-1;}
static int fb_scale(void) {return scale;}
static int user_copy_from(void *d,const void *s,unsigned long n)
{ memcpy(d,s,n); return 0; }
static int user_copy_to(void *d,const void *s,unsigned long n)
{ ++copy_to_calls; if (lock_depth) ++copy_to_locked; memcpy(d,s,n); return 0; }
#define USER_TEXT_MAX 1024
static long fixture_sys(long n,long a,long b,long c)
{
    if(n != SYS_TEXT_MEASURE) return -1;
#include "text_measure_dispatch.inc"
}
#define _sys fixture_sys
#include "../../c/apps/text_metrics_wiring.inc"
#undef _sys

#define MAXS 1024
struct res { int rc, n; unsigned short off[MAXS]; int x[MAXS]; };
static struct res R;
static int query(const char *s, int px, int face, int max, unsigned version)
{
    struct logit_text_carets_query q;
    memset(&q, 0, sizeof q);
    q.size = sizeof q; q.version = version; q.s = s; q.len = (int)strlen(s);
    q.px = px; q.face = face; q.off = R.off; q.x24_8 = R.x; q.max = max; q.count_out = &R.n;
    R.n = -7;
    R.rc = (int)fixture_sys(SYS_TEXT_MEASURE, (long)&q, LOGIT_TEXT_CARETS_QUERY, 0);
    return R.rc;
}

static int utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* LTR run: last caret == text_measure and == metrics advance, at any scale. */
static void ltr_case(const char *s, int px, int face, int pct, int expect_stops)
{
    scale = pct;
    int len = (int)strlen(s);
    char nm[160];
    int rc = query(s, px, face, MAXS, LOGIT_TEXT_CARETS_VERSION);
    snprintf(nm, sizeof nm, "carets supported (%s @%d%%)", s, pct); check(rc == 1 && R.n >= 1, nm);
    if (rc != 1) return;
    struct logit_text_metrics m;
    text_run_metrics_px(s, len, px, face, &m);
    int meas = text_measure(s, len, S(px), face);
    snprintf(nm, sizeof nm, "final caret equals text_measure (%s @%d%%)", s, pct);
    check(R.x[R.n-1] == meas * 256 && R.off[R.n-1] == len, nm);
    snprintf(nm, sizeof nm, "final caret equals metrics advance (%s @%d%%)", s, pct);
    check(R.x[R.n-1] == m.advance * 256, nm);
    snprintf(nm, sizeof nm, "first stop is origin (%s)", s); check(R.off[0] == 0 && R.x[0] == 0, nm);
    int mono = 1, split = 0, xmono = 1;
    for (int i = 0; i < R.n; i++) {
        if (i && R.off[i] <= R.off[i-1]) mono = 0;
        if (i && R.x[i] < R.x[i-1]) xmono = 0;
        if (R.off[i] < len && utf8_cont((unsigned char)s[R.off[i]])) split = 1;
    }
    check(!split, "utf8-split");
    snprintf(nm, sizeof nm, "offsets monotonic in logical order (%s)", s); check(mono, nm);
    snprintf(nm, sizeof nm, "LTR positions non-decreasing (%s)", s); check(xmono, nm);
    if (expect_stops >= 0) {
        snprintf(nm, sizeof nm, "stop count %d (%s)", expect_stops, s);
        check(R.n == expect_stops, nm);
    }
    (void)PT;
}

int main(int argc,char **argv)
{
    if(argc!=2)return 2;root=argv[1];text_init();
    const int pcts[] = {100, 125, 150};
    for (unsigned k = 0; k < 3; k++) {
        ltr_case("Hello, world 123", 14, 0, pcts[k], 17);
        ltr_case("Hello, world 123", 14, LOGIT_FACE_BOLD, pcts[k], 17);
        ltr_case("code", 13, LOGIT_FACE_MONO, pcts[k], 5);
        ltr_case("\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97", 16, 0, pcts[k], 4);
        ltr_case("e\xcc\x81" "a\xcc\x80\xcc\xa3" "b", 20, 0, pcts[k], 4);   /* e+acute, a+grave+dot, b */
    }
    scale = 100;

    /* Combining clusters: no stop between base and mark, offsets exact. */
    query("e\xcc\x81" "a", 20, 0, MAXS, LOGIT_TEXT_CARETS_VERSION);
    check(R.n == 3 && R.off[0] == 0 && R.off[1] == 3 && R.off[2] == 4, "combining-cluster stops at 0,3,4");
    /* Emoji ZWJ-less modifier and variation selector stay attached. */
    query("a\xef\xb8\x8f" "b", 20, 0, MAXS, LOGIT_TEXT_CARETS_VERSION);
    check(R.n == 3 && R.off[1] == 4, "variation selector stays attached");

    /* Arabic: bidi-aware visual positions; extents equal text_measure. */
    {
        const char *ar = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7";   /* 5 letters, 10 bytes */
        for (unsigned k = 0; k < 3; k++) {
            scale = pcts[k];
            query(ar, 18, 0, MAXS, LOGIT_TEXT_CARETS_VERSION);
            int meas = text_measure(ar, (int)strlen(ar), S(18), 0) * 256, mx = 0, mn = 1 << 30, mono = 1, split = 0;
            for (int i = 0; i < R.n; i++) {
                if (R.x[i] > mx) mx = R.x[i];
                if (R.x[i] < mn) mn = R.x[i];
                if (i && R.off[i] <= R.off[i-1]) mono = 0;
                if (R.off[i] < 10 && utf8_cont((unsigned char)ar[R.off[i]])) split = 1;
            }
            check(R.rc == 1 && R.n == 6 && mx == meas && mn == 0, "arabic extents equal text_measure");
            check(R.x[0] == meas && R.x[R.n-1] == 0, "arabic logical start at the right edge, end at the left");
            check(mono && R.off[R.n-1] == 10, "arabic offsets monotonic to len");
            check(!split, "utf8-split");
        }
        scale = 100;
        /* Mixed: LTR text then Arabic; stops stay in logical order. */
        const char *mix = "ab \xd9\x85\xd8\xb1 cd";
        query(mix, 16, 0, MAXS, LOGIT_TEXT_CARETS_VERSION);
        int mono = 1; for (int i = 1; i < R.n; i++) if (R.off[i] <= R.off[i-1]) mono = 0;
        check(R.rc == 1 && mono && R.off[0] == 0 && R.off[R.n-1] == (int)strlen(mix), "mixed bidi offsets logical");
        int meas = text_measure(mix, (int)strlen(mix), 16, 0) * 256, mx = 0;
        for (int i = 0; i < R.n; i++) if (R.x[i] > mx) mx = R.x[i];
        check(mx == meas, "mixed bidi max stop equals text_measure");
    }

    /* Argument handling and the max contract. */
    check(query("abc", 14, 0, MAXS, LOGIT_TEXT_CARETS_VERSION + 1) == 0, "version mismatch returns 0");
    check(query("abc", 14, 0, 3, LOGIT_TEXT_CARETS_VERSION) == -1, "more stops than max returns -1");
    check(query("abc", 14, 0, 4, LOGIT_TEXT_CARETS_VERSION) == 1 && R.n == 4, "exactly max stops fits");
    check(query("abc", 0, 0, 8, LOGIT_TEXT_CARETS_VERSION) == -1, "px 0 rejected");
    check(query("abc", 513, 0, 8, LOGIT_TEXT_CARETS_VERSION) == -1, "px 513 rejected");
    check(query("abc", 14, 4, 8, LOGIT_TEXT_CARETS_VERSION) == -1, "bad face bits rejected");
    check(query("abc", 14, 0, 0, LOGIT_TEXT_CARETS_VERSION) == -1, "max 0 rejected");
    check(query("abc", 14, 0, 1025, LOGIT_TEXT_CARETS_VERSION) == -1, "max 1025 rejected");
    check(query("", 14, 0, 4, LOGIT_TEXT_CARETS_VERSION) == 1 && R.n == 1 && R.off[0] == 0 && R.x[0] == 0, "empty run has one stop");
    {
        char big[LOGIT_TEXT_METRICS_MAX_BYTES + 2];
        memset(big, 'a', sizeof big); big[sizeof big - 1] = 0;
        check(query(big, 14, 0, MAXS, LOGIT_TEXT_CARETS_VERSION) == -1, "len over METRICS_MAX_BYTES rejected");
        big[LOGIT_TEXT_METRICS_MAX_BYTES - 1] = 0;
        check(query(big, 14, 0, MAXS, LOGIT_TEXT_CARETS_VERSION) == 1 && R.n == LOGIT_TEXT_METRICS_MAX_BYTES, "len at METRICS_MAX_BYTES accepted");
    }

    /* Locking and allocation discipline. */
    copy_to_calls = copy_to_locked = kmalloc_calls = kmalloc_locked = 0;
    query("Hello", 14, 0, 64, LOGIT_TEXT_CARETS_VERSION);
    check(copy_to_calls == 3 && copy_to_locked == 0, "user_copy_to ran three times with the text lock released");
    check(kmalloc_calls == 1 && kmalloc_locked == 0, "one staging kmalloc, outside the lock");
    check(lock_depth == 0, "lock balanced");

    printf("text-carets: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
