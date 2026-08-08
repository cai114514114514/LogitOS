/* c/lib/video/h264_dpb.c -- pictures as objects: allocation, the decoded
 * picture buffer, picture order counts, reference marking, the two reference
 * lists, implicit bi-prediction weights, the colocated motion field, and the
 * output process.
 *
 * None of this looks at a pixel, which is why it is not in h264.c. It is also
 * where B slices do most of their damage: with B pictures the decode order
 * stops being the display order, so a decoder can reconstruct every frame
 * perfectly and still hand them over in the wrong sequence -- and the result
 * plays, and looks almost right, and is wrong. The output process below is the
 * spec's bumping (C.4.5.3) rather than a heuristic, and the frame COUNT and
 * ORDER are compared against ffmpeg's for every test stream.
 */
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"

static int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------- picture storage -- */
void h264_release_pic(h264dec *d, int i)
{
    pic_t *p = &d->pics[i];
    if (!p->used) return;
    if (p->y) free(p->y - (long)H264_PAD * p->stride_y - H264_PAD);
    free(p->col);
    memset(p, 0, sizeof *p);
}

/* Drop every reference mark. A picture that is still waiting to be OUTPUT
 * survives: an IDR clears the reference lists, it does not throw away frames
 * the caller has not seen yet. Freeing them here is the classic way to lose
 * exactly the frames before each IDR -- a few missing frames per GOP, which a
 * player hides by just showing the next one. */
static void unref_all(h264dec *d)
{
    for (int i = 0; i < MAX_DPB; i++) {
        pic_t *p = &d->pics[i];
        if (!p->used || p == d->cur) continue;
        p->reference = 0;
        if (!p->needed_for_output) h264_release_pic(d, i);
    }
}

int h264_alloc_picture(h264dec *d)
{
    int slot = -1;
    for (int i = 0; i < MAX_DPB; i++)
        if (!d->pics[i].used) { slot = i; break; }
    if (slot < 0) return H264_ERR_OOM;

    int ly = d->stride_y * (d->mbh * 16 + 2 * H264_PAD);
    int lc = d->stride_c * (d->mbh * 8 + 2 * H264_PAD);
    uint8_t *base = (uint8_t *)malloc((size_t)(ly + 2 * lc));
    if (!base) return H264_ERR_OOM;
    memset(base, 0, (size_t)(ly + 2 * lc));    /* deterministic for partial frames */

    pic_t *p = &d->pics[slot];
    p->y = base + (long)H264_PAD * d->stride_y + H264_PAD;
    p->u = base + ly + (long)H264_PAD * d->stride_c + H264_PAD;
    p->v = base + ly + lc + (long)H264_PAD * d->stride_c + H264_PAD;
    p->stride_y = d->stride_y;
    p->stride_c = d->stride_c;
    p->used = 1;
    p->reference = 0;
    p->col = 0;
    d->cur = p;

    /* The per-picture mbinfo array must start ZEROED. A macroblock only writes
     * the fields it owns -- nz[] of uncoded blocks and mv[]/ref_idx[] of intra
     * macroblocks are left alone -- and its neighbours read those back when
     * deriving nC for CAVLC, ctxIdxInc for CABAC and the deblocking boundary
     * strength. This was malloc'd, and since the picture is freed every frame
     * the allocator handed the SAME chunk back on the next one: each frame
     * silently inherited the previous frame's macroblock state. */
    size_t mbbytes = (size_t)d->mbw * d->mbh * sizeof(mbinfo_t);
    d->mb = (mbinfo_t *)malloc(mbbytes);
    if (!d->mb) { h264_release_pic(d, slot); d->cur = 0; return H264_ERR_OOM; }
    memset(d->mb, 0, mbbytes);   /* malloc+memset, not calloc: mini-libc has
                                  * no calloc and this file is headed there */
    return H264_OK;
}

/* Replicate the visible edges into the H264_PAD border (MC pad). */
static void border_pad_plane(uint8_t *vis, int stride, int w, int h)
{
    for (int i = 1; i <= H264_PAD; i++) {
        memcpy(vis - (long)i * stride, vis, (size_t)w);
        memcpy(vis + (long)(h - 1 + i) * stride, vis + (long)(h - 1) * stride, (size_t)w);
    }
    for (int r = -H264_PAD; r < h + H264_PAD; r++) {
        uint8_t *row = vis + (long)r * stride;
        for (int i = 1; i <= H264_PAD; i++) {
            row[-i] = row[0];
            row[w - 1 + i] = row[w - 1];
        }
    }
}

void h264_border_pad(h264dec *d, pic_t *p)
{
    border_pad_plane(p->y, p->stride_y, d->mbw * 16, d->mbh * 16);
    border_pad_plane(p->u, p->stride_c, d->mbw * 8, d->mbh * 8);
    border_pad_plane(p->v, p->stride_c, d->mbw * 8, d->mbh * 8);
}

/* --------------------------------------------------------------------- POC */
int h264_compute_poc(h264dec *d, const slice_t *sl, int nal_ref_idc, int nal_type)
{
    const sps_t *sps = d->cur_sps;
    if (sps->poc_type == 0) {
        int maxlsb = 1 << sps->log2_max_poc_lsb;
        if (nal_type == 5) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
        int lsb = sl->poc_lsb, msb;
        if (lsb < d->prev_poc_lsb && d->prev_poc_lsb - lsb >= maxlsb / 2)
            msb = d->prev_poc_msb + maxlsb;
        else if (lsb > d->prev_poc_lsb && lsb - d->prev_poc_lsb > maxlsb / 2)
            msb = d->prev_poc_msb - maxlsb;
        else
            msb = d->prev_poc_msb;
        if (nal_ref_idc) { d->prev_poc_msb = msb; d->prev_poc_lsb = lsb; }
        int poc = msb + lsb;
        if (d->cur_pps->bottom_field_poc_in_frame) poc += sl->delta_poc_bottom;
        return poc;
    }
    /* poc_type 1 (zero cycle, optionally delta) and 2: decode-order based */
    int fn = d->frame_num_offset + sl->frame_num;
    int poc = 2 * fn - (nal_ref_idc ? 0 : 1);
    if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero)
        poc += sl->delta_poc[0];
    return poc;
}

/* --------------------------------------------------- reference marking --- */
static int find_short(const h264dec *d, int frame_num)
{
    for (int i = 0; i < MAX_DPB; i++)
        if (d->pics[i].used && d->pics[i].reference == 1 &&
            d->pics[i].frame_num == frame_num && &d->pics[i] != d->cur)
            return i;
    return -1;
}

/* Stop using picture i as a reference; free it only if nothing still needs it
 * for output. */
static void unref_pic(h264dec *d, int i)
{
    d->pics[i].reference = 0;
    if (!d->pics[i].needed_for_output) h264_release_pic(d, i);
}

void h264_mark_refs(h264dec *d)
{
    const slice_t *sl = &d->last_slice;
    pic_t *cur = d->cur;
    int maxfn = 1 << d->cur_sps->log2_max_frame_num;

    if (!d->cur_ref) return;

    if (d->cur_idr) {
        unref_all(d);
        if (sl->long_term_reference_flag) {
            cur->reference = 2; cur->lt_idx = 0;
            d->max_long_term_idx = 0;
        } else {
            cur->reference = 1;
        }
        cur->frame_num = sl->frame_num;
        return;
    }

    if (sl->adaptive_marking) {
        for (int c = 0; c < sl->n_mmco; c++) {
            int mmco = sl->mmco[c][0], arg = sl->mmco[c][1];
            if (mmco == 1) {                       /* short-term -> unused */
                int picnum = ((sl->frame_num - (arg + 1)) % maxfn + maxfn) % maxfn;
                int i = find_short(d, picnum);
                if (i >= 0) unref_pic(d, i);
            } else if (mmco == 2) {                /* long-term -> unused */
                for (int i = 0; i < MAX_DPB; i++)
                    if (d->pics[i].used && d->pics[i].reference == 2 &&
                        d->pics[i].lt_idx == arg) unref_pic(d, i);
            } else if (mmco == 3) {                /* short -> long assign */
                int picnum = ((sl->frame_num - ((arg & 0xffff) + 1)) % maxfn + maxfn) % maxfn;
                int i = find_short(d, picnum);
                if (i >= 0) { d->pics[i].reference = 2; d->pics[i].lt_idx = arg >> 16; }
            } else if (mmco == 4) {                /* max_long_term_frame_idx */
                int maxlt = arg - 1;
                for (int i = 0; i < MAX_DPB; i++)
                    if (d->pics[i].used && d->pics[i].reference == 2 &&
                        d->pics[i].lt_idx > maxlt) unref_pic(d, i);
                d->max_long_term_idx = maxlt;
            } else if (mmco == 5) {                /* clear all */
                unref_all(d);
                d->prev_poc_msb = 0; d->prev_poc_lsb = 0;
                d->max_long_term_idx = -1;
            } else if (mmco == 6) {                /* current -> long-term */
                cur->reference = 2; cur->lt_idx = arg >> 16;
                d->max_long_term_idx = arg >> 16;
            }
        }
    }
    if (cur->reference == 0) {
        cur->reference = 1;                        /* sliding window marks short */
        cur->frame_num = sl->frame_num;
    }

    /* sliding window: enforce max_num_ref_frames */
    int maxrefs = clip3(1, 16, d->cur_sps->max_num_ref_frames);
    for (;;) {
        int count = 0;
        for (int i = 0; i < MAX_DPB; i++)
            if (d->pics[i].used && d->pics[i].reference) count++;
        if (count <= maxrefs) break;
        int oldest = -1, odist = -1;
        for (int i = 0; i < MAX_DPB; i++) {
            if (!d->pics[i].used || d->pics[i].reference != 1 ||
                &d->pics[i] == d->cur) continue;
            int dist = ((sl->frame_num - d->pics[i].frame_num) % maxfn + maxfn) % maxfn;
            if (dist > odist) { odist = dist; oldest = i; }
        }
        if (oldest < 0) break;                     /* all long-term: encoder's problem */
        unref_pic(d, oldest);
    }
}

/* ------------------------------------------------- colocated motion ------ */
/* Freeze what a later B picture's direct modes will need from this one
 * (8.4.1.2.1). Called before marking, while the pictures this one referenced
 * are certainly still in the DPB, and only for pictures that can become
 * RefPicList1[0] -- a non-reference picture never can, so it pays nothing. */
void h264_store_colocated(h264dec *d)
{
    if (!d->cur || !d->cur_ref || !d->mb) return;
    size_t n = (size_t)d->mbw * d->mbh;
    colmb_t *col = (colmb_t *)malloc(n * sizeof(colmb_t));
    if (!col) return;                 /* direct modes degrade, decode goes on */
    memset(col, 0, n * sizeof(colmb_t));

    for (size_t a = 0; a < n; a++) {
        const mbinfo_t *m = &d->mb[a];
        colmb_t *c = &col[a];
        if (h264_is_intra_type(m->type)) {
            c->intra = 1;
            for (int b = 0; b < 16; b++) c->ref_poc[b] = COL_NOREF;
            continue;
        }
        for (int b = 0; b < 16; b++) {
            int q = ((b >> 2) >> 1) * 2 + ((b & 3) >> 1);
            /* 8.4.1.2.1: the colocated motion is L0's unless predFlagL0 is 0.
             * Deciding it here, once, is what lets a B picture store one
             * vector per 4x4 instead of the whole two-list state. */
            int list = m->ref_idx[0][q] >= 0 ? 0 : 1;
            int slot = m->ref_pic[list][q];
            if (m->ref_idx[list][q] < 0 || slot < 0 || !d->pics[slot].used) {
                c->ref_poc[b] = COL_NOREF;
                continue;
            }
            c->mv[b][0] = m->mv[list][b][0];
            c->mv[b][1] = m->mv[list][b][1];
            c->ref_poc[b] = d->pics[slot].poc;
            if (d->pics[slot].reference == 2) c->ref_lt |= (uint16_t)(1u << b);
            if (m->ref_idx[list][q] == 0) c->ref_zero |= (uint16_t)(1u << b);
        }
    }
    free(d->cur->col);
    d->cur->col = col;
}

/* ================================================= reference lists ======= */
/* PicNum of a short-term picture relative to the current frame_num (8.2.4.1):
 * a picture with a larger frame_num than the current one wrapped, so it is
 * older, and must sort as a negative number rather than a large positive one. */
static int picnum_of(const pic_t *p, int cur_fn, int maxfn)
{
    return p->frame_num > cur_fn ? p->frame_num - maxfn : p->frame_num;
}

static void sort_by_picnum_desc(pic_t **v, int n, int cur_fn, int maxfn)
{
    for (int i = 1; i < n; i++) {
        pic_t *p = v[i];
        int pn = picnum_of(p, cur_fn, maxfn);
        int j = i - 1;
        while (j >= 0 && picnum_of(v[j], cur_fn, maxfn) < pn) { v[j + 1] = v[j]; j--; }
        v[j + 1] = p;
    }
}

static void sort_by_poc(pic_t **v, int n, int ascending)
{
    for (int i = 1; i < n; i++) {
        pic_t *p = v[i];
        int j = i - 1;
        while (j >= 0 && (ascending ? v[j]->poc > p->poc : v[j]->poc < p->poc)) {
            v[j + 1] = v[j]; j--;
        }
        v[j + 1] = p;
    }
}

static void sort_by_ltidx(pic_t **v, int n)
{
    for (int i = 1; i < n; i++) {
        pic_t *p = v[i];
        int j = i - 1;
        while (j >= 0 && v[j]->lt_idx > p->lt_idx) { v[j + 1] = v[j]; j--; }
        v[j + 1] = p;
    }
}

/* Apply ref_pic_list_modification (8.2.4.3.1) to one list, in place. */
static int apply_reorder(slice_t *sl, int list, pic_t **rl,
                         int nactive, pic_t **st, int nst, pic_t **lt, int nlt,
                         int maxfn)
{
    int pred = sl->frame_num;
    int refIdx = 0;
    for (int c = 0; c < sl->n_reorder[list] && refIdx < nactive; c++) {
        int idc = sl->reorder_cmds[list][c][0], arg = sl->reorder_cmds[list][c][1];
        pic_t *target = 0;
        int picnum = -1;
        if (idc <= 1) {
            pred += idc == 0 ? -(arg + 1) : (arg + 1);
            pred = ((pred % maxfn) + maxfn) % maxfn;
            picnum = pred;
            for (int i = 0; i < nst; i++)
                if (st[i]->frame_num == pred) { target = st[i]; break; }
        } else {                               /* idc == 2: long-term */
            for (int i = 0; i < nlt; i++)
                if (lt[i]->lt_idx == arg) { target = lt[i]; break; }
        }
        if (!target) return -1;

        /* Followed literally. The subtle part is the compaction: it drops
         * other copies of the picture just inserted only from the entries
         * AFTER refIdx, never from the ones before it. That asymmetry is what
         * lets one picture occupy several slots -- removing every copy first
         * (which is the obvious reading) collapses the duplicates and silently
         * hands back the wrong reference. x264's weighted P depends on it. */
        for (int cIdx = nactive; cIdx > refIdx; cIdx--) rl[cIdx] = rl[cIdx - 1];
        rl[refIdx++] = target;
        int nIdx = refIdx;
        for (int cIdx = refIdx; cIdx <= nactive; cIdx++) {
            pic_t *p = rl[cIdx];
            if (!p) continue;
            int keep = (picnum < 0) ? (p != target)
                                    : !(p->reference == 1 && p->frame_num == picnum);
            if (keep) rl[nIdx++] = p;
        }
        for (int i = nIdx; i <= nactive; i++) rl[i] = 0;
    }
    return 0;
}

/* Build RefPicList0 and, for B slices, RefPicList1 (8.2.4.2). */
int h264_build_lists(h264dec *d, slice_t *sl)
{
    int maxfn = 1 << d->cur_sps->log2_max_frame_num;
    int cur_fn = sl->frame_num, cur_poc = d->cur->poc;
    pic_t *st[32], *lt[32];
    int nst = 0, nlt = 0;

    d->n_rl[0] = d->n_rl[1] = 0;
    if (sl->slice_type == SLICE_I) return H264_OK;

    for (int i = 0; i < MAX_DPB; i++) {
        pic_t *p = &d->pics[i];
        if (!p->used || p == d->cur || !p->reference) continue;
        if (p->reference == 1 && nst < 32) st[nst++] = p;
        else if (p->reference == 2 && nlt < 32) lt[nlt++] = p;
    }
    sort_by_ltidx(lt, nlt);

    for (int list = 0; list < (sl->slice_type == SLICE_B ? 2 : 1); list++) {
        /* The list is num_ref_idx_active entries long, which is NOT the same
         * as the number of distinct reference pictures. When there are fewer
         * pictures than that, the tail is undefined until the modification
         * commands fill it -- and a picture is allowed to appear at several
         * indices at once. */
        int nactive = clip3(1, 16, sl->num_ref_idx_active[list]);
        pic_t **rl = d->rl[list];
        pic_t *ord[32];
        int n = 0;

        if (sl->slice_type == SLICE_P) {
            for (int i = 0; i < nst; i++) ord[n++] = st[i];
            sort_by_picnum_desc(ord, n, cur_fn, maxfn);
        } else {
            /* 8.2.4.2.3. L0 walks backwards in time then forwards; L1 does the
             * opposite. Both halves are sorted by POC, not by frame_num --
             * with B pictures those two orders are simply different, and using
             * the P-slice ordering here gives a list that is plausible,
             * usually starts with the right picture, and puts the wrong one at
             * every other index. */
            pic_t *before[32], *after[32];
            int nb = 0, na = 0;
            for (int i = 0; i < nst; i++) {
                if (st[i]->poc < cur_poc) before[nb++] = st[i];
                else                      after[na++] = st[i];
            }
            sort_by_poc(before, nb, 0);        /* descending: nearest first */
            sort_by_poc(after, na, 1);         /* ascending:  nearest first */
            if (list == 0) {
                for (int i = 0; i < nb; i++) ord[n++] = before[i];
                for (int i = 0; i < na; i++) ord[n++] = after[i];
            } else {
                for (int i = 0; i < na; i++) ord[n++] = after[i];
                for (int i = 0; i < nb; i++) ord[n++] = before[i];
            }
        }
        for (int i = 0; i < nlt && n < 32; i++) ord[n++] = lt[i];

        int got = 0;
        for (int i = 0; i < n && got < nactive; i++) rl[got++] = ord[i];
        for (int i = got; i <= nactive; i++) rl[i] = 0;   /* [nactive] is scratch */

        /* 8.2.4.2.3: if L1 came out identical to L0 and has more than one
         * entry, its first two are swapped. Without it every bi-predicted
         * macroblock in a two-reference stream predicts both halves from the
         * same picture. */
        if (list == 1 && got > 1) {
            int same = (got == d->n_rl[0]);
            for (int i = 0; same && i < got; i++) same = (rl[i] == d->rl[0][i]);
            if (same) { pic_t *t = rl[0]; rl[0] = rl[1]; rl[1] = t; }
        }

        if (sl->n_reorder[list] &&
            apply_reorder(sl, list, rl, nactive, st, nst, lt, nlt, maxfn) < 0)
            return H264_ERR_CORRUPT;

        /* A conforming stream defines every active entry; if one is still
         * empty (truncated or corrupt reordering) fall back to the newest
         * reference rather than handing a null picture to motion
         * compensation. */
        for (int i = 0; i < nactive; i++)
            if (!rl[i]) rl[i] = i > 0 ? rl[i - 1] : (got > 0 ? rl[0] : 0);
        if (!rl[0]) return H264_ERR_CORRUPT;
        d->n_rl[list] = nactive;
    }
    return H264_OK;
}

/* ------------------------------------------- implicit bipred weights ----- */
/* weighted_bipred_idc == 2 (8.4.2.3.1). The weights are a function of where
 * the current picture sits between its two references in DISPLAY time, so
 * they are a property of the (refIdxL0, refIdxL1) pair rather than of the
 * slice header -- computed once per slice for every pair the lists can form.
 *
 * bilibili's encoder turns this on, so it is not an exotic path: it is what
 * every bi-predicted macroblock in the stream goes through. */
void h264_calc_implicit_weights(h264dec *d)
{
    int cur = d->cur->poc;
    for (int i = 0; i < d->n_rl[0] && i < 32; i++) {
        for (int j = 0; j < d->n_rl[1] && j < 32; j++) {
            pic_t *p0 = d->rl[0][i], *p1 = d->rl[1][j];
            int w0 = 32, w1 = 32;
            if (p0 && p1) {
                int td = clip3(-128, 127, p1->poc - p0->poc);
                if (td != 0 && p0->reference != 2 && p1->reference != 2) {
                    int tb = clip3(-128, 127, cur - p0->poc);
                    int tx = (16384 + (td < 0 ? -td : td) / 2) / td;
                    int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
                    int w = dsf >> 2;
                    /* Outside this range the spec falls back to the plain
                     * average; the check is not defensive, it is normative. */
                    if (w >= -64 && w <= 128) { w1 = w; w0 = 64 - w; }
                }
            }
            d->impl_w[i][j][0] = w0;
            d->impl_w[i][j][1] = w1;
        }
    }
}

/* ==================================================== output process ===== */
/* Table A-1: MaxDpbMbs by level. Only consulted when the SPS carries no VUI
 * bitstream_restriction -- x264 always writes one, so this is the fallback for
 * streams from encoders that do not. */
static int max_dpb_frames(const sps_t *s)
{
    static const struct { int lvl, mbs; } tab[] = {
        { 10,    396 }, { 11,    900 }, { 12,   2376 }, { 13,   2376 },
        { 20,   2376 }, { 21,   4752 }, { 22,   8100 }, { 30,   8100 },
        { 31,  18000 }, { 32,  20480 }, { 40,  32768 }, { 41,  32768 },
        { 42,  34816 }, { 50, 110400 }, { 51, 184320 }, { 52, 184320 },
        { 60, 696320 }, { 61, 696320 }, { 62, 696320 }
    };
    int mbs = 696320;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (s->level_idc <= tab[i].lvl) { mbs = tab[i].mbs; break; }
    int frame_mbs = s->mb_width * s->mb_height;
    if (frame_mbs <= 0) return 16;
    int n = mbs / frame_mbs;
    return clip3(1, 16, n);
}

/* How many decoded pictures may be held back before one has to come out.
 * Everything about B-frame output order reduces to this number. */
int h264_set_reorder_depth(h264dec *d)
{
    const sps_t *s = d->cur_sps;
    if (!s) return 0;
    if (s->has_bitstream_restriction) d->num_reorder = s->num_reorder_frames;
    else if (s->poc_type == 2)        d->num_reorder = 0;   /* output order == decode order */
    else                              d->num_reorder = max_dpb_frames(s);
    if (d->num_reorder < 0) d->num_reorder = 0;
    if (d->num_reorder > 16) d->num_reorder = 16;
    return d->num_reorder;
}

int h264_pending_output(const h264dec *d)
{
    int n = 0;
    for (int i = 0; i < MAX_DPB; i++)
        if (d->pics[i].used && d->pics[i].needed_for_output &&
            &d->pics[i] != d->cur) n++;
    return n;
}

/* Emit the pending picture with the smallest POC (C.4.5.3). Ties are broken by
 * decode order, which conforming streams never need and corrupt ones do. */
int h264_bump_one(h264dec *d, h264frame *out)
{
    int best = -1;
    for (int i = 0; i < MAX_DPB; i++) {
        pic_t *p = &d->pics[i];
        if (!p->used || !p->needed_for_output || p == d->cur) continue;
        if (best < 0 || p->poc < d->pics[best].poc ||
            (p->poc == d->pics[best].poc && p->output_seq < d->pics[best].output_seq))
            best = i;
    }
    if (best < 0) return 0;

    pic_t *p = &d->pics[best];
    out->width = d->width;
    out->height = d->height;
    out->stride_y = p->stride_y;
    out->stride_c = p->stride_c;
    out->y = p->y;
    out->u = p->u;
    out->v = p->v;
    out->poc = p->poc;
    out->pts = p->pts;
    p->needed_for_output = 0;
    /* The caller may read these planes until its next call into the decoder,
     * so a picture that is now neither referenced nor pending is released
     * THEN, not here. Freeing it immediately and handing back the pointers is
     * a use-after-free that works with most allocators, which is exactly why
     * it survives testing. */
    if (!p->reference) d->pending_free = best;
    return 1;
}
