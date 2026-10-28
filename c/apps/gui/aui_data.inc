/* aui_data.inc -- chips, kbd chips, squircle tiles, ring gauge, sparkline and
 * the multi-segment bar (design 4.14, 4.15, 3.7.2, 3.7.3).
 * Unity-included by aui.c AFTER aui_form.inc (it shares fd_text, the squircle
 * outline and the gradient emitter) and the path / text / motion code. */

/* ------------------------------------------------------------------ chips */

int aui_chip_w(const char *label, int icon, int on, int flags)
{
    (void)flags;
    int lead = (on || icon >= 0) ? 8 + 12 + 4 : 10;
    return lead + aui_text_measure_w(label ? label : "", 12, AUI_W_MEDIUM) + 10;
}

int aui_chip(int x, int y, const char *label, int icon, int on, int flags)
{
    int id = ++id_ctr, h = (flags & AUI_CHIP_LARGE) ? 28 : 24, w;
    int off = (flags & AUI_CHIP_OFF) != 0, t, tx;
    struct wres r;
    unsigned ink;
    aui_ensure();
    w = aui_chip_w(label, icon, on, flags);
    r = wpoll(id, X_(x), Y_(y), w, h, !off, 1);
    t = aui_anim(AUI_AK_FACE, (r.st & (AUI_HOVER | AUI_ACTIVE)) ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, h / 2);
    if (on) aui_round_a(x, y, w, h, h / 2, aui_t.accent_tint & 0xFFFFFFu, (int)(aui_t.accent_tint >> 24));
    else    aui_round(x, y, w, h, h / 2, aui_t.fill_control);
    if (t > 0 && !off) {
        int a = (int)(aui_t.fill_hover >> 24) * t / 255;
        if (a > 0) aui_round_a(x, y, w, h, h / 2, aui_t.fill_hover & 0xFFFFFFu, a);
    }
    ink = off ? aui_t.label_quaternary : aui_t.label_primary;
    tx = x + 10;
    if (on || icon >= 0) {
        gui_icon(on ? GICON_CHECK : icon, X_(x + 8), Y_(y + (h - 12) / 2), 12,
                 off ? aui_t.label_quaternary : (on ? aui_t.accent_ink : aui_t.label_secondary));
        tx = x + 8 + 12 + 4;
    }
    fd_text(aui_r(tx, y, w - (tx - x) - 10, h), label, 12, AUI_W_MEDIUM, ink, AUI_ALIGN_LEFT, w);
    return r.clicked;
}

int aui_kbd(int x, int y, const char *s)
{
    int w;
    aui_ensure();
    w = imax(18, aui_text_measure_w(s ? s : "", 11, AUI_W_MEDIUM) + 8);
    aui_round(x, y, w, 18, 4, aui_t.fill_control);
    aui_text_vc(aui_r(x, y, w, 18), s, 11, AUI_W_MEDIUM, aui_t.label_secondary, AUI_ALIGN_CENTER);
    return w;
}

/* ------------------------------------------------------------------ tiles */

static void fd_tile(struct aui_rect rc, unsigned top, unsigned bottom, int icon, unsigned icon_rgb, int flags,
                    int icon_px)
{
    if (rc.w <= 0 || rc.h <= 0) return;
    aui_ensure();
    {
        int X = X_(rc.x), Y = Y_(rc.y);
        long long rd = (long long)aui_dev(imin(rc.w, rc.h)) * 256 * 9 / 40;        /* r.icon 0.225 x size */
        if (!path_ok()) {
            aui_vgrad_round(rc.x, rc.y, rc.w, rc.h, imin(rc.w, rc.h) * 9 / 40, top, bottom);
        } else {
            struct ap p;
            unsigned pos[2] = { 0, 65536 }, col[2];
            col[0] = ap_argb(top, 255); col[1] = ap_argb(bottom, 255);
            fd_squircle_ap(&p, X, Y, rc.w, rc.h, rd, 0);
            fd_emit_grad(&p, 2, pos, col, X, Y * 256, (Y + rc.h) * 256, 0, X, Y, rc.w, rc.h, 0);
            if (flags & AUI_TILE_EDGE) {
                struct ap q;
                fd_squircle_ap(&q, X, Y, rc.w, rc.h, rd, 128);
                path_emit(&q, ap_argb(aui_t.sep_opaque, 255), 0, fd_dev1_q8(), 0, X, Y, rc.w, rc.h, 1);
            }
        }
    }
    if (icon >= 0 && icon_px > 0)
        gui_icon(icon, X_(rc.x + (rc.w - icon_px) / 2), Y_(rc.y + (rc.h - icon_px) / 2), icon_px, icon_rgb);
}

void aui_tile(struct aui_rect rect, unsigned top, unsigned bottom, int icon, unsigned icon_rgb, int flags)
{
    fd_tile(rect, top, bottom, icon, icon_rgb, flags, imin(rect.w, rect.h) * 56 / 100);
}

struct aui_tile_colors aui_file_kind_colors(enum aui_file_kind kind)
{
    /* design 3.7.3, tabulated art colours */
    static const struct aui_tile_colors tab[AUI_FK_COUNT] = {
        [AUI_FK_FOLDER] = { 0x6CB8FF, 0x3D8BF5, 0xFFFFFF, GICON_FOLDER, 0 },
        [AUI_FK_TEXT]   = { 0xFFFFFF, 0xEEF0F4, 0x8C8C97, GICON_DOC, AUI_TILE_EDGE },
        [AUI_FK_CODE]   = { 0xB48CFF, 0x8358F0, 0xFFFFFF, GICON_CODE, 0 },
        [AUI_FK_IMAGE]  = { 0x46D6B3, 0x1FA88A, 0xFFFFFF, GICON_IMAGE, 0 },
        [AUI_FK_AUDIO]  = { 0xFF7AA2, 0xF0457A, 0xFFFFFF, GICON_PLAY, 0 },
        [AUI_FK_VIDEO]  = { 0xFFB259, 0xF28A1E, 0xFFFFFF, GICON_PLAY, 0 },
        [AUI_FK_OTHER]  = { 0xB5B5BE, 0x8E8E99, 0xFFFFFF, GICON_DOC, 0 },
    };
    return tab[(unsigned)kind < AUI_FK_COUNT ? (int)kind : AUI_FK_OTHER];
}

void aui_settings_chip(int x, int y, unsigned rgb, int icon)
{
    fd_tile(aui_r(x, y, 20, 20), aui_mix(rgb, 0xFFFFFF, 20), rgb, icon, 0xFFFFFF, 0, 13);
}

/* ------------------------------------------------------------- ring gauge */

void aui_ring_gauge(struct aui_rect rect, int per_mille, int stroke, unsigned rgb,
                    const char *value, const char *caption)
{
    int id = ++id_ctr, d = imin(rect.w, rect.h), cx = rect.x + rect.w / 2, cy = rect.y + rect.h / 2, R, sweep, sm;
    int has_cap = caption && caption[0], vtop;
    (void)id;
    aui_ensure();
    if (d < 16) return;
    stroke = iclamp(stroke, 2, d / 2 - 2);
    R = d / 2 - (stroke + 1) / 2 - 2;             /* the arc's damage halo ends inside the rect */
    sm = aui_spring_ex(AUI_AK_VALUE, iclamp(per_mille, 0, 1000) * 256, AUI_SPRING_SMOOTH);
    sm = iclamp(sm, 0, 1000 * 256);
    aui_arc(cx, cy, R, -90, 360, stroke, ap_argb(aui_t.fill_control, 255));
    sweep = (int)((long long)sm * 360 / (1000 * 256));
    if (sweep >= 1) aui_arc(cx, cy, R, -90, sweep, stroke, ap_argb(rgb, 255));
    if (value && value[0]) {
        int top = cy - (has_cap ? 20 : 12);
        vtop = (aui_text_baseline24(top * 256, 24 * 256, 17) - tx_asc24(17) + 128) >> 8;
        aui_text_tnum(cx, vtop, 17, AUI_W_SEMIBOLD, aui_t.label_primary, value, AUI_ALIGN_CENTER);
        if (has_cap) fd_text(aui_r(cx - R, top + 24, 2 * R, 16), caption, 12, AUI_W_REGULAR, aui_t.label_secondary,
                             AUI_ALIGN_CENTER, 2 * R - 8);
    } else if (has_cap) {
        fd_text(aui_r(cx - R, cy - 8, 2 * R, 16), caption, 12, AUI_W_REGULAR, aui_t.label_secondary,
                AUI_ALIGN_CENTER, 2 * R - 8);
    }
}

/* -------------------------------------------------------------- sparkline */

#define FD_SPARK_MAX 120
static unsigned char fd_sv[FD_SPARK_MAX + 8];
static int fd_sxy[(FD_SPARK_MAX + 8) * 2];

void aui_sparkline_range(struct aui_rect rect, const int *samples, int n, unsigned rgb, int lo, int hi)
{
    int id = ++id_ctr, first, cnt, nv = 0, nxy = 0, px0, py0, pw, ph, bottom24, newest;
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    (void)id;
    if (!samples || n < 2 || rect.w <= 12 || rect.h <= 12) return;
    if (hi <= lo) hi = lo + 1;
    first = n > FD_SPARK_MAX ? n - FD_SPARK_MAX : 0;
    cnt = n - first;
    px0 = rect.x + 5; py0 = rect.y + 5; pw = rect.w - 10; ph = rect.h - 10;
    newest = aui_spring_ex(AUI_AK_VALUE, iclamp(samples[n - 1], lo, hi) * 256, AUI_SPRING_SNAPPY);
    for (int i = 0; i < cnt; i++) {
        long long v24 = (i == cnt - 1) ? newest : (long long)iclamp(samples[first + i], lo, hi) * 256;
        long long x24 = ((long long)px0 * 256) + (long long)pw * 256 * i / (cnt - 1);
        long long y24 = ((long long)(py0 + ph) * 256) - (long long)ph * (v24 - (long long)lo * 256) / (hi - lo);
        fd_sv[nv++] = i == 0 ? LOGIT_VERB_M : LOGIT_VERB_L;
        fd_sxy[nxy++] = (int)x24; fd_sxy[nxy++] = (int)y24;
    }
    if (!path_ok()) {   /* no PATH: one 2 x 2 dot per sample (no stroke, no fill) */
        for (int i = 0; i < nv; i++)
            aui_round(fd_sxy[2 * i] / 256 - 1, fd_sxy[2 * i + 1] / 256 - 1, 2, 2, 1, rgb);
        return;
    }
    bottom24 = (rect.y + rect.h - 2) * 256;   /* + the fill's 1 pt halo stays inside the rect */
    /* the fill: the polyline closed down to the rect's last row, LINEAR @ 77 -> @ 0 */
    {
        unsigned pos[2] = { 0, 65536 }, col[2];
        int fx = fd_sxy[0], lx = fd_sxy[nxy - 2];
        unsigned char fv[FD_SPARK_MAX + 8];
        int fxy[(FD_SPARK_MAX + 8) * 2 + 8], fn = 0, fnv = 0;
        fv[fnv++] = LOGIT_VERB_M; fxy[fn++] = fx; fxy[fn++] = bottom24;
        for (int i = 0; i < nv; i++) { fv[fnv++] = LOGIT_VERB_L; fxy[fn++] = fd_sxy[2 * i]; fxy[fn++] = fd_sxy[2 * i + 1]; }
        fv[fnv++] = LOGIT_VERB_L; fxy[fn++] = lx; fxy[fn++] = bottom24;
        fv[fnv++] = LOGIT_VERB_Z;
        col[0] = ap_argb(rgb, 77); col[1] = ap_argb(rgb, 0);
        gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt);
        pa.verbs = fv; pa.xy = fxy; pa.nverb = (unsigned)fnv; pa.nxy = (unsigned)fn; pa.rule = LOGIT_PATH2_NONZERO;
        pt.kind = LOGIT_PAINT2_LINEAR; pt.x0 = 0; pt.x1 = 0; pt.y0 = rect.y * 256; pt.y1 = (rect.y + rect.h) * 256;
        pt.nstop = 2; pt.stop_pos = pos; pt.stop_argb = col;
        aui_path(&pa, &pt, 0);
    }
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = fd_sv; pa.xy = fd_sxy; pa.nverb = (unsigned)nv; pa.nxy = (unsigned)nxy; pa.rule = LOGIT_PATH2_NONZERO;
    pt.kind = LOGIT_PAINT2_SOLID; pt.argb = ap_argb(rgb, 255);
    st.width = 384; st.cap = LOGIT_STROKE2_CAP_ROUND; st.join = LOGIT_STROKE2_JOIN_ROUND; st.miter = 4 * 65536;
    aui_path(&pa, &pt, &st);
}

void aui_sparkline(struct aui_rect rect, const int *samples, int n, unsigned rgb)
{
    int hi = 1;
    for (int i = 0; samples && i < n; i++) if (samples[i] > hi) hi = samples[i];
    aui_sparkline_range(rect, samples, n, rgb, 0, hi);
}

/* ------------------------------------------------------------- multi-bar */

static int fd_seg_sum(const struct aui_seg *segs, int n)
{
    long long s = 0;
    for (int i = 0; i < n; i++) if (segs[i].value > 0) s += segs[i].value;
    return s > 0x7fffffff ? 0x7fffffff : (int)s;
}

/* Segment i's device span [*x0, *x1) in the bar. Boundaries are cumulative
 * (floor of the running share), so spans never overlap and the gaps are exactly
 * 2 device pixels. */
int aui_multibar_span(struct aui_rect rect, const struct aui_seg *segs, int n, int total, int i, int *x0, int *x1)
{
    int X = X_(rect.x), d0 = aui_dev(X), wd = aui_dev(X + rect.w) - d0, vis = 0, k = 0, idx = 0;
    long long before = 0, avail;
    if (i < 0 || i >= n || segs[i].value <= 0) return 0;
    for (int j = 0; j < n; j++) if (segs[j].value > 0) vis++;
    if (total <= 0) total = fd_seg_sum(segs, n);
    if (total <= 0) return 0;
    for (int j = 0; j < i; j++) if (segs[j].value > 0) { before += segs[j].value; idx++; }
    avail = wd - 2 * (vis - 1);
    if (avail < 1) avail = 1;
    k = idx;
    *x0 = d0 + (int)(before * avail / total) + 2 * k;
    *x1 = d0 + (int)((before + segs[i].value) * avail / total) + 2 * k;
    return *x1 > *x0;
}

void aui_multibar_total(struct aui_rect rect, const struct aui_seg *segs, int n, int total)
{
    int X = X_(rect.x), Y = Y_(rect.y), sum, first = -1, last = -1;
    long long yd0, yd1, R;
    aui_ensure();
    if (!segs || n <= 0 || rect.w <= 0) return;
    sum = fd_seg_sum(segs, n);
    if (total < sum) total = sum;
    if (!path_ok()) {
        aui_round(rect.x, rect.y, rect.w, rect.h, rect.h / 2, aui_t.fill_control);
        for (int i = 0, acc = 0; i < n && total > 0; i++) {
            int x0 = rect.x + (int)((long long)acc * rect.w / total);
            int x1 = rect.x + (int)((long long)(acc + imax(0, segs[i].value)) * rect.w / total);
            acc += imax(0, segs[i].value);
            if (x1 > x0) aui_fill(x0 + (i ? 1 : 0), rect.y, x1 - x0 - (i ? 1 : 0), rect.h, segs[i].rgb);
        }
        return;
    }
    yd0 = (long long)aui_dev(Y) * 256; yd1 = (long long)aui_dev(Y + rect.h) * 256;
    R = (yd1 - yd0) / 2;
    if (sum < total) {
        struct ap p;
        long long rr[4] = { R, R, R, R };
        ap_begin(&p, 1);
        ap_rrect(&p, (long long)aui_dev(X) * 256, yd0, (long long)aui_dev(X + rect.w) * 256, yd1, rr, rr);
        path_emit(&p, ap_argb(aui_t.fill_control, 255), 0, 0, 0, X, Y, rect.w, rect.h, 0);
    }
    for (int i = 0; i < n; i++) if (segs[i].value > 0) { if (first < 0) first = i; last = i; }
    for (int i = 0; i < n; i++) {
        int a, b;
        struct ap p;
        long long rx[4], ry[4];
        if (!aui_multibar_span(rect, segs, n, total, i, &a, &b)) continue;
        rx[0] = rx[3] = (i == first) ? R : 0;
        rx[1] = rx[2] = (i == last) ? R : 0;
        for (int c = 0; c < 4; c++) { if (rx[c] > (long long)(b - a) * 128) rx[c] = (long long)(b - a) * 128; ry[c] = rx[c] > 0 ? R : 0; }
        ap_begin(&p, 1);
        ap_rrect(&p, (long long)a * 256, yd0, (long long)b * 256, yd1, rx, ry);
        path_emit(&p, ap_argb(segs[i].rgb, 255), 0, 0, 0, X, Y, rect.w, rect.h, 0);
    }
}

void aui_multibar(struct aui_rect rect, const struct aui_seg *segs, int n)
{
    aui_multibar_total(rect, segs, n, 0);
}

int aui_multibar_legend(int x, int y, int w, const struct aui_seg *segs, int n)
{
    int cx = x, cy = y, any = 0;
    aui_ensure();
    for (int i = 0; segs && i < n; i++) {
        int lw = aui_text_measure_w(segs[i].label ? segs[i].label : "", 12, AUI_W_REGULAR);
        int iw = 8 + 6 + lw;
        if (any && cx + iw > x + w) { cx = x; cy += 16; }
        aui_round(cx, cy + 4, 8, 8, 2, segs[i].rgb);
        aui_text_vc(aui_r(cx + 14, cy, lw + 2, 16), segs[i].label, 12, AUI_W_REGULAR, aui_t.label_secondary,
                    AUI_ALIGN_LEFT);
        cx += iw + 16;
        any = 1;
    }
    return any ? cy + 16 - y : 0;
}
