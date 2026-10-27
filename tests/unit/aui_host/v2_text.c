/* v2_text.c -- the host harness's answer to GUI ABI v2 text (aui-text-adopt).
 *
 *   SYS_GUI_TEXT_RUN2        the same box glyphs as SYS_GUI_TEXT_RUN, blended with
 *                            the run's alpha, at 24.8-point placement; a weight
 *                            (ui-aui-text-api) draws as stroke thickness: each
 *                            box is 1 device px wider per baked slot above 400
 *                            (500 +1, 600 +2, 700 +3). Weight 0 (and 400) boxes
 *                            are byte-identical to what they were.
 *   SYS_TEXT_MEASURE b = -2  the carets query, consistent with the box-glyph
 *                            advances: stop i sits at aui_host_text_advance of the
 *                            first off[i] bytes, in device 24.8
 *
 * Clusters: a code point extends the one before it when it is a combining mark
 * (U+0300..036F), a variation selector, an emoji skin tone, ZWJ, or follows a ZWJ.
 * No stop is placed inside a cluster.
 *
 * Counters and a spy hook are exported for tests/unit/aui_text_test.c. */
#include <stdint.h>
#include <string.h>

#include "aui_host.h"

unsigned long v2_text_nmetrics, v2_text_ncarets, v2_text_nrun2;
void (*v2_text_spy)(const struct logit_run2 *);

static unsigned u8next(const unsigned char **pp, const unsigned char *e)
{
    const unsigned char *p = *pp;
    unsigned c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0xF8 || (c >= 0x80 && c < 0xC0)) extra = 0;
    else if (extra) c &= (0x3F >> extra);
    while (extra-- > 0 && p < e && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}
static int extender(unsigned cp, unsigned prev)
{
    return prev == 0x200D || cp == 0x200D || (cp >= 0x300 && cp <= 0x36F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

static long carets(struct aui_host *h, const struct logit_text_carets_query *q)
{
    if (q->size != sizeof *q || q->version != LOGIT_TEXT_CARETS_VERSION) return 0;
    if (q->flags || q->len < 0 || q->len > LOGIT_TEXT_METRICS_MAX_BYTES ||
        q->px < 1 || q->px > 512 || q->max < 1 || q->max > 1024 || (q->face & ~3)) return -1;
    int pxd = aui_host_pt(q->px, h->pct);
    const unsigned char *s = (const unsigned char *)q->s, *e = s + q->len, *p = s;
    int n = 0;
    q->off[n] = 0; q->x24_8[n++] = 0;
    unsigned prev = 0;
    while (p < e) {
        unsigned cp = u8next(&p, e);
        if (p < e) {
            const unsigned char *pk = p;
            unsigned nx = u8next(&pk, e);
            if (extender(nx, cp)) { prev = cp; continue; }
        }
        (void)prev;
        if (n >= q->max) return -1;
        q->off[n] = (unsigned short)(p - s);
        q->x24_8[n++] = aui_host_text_advance(q->s, (int)(p - s), pxd) * 256;
        prev = cp;
    }
    *q->count_out = n;
    return 1;
}

static long text_measure(struct aui_host *h, long a, long b, long c)
{
    if (b == LOGIT_TEXT_METRICS_QUERY && c == 0) v2_text_nmetrics++;
    if (b == LOGIT_TEXT_CARETS_QUERY && c == 0) {
        v2_text_ncarets++;
        return carets(h, (const struct logit_text_carets_query *)a);
    }
    return aui_host_sys_builtin(h, SYS_TEXT_MEASURE, a, b, c);
}

/* floor(v24_8 * pct / 100 / 256): fb_pt's floor, extended to 24.8, so a whole-point
 * run lands where the legacy run does */
static int dev24(int v24_8, int pct)
{
    long long n = (long long)v24_8 * pct;
    long long q = n >= 0 ? n / 25600 : -((-n + 25599) / 25600);
    return (int)q;
}

static void blend(struct aui_host *h, int x, int y, uint32_t rgb, int alpha)
{
    if (x < 0 || y < 0 || x >= h->w || y >= h->h) return;
    if (h->clip_on && (x < h->clx0 || y < h->cly0 || x >= h->clx1 || y >= h->cly1)) return;
    uint32_t d = h->px[(size_t)y * h->w + x], o = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int sc = (int)((rgb >> sh) & 255), dc = (int)((d >> sh) & 255);
        o |= (uint32_t)((sc * alpha + dc * (255 - alpha) + 127) / 255) << sh;
    }
    h->px[(size_t)y * h->w + x] = o;
}

static long text_run2(struct aui_host *h, long a, long b, long c)
{
    (void)b; (void)c;
    const struct logit_run2 *r = (const struct logit_run2 *)a;
    if (!r || r->size != sizeof *r || r->s == 0 || r->alpha < 0 || r->alpha > 255) return -1;
    v2_text_nrun2++;
    if (v2_text_spy) v2_text_spy(r);
    int pct = h->pct;
    int len = r->len < 0 ? 0 : r->len > 1023 ? 1023 : r->len;
    int px = dev24(r->px, pct);
    if (px < 1 || px > 512) return -1;
    int x = dev24(r->x, pct), y = dev24(r->y, pct);
    int base = y + (80 * px + 50) / 100, ht = (70 * px + 50) / 100, cur = 0;
    if (ht < 1) ht = 1;
    /* the kernel's slot rule (text.c text_weight_slot): nearest of 400..700, ties up */
    int wt = r->weight <= 0 ? 0 : r->weight < 100 ? 100 : r->weight > 900 ? 900 : r->weight;
    int thick = wt < 450 ? 0 : wt < 550 ? 1 : wt < 650 ? 2 : 3;
    const unsigned char *p = (const unsigned char *)r->s, *e = p + len, *st;
    while (p < e) {
        st = p;
        unsigned cp = u8next(&p, e);
        int adv = aui_host_text_advance((const char *)st, (int)(p - st), px);
        if (cp > 0x20 && cp != 0x7F) {
            int gap = adv / 6, bw = adv - gap + thick;
            if (bw > adv) bw = adv;
            if (bw < 1) bw = 1;
            for (int j = 0; j < ht; j++)
                for (int i = 0; i < bw; i++) blend(h, x + cur + i, base - ht + j, r->color & 0xFFFFFF, r->alpha);
        }
        cur += adv;
    }
    return 0;
}

static void reg(struct aui_host *h)
{
    (void)h;
    aui_host_on(SYS_TEXT_MEASURE, text_measure);
    aui_host_on(SYS_GUI_TEXT_RUN2, text_run2);
}
AUI_HOST_V2(text, LOGIT_GUI2_F_TEXT_RUN2 | LOGIT_GUI2_F_CARETS, reg)
