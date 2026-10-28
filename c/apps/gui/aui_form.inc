/* aui_form.inc -- form groups and rows, empty state (design 4.13, 4.16).
 * Unity-included by aui.c, AFTER aui_paint.inc / aui_text.inc / aui_button.inc,
 * so it may use the path helpers (ap_*, path_emit's siblings) and the text
 * helpers (tx_asc24) those files define. */

/* ------------------------------------------------------------ shared bits */

/* The squircle outline of aui_squircle() (corner extent 1.25 r, handle 0.34 of
 * it), built in DEVICE 24.8 from window coordinates, inset by `hw` device 24.8
 * on every side so a stroke centred on it keeps its outer edge on the rect. */
static void fd_squircle_ap(struct ap *p, int X, int Y, int w, int h, long long rdev24, long long hw)
{
    long long x0 = (long long)aui_dev(X) * 256 + hw, x1 = (long long)aui_dev(X + w) * 256 - hw;
    long long y0 = (long long)aui_dev(Y) * 256 + hw, y1 = (long long)aui_dev(Y + h) * 256 - hw;
    long long e = (rdev24 - hw) * 5 / 4;
    long long cap = imin((int)((x1 - x0) / 512), (int)((y1 - y0) / 512)) * 256LL;
    long long a;
    if (e > cap) e = cap;
    if (e < 0) e = 0;
    a = e * 87 / 256;
    ap_begin(p, 1);
    ap_m(p, x0 + e, y0);
    ap_l(p, x1 - e, y0);
    ap_c(p, x1 - a, y0, x1, y0 + a, x1, y0 + e);
    ap_l(p, x1, y1 - e);
    ap_c(p, x1, y1 - a, x1 - a, y1, x1 - e, y1);
    ap_l(p, x0 + e, y1);
    ap_c(p, x0 + a, y1, x0, y1 - a, x0, y1 - e);
    ap_l(p, x0, y0 + e);
    ap_c(p, x0, y0 + a, x0 + a, y0, x0 + e, y0);
    ap_z(p);
}

/* Fill (stroke_q8 == 0) or stroke `p` with a vertical LINEAR gradient from
 * window y `ytop` to `ybot` (24.8 points), stops given by pos/argb. Damage is
 * noted for (bx, by, bw, bh) grown by `halo`, hashed with the stops. */
static long fd_emit_grad(const struct ap *p, int nstop, const unsigned *pos, const unsigned *argb,
                         int xwin, int ytop24, int ybot24, int stroke_q8, int bx, int by, int bw, int bh,
                         int halo)
{
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    unsigned sig = 0;
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = p->v; pa.xy = p->xy; pa.nverb = (unsigned)p->nv; pa.nxy = (unsigned)p->nxy;
    pa.rule = LOGIT_PATH2_NONZERO;
    pt.kind = LOGIT_PAINT2_LINEAR;
    pt.x0 = xwin * 256; pt.x1 = xwin * 256; pt.y0 = ytop24; pt.y1 = ybot24;
    pt.nstop = (unsigned char)nstop; pt.stop_pos = pos; pt.stop_argb = argb;
    for (int i = 0; i < nstop; i++) sig = sig * 31u + argb[i] + pos[i];
    if (stroke_q8 > 0) {
        st.width = stroke_q8; st.cap = LOGIT_STROKE2_CAP_BUTT; st.join = LOGIT_STROKE2_JOIN_MITER;
        st.miter = 4 * 65536;
    }
    ad_note_path(bx, by, bw, bh, halo, p->v, p->nv, p->xy, p->nxy,
                 sig ^ (unsigned)ytop24 ^ ((unsigned)ybot24 << 3) ^ (unsigned)stroke_q8);
    return ol_window_path2(&pa, &pt, stroke_q8 > 0 ? &st : 0);
}

/* A 1 device-pixel width in 24.8 points. */
static int fd_dev1_q8(void) { return (int)dev_ceil_pt24(256); }

/* The opaque raised surface of design 4.16 / 3.2: bg.card squircle r 12, E1 in
 * light, and the 1 dev rim (light: #141428 @ 20 outer-ish; dark: #FFFFFF
 * @ 38 -> 10 at 40 % -> 18) on top. Falls back to a circular card without PATH. */
static void fd_card(struct aui_rect rc)
{
    unsigned bg = aui_t.bg_card;
    int dark = aui_is_dark();
    if (rc.w <= 0 || rc.h <= 0) return;
    if (!dark) aui_elev(rc, AUI_FORM_R, LOGIT_E1);
    if (!path_ok()) {
        aui_round(rc.x, rc.y, rc.w, rc.h, AUI_FORM_R, bg);
        aui_stroke(rc.x, rc.y, rc.w, rc.h, AUI_FORM_R, 1, dark ? aui_mix(bg, 0xFFFFFF, 18) : aui_mix(bg, 0x141428, 20));
        return;
    }
    aui_squircle(rc, AUI_FORM_R, ap_argb(bg, 255));
    {
        struct ap p;
        int X = X_(rc.x), Y = Y_(rc.y);
        long long hw = 128;                    /* half a device pixel: the stroke is centred on the outline */
        long long rd = (long long)devlen(X, AUI_FORM_R) * 256;
        unsigned pos[3] = { 0, 26214, 65536 };
        unsigned col[3];
        fd_squircle_ap(&p, X, Y, rc.w, rc.h, rd, hw);
        if (dark) {
            col[0] = ap_argb(0xFFFFFF, 38); col[1] = ap_argb(0xFFFFFF, 10); col[2] = ap_argb(0xFFFFFF, 18);
            fd_emit_grad(&p, 3, pos, col, X, Y * 256, (Y + rc.h) * 256, fd_dev1_q8(), X, Y, rc.w, rc.h, 1);
        } else {
            unsigned a1[2] = { 0, 65536 };
            col[0] = col[1] = ap_argb(0x141428, 20);
            fd_emit_grad(&p, 2, a1, col, X, Y * 256, (Y + rc.h) * 256, fd_dev1_q8(), X, Y, rc.w, rc.h, 1);
        }
    }
}

/* Text of one line vertically centred in `r` on the metric baseline (3.6); a
 * line that does not fit `maxw` is TAIL-elided. */
static void fd_text(struct aui_rect r, const char *s, int px, int weight, unsigned color, int align, int maxw)
{
    if (!s || !s[0] || maxw <= 0) return;
    if (aui_text_measure_w(s, px, weight) <= maxw) {
        r.w = imin(r.w, maxw);
        aui_text_vc(r, s, px, weight, color, align);
        return;
    }
    {   /* same baseline: top = baseline - ascent */
        int top = (aui_text_baseline24(r.y * 256, r.h * 256, px) - tx_asc24(px) + 128) >> 8;
        aui_text_ellipsis_w(r.x, top, maxw, s, px, weight, color, AUI_TRUNC_TAIL);
    }
}

/* ------------------------------------------------------------- form group */

/* -DUI_FORM_NEGCTL (tests/ui-aui-form-data.mk) lays plain rows out 40 pt high:
 * the negative control of 'form-row-h'. */
#ifdef UI_FORM_NEGCTL
#define FD_ROW_PLAIN_H 40
#else
#define FD_ROW_PLAIN_H AUI_FORM_ROW_H
#endif

static struct {
    int open, rows;
    struct aui_rect *col;
    struct aui_rect card;
    int cy, bottom;
} fm;

int aui_form_h(int rows44, int rows56)
{
    return (rows44 < 0 ? 0 : rows44) * AUI_FORM_ROW_H + (rows56 < 0 ? 0 : rows56) * AUI_FORM_ROW_SUB_H;
}

void aui_form_group_begin(const char *label, struct aui_rect *col)
{
    int y = col->y;
    aui_ensure();
    fm.open = 1; fm.rows = 0; fm.col = col;
    if (label && label[0]) {
        fd_text(aui_r(col->x + 4, y, col->w - 4, AUI_FORM_LABEL_H), label, 12, AUI_W_SEMIBOLD,
                aui_t.label_secondary, AUI_ALIGN_LEFT, col->w - 4);
        y += AUI_FORM_LABEL_H + 8;
    }
    fm.card = aui_r(col->x, y, col->w, col->h);
    fm.cy = y;
    fd_card(fm.card);
}

struct aui_rect aui_form_row_ex(const char *title, const char *subtitle, int ctl_w)
{
    int sub = subtitle && subtitle[0];
    int h = sub ? AUI_FORM_ROW_SUB_H : FD_ROW_PLAIN_H;
    struct aui_rect row;
    int tx, tw;
    if (!fm.open) return aui_r(0, 0, 0, 0);
    row = aui_r(fm.card.x, fm.cy, fm.card.w, h);
    if (fm.rows > 0)
        aui_hairline_dev(row.x + AUI_FORM_PAD_X, row.y, row.x + row.w, row.y,
                         ap_argb(aui_t.sep_hairline, (int)(aui_t.sep_hairline >> 24)));
    tx = row.x + AUI_FORM_PAD_X;
    tw = row.w - 2 * AUI_FORM_PAD_X - (ctl_w > 0 ? ctl_w + 12 : 0);
    if (sub) {
        int top = row.y + (h - 38) / 2;          /* 20 title + 2 + 16 subtitle */
        fd_text(aui_r(tx, top, tw, 20), title, 14, AUI_W_REGULAR, aui_t.label_primary, AUI_ALIGN_LEFT, tw);
        fd_text(aui_r(tx, top + 22, tw, 16), subtitle, 12, AUI_W_REGULAR, aui_t.label_secondary, AUI_ALIGN_LEFT, tw);
    } else {
        fd_text(aui_r(tx, row.y, tw, h), title, 14, AUI_W_REGULAR, aui_t.label_primary, AUI_ALIGN_LEFT, tw);
    }
    fm.cy += h;
    fm.rows++;
    return row;
}

struct aui_rect aui_form_row(const char *title, const char *subtitle)
{
    return aui_form_row_ex(title, subtitle, 0);
}

struct aui_rect aui_form_slot(struct aui_rect row, int w, int h)
{
    return aui_r(row.x + row.w - AUI_FORM_PAD_X - w, row.y + (row.h - h) / 2, w, h);
}

int aui_form_row_switch_x(struct aui_rect row)
{
    return row.x + row.w - AUI_FORM_PAD_X - AUI_FORM_SWITCH_W;
}

struct aui_rect aui_form_kv(const char *label, const char *value)
{
    struct aui_rect row;
    if (!fm.open) return aui_r(0, 0, 0, 0);
    row = aui_r(fm.card.x, fm.cy, fm.card.w, AUI_FORM_ROW_H);
    if (fm.rows > 0)
        aui_hairline_dev(row.x + AUI_FORM_PAD_X, row.y, row.x + row.w, row.y,
                         ap_argb(aui_t.sep_hairline, (int)(aui_t.sep_hairline >> 24)));
    fd_text(aui_r(row.x, row.y, AUI_FORM_KV_LABEL_W, row.h), label, 13, AUI_W_REGULAR, aui_t.label_secondary,
            AUI_ALIGN_RIGHT, AUI_FORM_KV_LABEL_W);
    fd_text(aui_r(row.x + AUI_FORM_KV_VALUE_X, row.y, row.w - AUI_FORM_KV_VALUE_X - AUI_FORM_PAD_X, row.h),
            value, 13, AUI_W_REGULAR, aui_t.label_primary, AUI_ALIGN_LEFT,
            row.w - AUI_FORM_KV_VALUE_X - AUI_FORM_PAD_X);
    fm.cy += AUI_FORM_ROW_H;
    fm.rows++;
    return row;
}

void aui_form_group_end(void)
{
    if (!fm.open) return;
    fm.bottom = fm.card.y + fm.card.h;
    if (fm.col) fm.col->y = fm.bottom + AUI_FORM_GAP;
    fm.open = 0;
}

void aui_form_help(const char *text)
{
    int x, y, w, h;
    if (!fm.col || !text || !text[0]) return;
    x = fm.card.x + 4; y = fm.bottom + 8; w = fm.card.w - 8;
    h = aui_text_measure_wrap_lh(w, text, -1, 12, AUI_W_REGULAR, 16, 2);
    aui_text_wrap_lh(aui_r(x, y, w, h), text, -1, 12, AUI_W_REGULAR, aui_t.label_tertiary, 16, 2,
                     AUI_ALIGN_LEFT, AUI_TRUNC_TAIL);
    fm.col->y = y + h + AUI_FORM_GAP;
}

/* ------------------------------------------------------------ empty state */

/* Contrast of two 0xRRGGBB colours x 100, on a gamma-2 luminance (integer only;
 * within ~10 % of WCAG, which is all a >= 3:1 test of an art colour needs). */
static int fd_lum(unsigned c)
{
    long long r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return (int)((2126 * r * r + 7152 * g * g + 722 * b * b) / 10000);
}
static int fd_contrast100(unsigned a, unsigned b)
{
    long long la = fd_lum(a) + 3251, lb = fd_lum(b) + 3251;   /* + 0.05 on a 0..65025 scale */
    if (la < lb) { long long t = la; la = lb; lb = t; }
    return (int)(la * 100 / lb);
}

#define FD_ES_TILE 64
#define FD_ES_BTN_H 28

static int fd_es_body_w(struct aui_rect region) { return imin(300, imax(40, region.w - 32)); }

int aui_empty_state_h(const char *body, const char *primary, const char *secondary)
{
    int h = FD_ES_TILE + 12 + 24, bh = 0;
    if (body && body[0]) bh = aui_text_measure_wrap_lh(300, body, -1, 13, AUI_W_REGULAR, 18, 4);
    h += 4 + bh;
    if ((primary && primary[0]) || (secondary && secondary[0])) h += 16 + FD_ES_BTN_H;
    return h;
}

struct aui_rect aui_empty_state_rect(struct aui_rect region, const char *body, const char *primary,
                                     const char *secondary)
{
    int h = aui_empty_state_h(body, primary, secondary);
    int cy = region.y + region.h * 42 / 100;
    return aui_r(region.x, cy - h / 2, region.w, h);
}

int aui_empty_state(struct aui_rect region, int icon, unsigned identity_rgb, const char *title,
                    const char *body, const char *primary, const char *secondary)
{
    struct aui_rect blk;
    int y, bw = fd_es_body_w(region), bh = 0, ret = 0, cx = region.x + region.w / 2;
    aui_ensure();
    blk = aui_empty_state_rect(region, body, primary, secondary);
    y = blk.y;
    {   /* the tile: identity @ 36, a 32 symbol in accent.ink unless the identity colour reads >= 3:1 */
        struct aui_rect t = aui_r(cx - FD_ES_TILE / 2, y, FD_ES_TILE, FD_ES_TILE);
        unsigned under = aui_mix(aui_t.bg_content, identity_rgb, 36);
        unsigned ink = fd_contrast100(identity_rgb, under) >= 300 ? identity_rgb : aui_t.accent_ink;
        aui_squircle(t, 14, ap_argb(identity_rgb, 36));
        if (icon >= 0) gui_icon(icon, X_(t.x + (t.w - 32) / 2), Y_(t.y + (t.h - 32) / 2), 32, ink);
    }
    y += FD_ES_TILE + 12;
    fd_text(aui_r(region.x, y, region.w, 24), title, 17, AUI_W_SEMIBOLD, aui_t.label_primary, AUI_ALIGN_CENTER,
            region.w - 32);
    y += 24 + 4;
    if (body && body[0]) {
        bh = aui_text_measure_wrap_lh(300, body, -1, 13, AUI_W_REGULAR, 18, 4);
        aui_text_wrap_lh(aui_r(cx - bw / 2, y, bw, bh), body, -1, 13, AUI_W_REGULAR, aui_t.label_secondary, 18, 4,
                         AUI_ALIGN_CENTER, AUI_TRUNC_TAIL);
    }
    y += bh;
    if ((primary && primary[0]) || (secondary && secondary[0])) {
        int pw = (primary && primary[0]) ? imax(72, aui_text_measure_w(primary, 13, AUI_W_MEDIUM) + 32) : 0;
        int sw = (secondary && secondary[0]) ? imax(72, aui_text_measure_w(secondary, 13, AUI_W_MEDIUM) + 32) : 0;
        int total = pw + sw + (pw && sw ? 8 : 0), x = cx - total / 2;
        y += 16;
        if (pw) { if (aui_button_ex(x, y, pw, FD_ES_BTN_H, primary, AUI_V_PRIMARY, 1)) ret = 1; x += pw + 8; }
        if (sw) { if (aui_button_ex(x, y, sw, FD_ES_BTN_H, secondary, AUI_V_SECONDARY, 1)) ret = 2; }
    }
    return ret;
}
