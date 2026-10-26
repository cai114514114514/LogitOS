/* Shared paragraph layout (see para.h).  Freestanding, no libc. */
#include "para.h"
#include "linebreak.h"

static unsigned char para_brk[TL_PARA_MAX_BYTES + 1];   /* not reentrant, like linebreak.c */

struct pl {
    const char *s;
    const unsigned short *off;
    const int *x;
    int n;                         /* stop count; stop n-1 is the end */
};

/* Is the cluster ending at stop k (bytes off[k-1]..off[k]) only spaces / line ends? */
static int pl_blank(const struct pl *p, int k)
{
    for (int i = p->off[k - 1]; i < p->off[k]; i++) {
        char c = p->s[i];
        if (c != ' ' && c != '\n' && c != '\r') return 0;
    }
    return 1;
}

/* Stop index after dropping trailing blank clusters of [from, k). */
static int pl_trim(const struct pl *p, int from, int k)
{
    while (k > from && pl_blank(p, k)) k--;
    return k;
}

static int pl_w(const struct pl *p, int from, int k)
{
    return p->x[pl_trim(p, from, k)] - p->x[from];
}

#ifdef TL_PARA_BYTE_CUT
/* NEGATIVE CONTROL: cut inside a multi-byte cluster instead of at a stop. */
static int cut_end(const struct pl *p, int j)
{
    return (j > 0 && p->off[j] - p->off[j - 1] > 1) ? p->off[j] - 1 : p->off[j];
}
static int cut_start(const struct pl *p, int j)
{
    return (j < p->n - 1 && p->off[j + 1] - p->off[j] > 1) ? p->off[j] + 1 : p->off[j];
}
#else
static int cut_end(const struct pl *p, int j)   { return p->off[j]; }
static int cut_start(const struct pl *p, int j) { return p->off[j]; }
#endif

static void put(struct tl_line *l, int start, int end, int width, int kind, int cut_off, int cut_len)
{
    l->start = (unsigned short)start;
    l->len = (unsigned short)(end - start);
    l->width24_8 = width;
    l->x_off24_8 = 0;
    l->ellipsis = (unsigned char)kind;
    l->cut_off = (unsigned short)(kind ? cut_off : end);
    l->cut_len = (unsigned short)(kind ? cut_len : 0);
}

int tl_para_layout(const char *s, int len, const struct tl_para_opts *o,
                   const struct tl_measure_cb *cb, struct tl_line *out, int max_out,
                   unsigned short *scratch_off, int *scratch_x, int scratch_max)
{
    if (!s || len < 0 || !o || !cb || !cb->carets || !out || max_out < 1) return -1;
    if (len > TL_PARA_MAX_BYTES) return -1;
    if (len == 0) { put(&out[0], 0, 0, 0, 0, 0, 0); return 1; }
    if (!scratch_off || !scratch_x || scratch_max < 2) return -1;

    int n = cb->carets(cb->ctx, s, len, o->px, (unsigned)o->face, scratch_off, scratch_x, scratch_max);
    if (n < 2 || n > scratch_max || scratch_off[0] != 0 || scratch_off[n - 1] != len) return -1;
    for (int i = 1; i < n; i++)
        if (scratch_off[i] <= scratch_off[i - 1] || scratch_x[i] < scratch_x[i - 1]) return -1;
    if (tl_break_utf8(s, len, para_brk, 0) < 0) return -1;

    struct pl P = { s, scratch_off, scratch_x, n };
    const struct pl *p = &P;
    const int W = o->width24_8, unlimited = W <= 0;
    const int limit = o->max_lines > 0 ? o->max_lines : 0x7fffffff;
    int ew = -1;                       /* ellipsis width, measured on first use */
    int si = 0, nl = 0;

    while (si < n - 1) {
        if (nl >= max_out) return -1;
        struct tl_line *L = &out[nl];

        if (nl == limit - 1 && o->trunc != TL_TRUNC_NONE) {
            /* Last allowed line: the rest of this hard line, cut to fit. */
            int M = si + 1;
            while (M < n - 1 && para_brk[scratch_off[M]] != TL_BRK_MANDATORY) M++;
            int more = M < n - 1;
            int tk = pl_trim(p, si, M);
            int wtot = p->x[tk] - p->x[si];
            if (unlimited || (wtot <= W && !more)) {
                put(L, scratch_off[si], scratch_off[M], wtot, 0, 0, 0);
                si = M; nl++;
                break;
            }
            if (ew < 0) {
                unsigned short eo[4]; int ex[4];
                int r = cb->carets(cb->ctx, "\xe2\x80\xa6", 3, o->px, (unsigned)o->face, eo, ex, 4);
                if (r < 2 || r > 4 || eo[0] != 0 || eo[r - 1] != 3 || ex[r - 1] < ex[0]) return -1;
                ew = ex[r - 1] - ex[0];
            }
            int budget = W - ew; if (budget < 0) budget = 0;
            int a, b, kind, endb;
            if (wtot <= budget) {              /* all of this line fits; text after it is hidden */
                kind = TL_ELL_TAIL; a = tk; b = M; endb = scratch_off[M];
                put(L, scratch_off[si], endb, wtot + ew, kind, scratch_off[tk], endb - scratch_off[tk]);
            } else if (o->trunc == TL_TRUNC_TAIL) {
                int j = tk;
                while (j > si && p->x[j] - p->x[si] > budget) j--;
                j = pl_trim(p, si, j);
                endb = scratch_off[M];
                int co = cut_end(p, j);
                put(L, scratch_off[si], endb, p->x[j] - p->x[si] + ew, TL_ELL_TAIL, co, endb - co);
            } else if (o->trunc == TL_TRUNC_HEAD) {
                int j = si;
                while (j < tk && p->x[tk] - p->x[j] > budget) j++;
                while (j < tk && pl_blank(p, j + 1)) j++;
                int cs = cut_start(p, j);
                put(L, scratch_off[si], scratch_off[tk], p->x[tk] - p->x[j] + ew, TL_ELL_HEAD,
                    scratch_off[si], cs - scratch_off[si]);
            } else {                           /* MIDDLE */
                a = si;
                for (int k = si; k <= tk && p->x[k] - p->x[si] <= budget / 2; k++) a = k;
                a = pl_trim(p, si, a);
                int tb = budget - (p->x[a] - p->x[si]);
                b = a;
                while (b < tk && p->x[tk] - p->x[b] > tb) b++;
                while (b < tk && pl_blank(p, b + 1)) b++;
                int co = cut_end(p, a), cs = cut_start(p, b);
                put(L, scratch_off[si], scratch_off[tk],
                    (p->x[a] - p->x[si]) + (p->x[tk] - p->x[b]) + ew, TL_ELL_MIDDLE, co, cs - co);
            }
            si = M; nl++;
            break;
        }

        /* Greedy fill over the opportunities; a mandatory break always ends the line. */
        int lb = -1, e = -1;
        for (int k = si + 1; k < n; k++) {
            int brk = para_brk[scratch_off[k]];
            if (brk == TL_BRK_NONE) continue;
            if (unlimited || pl_w(p, si, k) <= W) {
                lb = k;
                if (brk == TL_BRK_MANDATORY) { e = k; break; }
            } else {
                if (lb >= 0) e = lb;
                else {                         /* over-long word: break at a cluster boundary */
                    e = si + 1;
                    for (int j = si + 1; j < k; j++)
                        if (pl_w(p, si, j) <= W) e = j;
                }
                break;
            }
        }
        if (e < 0) e = n - 1;
        put(L, scratch_off[si], scratch_off[e], pl_w(p, si, e), 0, 0, 0);
        si = e; nl++;
        if (nl >= limit) break;
    }

    for (int i = 0; i < nl; i++) {
        int sp = W - out[i].width24_8;
        if (unlimited || sp < 0) sp = 0;
        out[i].x_off24_8 = o->align == TL_ALIGN_C ? sp / 2 : o->align == TL_ALIGN_R ? sp : 0;
    }
    return nl;
}
