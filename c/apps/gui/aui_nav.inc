/* aui_nav.inc -- floating sidebar, toolbar, split view, disclosure, inspector
 * (design_language.md 4.1, 4.2, 4.17, 4.18, signature 6.4; ui-aui-nav).
 * Unity-included by aui.c after the widget modules, so it shares the frame
 * state (id_ctr, in, focus, clip stack, translation, the damage recorder) and
 * the paint internals (path_rrect, mat_*, bd_ok) of aui.c / aui_paint.inc.
 *
 * WHY THESE SPRINGS ARE NOT aui_spring_ex(). aui_spring_ex steps a slot by
 * `frame_ms - last_ms` where last_ms is the previous frame's clock, capped at
 * 250 ms. In an app that sleeps on wait_idle(0) the frame before a click or
 * an arrow key is usually long gone, so the first step of a freshly retargeted
 * spring integrates 250 ms -- twice the snappy settle time -- and the pill
 * would teleport instead of gliding whenever the user paused first. nav_spr
 * below is the same spring (ol_spring256, the LOGIT_SPRINGS constants, the
 * design 3.8 settle rule, Reduce Motion snaps, anim_live for the wake
 * contract) with one difference: a step that STARTS motion integrates at most
 * one nominal frame (AUI_ANIM_TICK_MIN), so the glide is visible from the
 * frame that took the input. ui_aui_nav_test.c checks it after a 2 s idle.
 *
 * WHY SOME CALLS DRAW NOTHING. aui_end() diffs this frame's draw calls against
 * last frame's BY INDEX (aui.c 5a-flush), so a call that appears only while
 * hovered shifts every later call and over-reports the whole rest of the
 * frame. The hover washes, the section chevrons and the toolbar scroll-edge
 * hairline are therefore issued EVERY frame as one PATH call each, at alpha 0
 * when invisible (the kernel paints nothing; the recorder still sees the call
 * at its rect), so hovering a sidebar row damages that row and the one it
 * left, nothing else. */

#include "../../include/ui/logit_fmt.h"

#ifdef UI_NAV_NEGCTL
/* THE NEGATIVE CONTROL of make test-ui-aui-nav: the old "blink" selection --
 * a second pill painted straight onto the target row while the first one
 * glides (two pills on screen). ui_aui_nav_test.c must report 'FAIL
 * single-pill' and nothing else. */
#define NAV_TWO_PILLS 1
#else
#define NAV_TWO_PILLS 0
#endif

#if defined(AUI_NO_AA)
#define nav_rt() 1
#else
#define nav_rt() (glass_reduced != 0)
#endif

static int nav_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------ window focus ---- */
static int nav_focus_v = 1;
static unsigned nav_focus_gen;

int aui_nav_focused(void)
{
    if (in.ev == EV_WINDOW_FOCUS) {
        nav_focus_v = in.a != 0;
        nav_focus_gen = anim_gen;
    } else if (nav_focus_gen != anim_gen) {
        long f = _sys(SYS_GUI_WIN_STATE, WINS_FOCUSED, 0, 0);
        nav_focus_v = f != 0;               /* -1 (no answer) keeps the active look */
        nav_focus_gen = anim_gen;
    }
    return nav_focus_v;
}

/* ------------------------------------------------------------ springs ---- */
struct nav_spr {
    struct ol_spring256 sp;
    unsigned last_ms, gen;
    unsigned char init, live;
};

static void nav_spr_set(struct nav_spr *s, int32_t x24)
{
    ol_spring256_init(&s->sp, x24, LOGIT_SPRINGS[LOGIT_SPRING_SNAPPY].k,
                      LOGIT_SPRINGS[LOGIT_SPRING_SNAPPY].c_q8);
    s->init = 1; s->live = 0; s->gen = anim_gen; s->last_ms = frame_ms;
}

/* The spring's position this frame (24.8), moving toward target24 on preset
 * `p` (enum aui_spring == enum logit_spring). Idempotent within a frame: a
 * second call only retargets (and asks for the next frame). */
static int32_t nav_spr_go(struct nav_spr *s, int32_t target24, int p)
{
    if ((unsigned)p >= LOGIT_SPRING_COUNT) p = LOGIT_SPRING_SNAPPY;
#ifdef AUI_ANIM_OFF
    nav_spr_set(s, target24);
    return target24;
#else
    unsigned dt;
    if (!s->init || anim_reduced) { nav_spr_set(s, target24); return target24; }
    s->sp.stiffness = (unsigned short)LOGIT_SPRINGS[p].k;
    s->sp.damping_q8 = (unsigned short)LOGIT_SPRINGS[p].c_q8;
    if (s->gen == anim_gen) {
        if (target24 != s->sp.target) { ol_spring256_retarget(&s->sp, target24); s->live = 1; anim_live++; }
        return s->sp.x;
    }
    dt = frame_ms - s->last_ms;
    if (s->gen + 1 != anim_gen) dt = 0;              /* not drawn last frame */
    if (!s->live) {
        if (target24 == s->sp.target) {
            s->gen = anim_gen; s->last_ms = frame_ms;
            return s->sp.x;
        }
        if (dt > AUI_ANIM_TICK_MIN) dt = AUI_ANIM_TICK_MIN;   /* motion starts now */
    } else if (dt > 250u) dt = 250u;
    if (target24 != s->sp.target) ol_spring256_retarget(&s->sp, target24);
    s->live = 1;
    if (!ol_spring256_step(&s->sp, dt * 1000u)) s->live = 0;
    else if (logit_spring_settled(s->sp.x - s->sp.target, s->sp.v * 1000)) {
        s->sp.x = s->sp.target; s->sp.v = 0; s->sp.xf = s->sp.vf = 0;
        s->live = 0;
    }
    s->gen = anim_gen; s->last_ms = frame_ms;
    if (s->live) anim_live++;
    return s->sp.x;
#endif
}

static int nav_r24(int32_t v24) { return (int)((v24 >= 0 ? v24 + 128 : v24 - 127) / 256); }

/* A timed 0..255 fade (hover washes, chevrons, the scroll-edge hairline):
 * linear, `in_ms` up and `out_ms` down, re-aimed from where it is. */
struct nav_fade { int from, to; unsigned t0; unsigned char init; };

static int nav_fade_go(struct nav_fade *f, int on, int in_ms, int out_ms)
{
    int to = on ? 255 : 0, cur, ms;
    unsigned el;
    if (!f->init) { f->init = 1; f->from = f->to = to; f->t0 = frame_ms; return to; }
    ms = f->to > f->from ? in_ms : out_ms;
    el = frame_ms - f->t0;
    cur = (int)el >= ms || ms <= 0 ? f->to : f->from + (f->to - f->from) * (int)el / ms;
    if (to != f->to) {
        f->from = cur; f->to = to; f->t0 = frame_ms;
        cur = f->from;
    }
    if (cur != f->to) anim_live++;
    return cur;
}

/* ------------------------------------------------------- paint helpers ---- */
static unsigned nav_argb_scale(unsigned argb, int t255)
{
    unsigned a = (argb >> 24) * (unsigned)nav_clampi(t255, 0, 255) / 255u;
    return (a << 24) | (argb & 0xFFFFFFu);
}
static unsigned nav_opaque(unsigned rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }
/* An ARGB over an opaque rgb, opaque: the no-PATH fallback of a translucent ink. */
static unsigned nav_over(unsigned rgb, unsigned argb)
{ return aui_mix(rgb & 0xFFFFFFu, argb & 0xFFFFFFu, (int)(argb >> 24)); }

/* A rounded rect fill that is ALWAYS one call on a PATH kernel, even at alpha
 * 0 (see the file header). Without PATH, nothing at alpha 0. */
static void nav_wash(struct aui_rect rc, const int rad[4], unsigned argb, unsigned under_rgb)
{
    int r[4], k;
    if (rc.w <= 0 || rc.h <= 0) return;
    for (k = 0; k < 4; k++) r[k] = clamp_radius(rc.w, rc.h, rad[k]);
    if (path_ok()) { path_rrect(rc.x, rc.y, rc.w, rc.h, r, 0, argb); return; }
    if ((argb >> 24) == 0) return;
    (void)under_rgb;
    round_impl(rc.x, rc.y, rc.w, rc.h, imin(imin(r[0], r[1]), imin(r[2], r[3])),
               argb & 0xFFFFFFu, (int)(argb >> 24));
}

/* Exactly one device row, the LAST one of the point span ending at y_pt
 * (so the line sits inside a band whose bottom is y_pt), from x0 to x1 points.
 * Always one call on a PATH kernel (alpha 0 included). */
static void nav_hline_last_row(int x0, int x1, int y_pt, unsigned argb)
{
    if (x1 <= x0) return;
    if (!path_ok()) {
        if (argb >> 24) aui_fill_a(x0, y_pt - 1, x1 - x0, 1, argb & 0xFFFFFFu, (int)(argb >> 24));
        return;
    }
    struct ap p;
    long long dx0 = aui_dev(X_(x0)), dx1 = aui_dev(X_(x1)), dy1 = aui_dev(Y_(y_pt)), dy0 = dy1 - 1;
    ap_begin(&p, 0);
    ap_m(&p, dev_ceil_pt24(dx0 * 256), dev_ceil_pt24(dy0 * 256));
    ap_l(&p, dev_ceil_pt24(dx1 * 256), dev_ceil_pt24(dy0 * 256));
    ap_l(&p, dev_ceil_pt24(dx1 * 256), dev_ceil_pt24(dy1 * 256));
    ap_l(&p, dev_ceil_pt24(dx0 * 256), dev_ceil_pt24(dy1 * 256));
    ap_z(&p);
    path_emit(&p, argb, 1, 0, 0, X_(x0), Y_(y_pt - 1), x1 - x0, 1, 1);
}

/* A 1 dev-px INNER outline of a rounded rect, painted with `pt` (solid or
 * linear). The geometry is path_rrect's (outer edge on the rect). */
static void nav_rim(struct aui_rect rc, int radius, const struct logit_paint2 *pt, unsigned fallback_rgb)
{
    int r = clamp_radius(rc.w, rc.h, radius);
    if (rc.w <= 0 || rc.h <= 0) return;
    if (!path_ok()) { aui_stroke(rc.x, rc.y, rc.w, rc.h, r, 1, fallback_rgb); return; }
    {
        struct ap p;
        struct logit_path2 pa;
        struct logit_paint2 paint = *pt;
        struct logit_stroke2 st;
        long long rx[4], ry[4];
        int X = X_(rc.x), Y = Y_(rc.y), k;
        int sq8 = (int)dev_ceil_pt24(256);                   /* one device pixel */
        long long x0 = (long long)aui_dev(X) * 256, x1 = (long long)aui_dev(X + rc.w) * 256;
        long long y0 = (long long)aui_dev(Y) * 256, y1 = (long long)aui_dev(Y + rc.h) * 256;
        long long hw = ap_fdiv((long long)sq8 * aui_scale(), 100) / 2;
        int ax[4] = { X, X + rc.w - r, X + rc.w - r, X };
        int ay[4] = { Y, Y, Y + rc.h - r, Y + rc.h - r };
        unsigned sig = paint.argb;
        x0 += hw; y0 += hw; x1 -= hw; y1 -= hw;
        for (k = 0; k < 4; k++) {
            rx[k] = (long long)devlen(ax[k], r) * 256 - hw;
            ry[k] = (long long)devlen(ay[k], r) * 256 - hw;
            if (rx[k] < 0 || ry[k] < 0) rx[k] = ry[k] = 0;
        }
        ap_begin(&p, 1);
        ap_rrect(&p, x0, y0, x1, y1, rx, ry);
        gfx_zero(&pa, sizeof pa); gfx_zero(&st, sizeof st);
        pa.verbs = p.v; pa.xy = p.xy; pa.nverb = (unsigned)p.nv; pa.nxy = (unsigned)p.nxy;
        pa.rule = LOGIT_PATH2_NONZERO;
        st.width = sq8; st.cap = LOGIT_STROKE2_CAP_BUTT; st.join = LOGIT_STROKE2_JOIN_MITER;
        st.miter = 4 * 65536;
        for (k = 0; k < paint.nstop && paint.stop_argb; k++) AD_MIX(sig, paint.stop_argb[k]);
        AD_MIX(sig, paint.kind); AD_MIX(sig, paint.y0); AD_MIX(sig, paint.y1);
        ad_note_path(X, Y, rc.w, rc.h, 1, p.v, p.nv, p.xy, p.nxy, sig ^ (unsigned)sq8);
        ol_window_path2(&pa, &paint, &st);
    }
}

static void nav_rim_solid(struct aui_rect rc, int radius, unsigned argb, unsigned under_rgb)
{
    struct logit_paint2 pt;
    gfx_zero(&pt, sizeof pt);
    pt.kind = LOGIT_PAINT2_SOLID; pt.argb = argb;
    nav_rim(rc, radius, &pt, nav_over(under_rgb, argb));
}

/* The dark rim of 3.2: #FFFFFF @ 38 at the top, @ 10 at 40 %, @ 18 at the bottom. */
static void nav_rim_dark(struct aui_rect rc, int radius, unsigned under_rgb)
{
    static const unsigned pos[3] = { 0, 26214, 65536 };
    static const unsigned col[3] = { 0x26FFFFFFu, 0x0AFFFFFFu, 0x12FFFFFFu };
    struct logit_paint2 pt;
    gfx_zero(&pt, sizeof pt);
    pt.kind = LOGIT_PAINT2_LINEAR; pt.nstop = 3;
    pt.stop_pos = pos; pt.stop_argb = col; pt.argb = col[0];
    pt.x0 = pt.x1 = X_(rc.x) * 256;
    pt.y0 = Y_(rc.y) * 256; pt.y1 = Y_(rc.y + rc.h) * 256;
    nav_rim(rc, radius, &pt, nav_over(under_rgb, 0x18FFFFFFu));
}

/* The 3 pt accent.focusRing, 1 pt outside the control (4 intro). */
static void nav_focus_ring(struct aui_rect rc, int radius)
{
    if (!(focus_vis)) return;
    aui_stroke_a(aui_r(rc.x - 4, rc.y - 4, rc.w + 8, rc.h + 8), radius + 4, 3 * 256,
                 aui_tok()->accent_focus_ring);
}

/* A stroked chevron `size` points square at (x, y), pointing DOWN at 0 and
 * turned by deg24 (24.8 degrees, clockwise positive), in argb. Always one
 * call on a PATH kernel; without PATH the nearest GICON (nothing at alpha 0). */
static void nav_chevron(int x, int y, int size, int32_t deg24, unsigned argb)
{
    if (!path_ok()) {
        if ((argb >> 24) == 0) return;
        int d = nav_r24(deg24);
        int id = d <= -45 ? GICON_CHEVRON_R : d >= 45 ? GICON_CHEVRON_L : GICON_CHEVRON_D;
        gui_icon(id, X_(x), Y_(y), size, argb & 0xFFFFFFu);
        return;
    }
    /* icons.c ic_chev_d in its 0..100 box: (20,38) (50,68) (80,38) */
    static const int pts[6] = { 20, 38, 50, 68, 80, 38 };
    long long c = gfx_cos((int)deg24), s = gfx_sin((int)deg24);
    long long cx = (long long)X_(x) * 256 + size * 128, cy = (long long)Y_(y) * 256 + size * 128;
    struct ap p;
    ap_begin(&p, 0);
    for (int i = 0; i < 3; i++) {
        long long dx = (long long)(pts[2 * i] - 50) * size * 256 / 100;
        long long dy = (long long)(pts[2 * i + 1] - 50) * size * 256 / 100;
        long long rx = cx + ((dx * c - dy * s) >> 16), ry = cy + ((dx * s + dy * c) >> 16);
        if (i == 0) ap_m(&p, rx, ry); else ap_l(&p, rx, ry);
    }
    path_emit(&p, argb, 0, size * 256 * 14 / 100, 1, X_(x), Y_(y), size, size, 1);
}

/* Integer top of a run of `px` metric-centred (3.6) in a box at y, h points. */
static int nav_text_top(int y, int h, int px)
{
    int t24 = aui_text_baseline24(y * 256, h * 256, px) - tx_asc24(px);
    return nav_r24(t24);
}
/* Top of a run of `px` whose baseline is that of `ref_px` centred in (y, h). */
static int nav_text_top_on(int y, int h, int ref_px, int px)
{
    int b24 = aui_text_baseline24(y * 256, h * 256, ref_px);
    return nav_r24(b24 - tx_asc24(px));
}

/* ================================================================ 4.1 ==== */
#define NAV_SB_MAX   2
#define NAV_ROWS     48
#define NAV_SECS     16
#define NAV_LBL      64
#define NAV_MAT_MS   120
#define NAV_TYPE_MS  800
#define NAV_HDR_H    16

struct nav_row {
    int y, icon, count, sec;
    unsigned flags;
    unsigned char st, can;          /* st: AUI_HOVER / AUI_ACTIVE; can: hittable */
    char label[NAV_LBL];
};
struct nav_sec {
    int y, rows_y, nrows, collapsible, h;   /* h: animated content height, points */
    unsigned char hover;
    char title[32];
    struct nav_spr hs, rot;
    struct nav_fade chev;
};
struct nav_sb {
    int id, used;
    unsigned gen;
    struct aui_rect r;              /* this frame, current coordinates */
    int wx, wy, W;                  /* window-local origin, live width */
    unsigned flags;
    int wid;
    struct nav_spr w;
    int w_target, drag, grab;
    struct nav_spr pill;
    int sel;                        /* the selected row, last frame */
    int nrows, nsec, cur_y, y0, sec_cur;
    int clicked, key_pick, key_open, sel_now;
    char tbuf[16];
    int tlen;
    unsigned tms;
    unsigned collapsed;
    struct nav_row rows[NAV_ROWS];
    struct nav_sec secs[NAV_SECS];
    /* hover washes: slot 0 follows the pointer, slot 1 fades out where it left */
    int hv[2], hv_a0[2];
    unsigned hv_t0[2];
    struct aui_rect hv_rect[2];
    /* material crossfade, 0 = solid .. 256 = glass */
    int mat_init, mat_to, mat_from;
    unsigned mat_t0;
    struct aim_ctx aim;
};
static struct nav_sb nav_sbs[NAV_SB_MAX];
static struct nav_sb *nav_cur;

static struct nav_sb *nav_sb_find(int id)
{
    for (int i = 0; i < NAV_SB_MAX; i++) if (nav_sbs[i].used && nav_sbs[i].id == id) return &nav_sbs[i];
    return 0;
}
static struct nav_sb *nav_sb_get(int id)
{
    struct nav_sb *s = nav_sb_find(id), *old = &nav_sbs[0];
    if (s) return s;
    for (int i = 0; i < NAV_SB_MAX; i++) {
        if (!nav_sbs[i].used) { old = &nav_sbs[i]; break; }
        if (nav_sbs[i].gen < old->gen) old = &nav_sbs[i];
    }
    gfx_zero(old, sizeof *old);
    old->id = id; old->used = 1; old->sel = -1;
    old->hv[0] = old->hv[1] = -1;
    return old;
}

int aui_sidebar_width(int id)
{
    struct nav_sb *s = nav_sb_find(id);
    return s ? s->W : 0;
}

static int nav_sb_pickable(const struct nav_sb *sb, int i)
{
    const struct nav_row *r = &sb->rows[i];
    return r->can && !(r->flags & (AUI_SB_ROW_OFF | AUI_SB_ROW_ACTION));
}

static int nav_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int nav_prefix(const char *label, const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        if (!label[i]) return 0;
        if (nav_lower((unsigned char)label[i]) != nav_lower((unsigned char)buf[i])) return 0;
    }
    return 1;
}

/* ↑ ↓ ⏎ and type-to-select, against LAST frame's rows (their order is call
 * order, which is this frame's too). */
static void nav_sb_keys(struct nav_sb *sb)
{
    int a, n = sb->nrows, cur = sb->sel;
    if (focus_id != sb->wid || in.ev != EV_KEY || in.key_used || n <= 0) return;
    a = in.a;
    if (a == KEY_UP || a == KEY_DOWN) {
        int d = a == KEY_UP ? -1 : 1, i = cur < 0 ? (d > 0 ? -1 : n) : cur;
        for (i += d; i >= 0 && i < n; i += d)
            if (nav_sb_pickable(sb, i)) { sb->key_pick = i; break; }
        in.key_used = 1;
        sb->tlen = 0;
        return;
    }
    if (a == '\n' || a == '\r') {
        if (cur >= 0 && cur < n) sb->key_open = cur;
        in.key_used = 1;
        return;
    }
    if ((a > ' ' && a < 0x7F) || (a == ' ' && sb->tlen > 0)) {
        int start;
        if (frame_ms - sb->tms > NAV_TYPE_MS) sb->tlen = 0;
        if (sb->tlen < (int)sizeof sb->tbuf - 1) sb->tbuf[sb->tlen++] = (char)a;
        sb->tms = frame_ms;
        /* a fresh single key cycles past the current row; a longer prefix
         * keeps it if it still matches */
        start = cur < 0 ? 0 : (sb->tlen == 1 ? cur + 1 : cur);
        for (int k = 0; k < n; k++) {
            int i = (start + k) % n;
            if (nav_sb_pickable(sb, i) && nav_prefix(sb->rows[i].label, sb->tbuf, sb->tlen)) {
                sb->key_pick = i;
                break;
            }
        }
        in.key_used = 1;
    }
}

/* The panel: shadow, material, edge (the one rrect rule of 3.4). */
static void nav_sb_container(struct nav_sb *sb, struct aui_rect C)
{
    const struct logit_tokens *t = aui_tok();
    const struct logit_material *m = logit_material(LOGIT_MAT_SIDEBAR);
    static const int r12[4] = { AUI_SB_RADIUS, AUI_SB_RADIUS, AUI_SB_RADIUS, AUI_SB_RADIUS };
    int dark = theme_dark, rt = nav_rt(), g, want;
    unsigned el;
    want = !(sb->flags & AUI_SB_SOLID) && !rt && aui_nav_focused() &&
           cg_has(LOGIT_GUI2_F_BACKDROP_CACHE) && bd_ok();
    want = want ? 256 : 0;
    if (!sb->mat_init || rt) {
        sb->mat_init = 1; sb->mat_from = sb->mat_to = want; sb->mat_t0 = frame_ms;
    } else if (want != sb->mat_to) {
        el = frame_ms - sb->mat_t0;
        g = (int)el >= NAV_MAT_MS ? sb->mat_to
                                  : sb->mat_from + (sb->mat_to - sb->mat_from) * (int)el / NAV_MAT_MS;
        sb->mat_from = g; sb->mat_to = want; sb->mat_t0 = frame_ms;
    }
    el = frame_ms - sb->mat_t0;
    if ((int)el >= NAV_MAT_MS || sb->mat_from == sb->mat_to) g = sb->mat_to;
    else { g = sb->mat_from + (sb->mat_to - sb->mat_from) * (int)el / NAV_MAT_MS; anim_live++; }

    if (!dark) aui_elev(C, AUI_SB_RADIUS, LOGIT_E1);
    if (g >= 256) {
        aui_backdrop(1, C, AUI_SB_RADIUS, LOGIT_MAT_SIDEBAR);
    } else if (g > 0 && bd_ok()) {
        /* mid-crossfade: the region's tint runs from the opaque solid to the
         * material, so the WM itself fades the blur in (app ink cannot be
         * translucent over a region: it is blended against fill_rgb) */
        unsigned solid = nav_opaque(t->bg_sidebar), tint = dark ? m->tint_d : m->tint_l, mix = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            int a = (int)((solid >> sh) & 255), b = (int)((tint >> sh) & 255);
            mix |= (unsigned)(a + (b - a) * g / 256) << sh;
        }
        {
            struct logit_backdrop2 b;
            int X = X_(C.x), Y = Y_(C.y), q[5], rad[4], k, rr = mat_radius(C, AUI_SB_RADIUS, m);
            unsigned fill = (dark ? m->rt_d : m->rt_l) & 0xFFFFFFu;
            mat_geom(X, Y, C.w, C.h, rr, q);
            mat_radii(q[4], m->cut, rad);
            gfx_zero(&b, sizeof b);
            b.id = 1;
            b.x = q[0]; b.y = q[1]; b.w = q[2]; b.h = q[3];
            for (k = 0; k < 4; k++) b.radius[k] = rad[k];
            b.tint_argb = mix & 0xFFFFFFu;
            b.opacity = (int)(mix >> 24); b.tone = 0;
            b.blur_level = m->tier; b.rim = MAT_RIM;
            b.fill_rgb = fill;
            b.flags = LOGIT_RECT_SNAP;
            if (ol_window_backdrop2(&b) == 0) {
                bd_live |= 1u << 1;
                ad_note_mat(X, Y, C.w, C.h, rr, m->tier, mat_sig(0x6264u + 1u, rr, mix, fill, m->tier), 0);
            } else g = 0;
        }
    } else g = 0;
    if (g <= 0) {
        if (bd_live & (1u << 1)) aui_backdrop_clear(1);
        aui_round4(C, r12, nav_opaque(t->bg_sidebar));
    }
    if (dark) nav_rim_dark(C, AUI_SB_RADIUS, t->bg_sidebar);
    else nav_rim_solid(C, AUI_SB_RADIUS, t->sep_hairline, t->bg_sidebar);
}

void aui_sidebar_begin(int id, struct aui_rect r, unsigned flags)
{
    struct nav_sb *sb = nav_sb_get(id);
    struct wres st;
    int W, div;
    sb->wid = ++id_ctr;
    if (foc_n < 128) foc_ids[foc_n++] = sb->wid;
    if (!sb->w.init) {
        sb->w_target = nav_clampi(r.w > 0 ? r.w : AUI_SB_W_DEFAULT, AUI_SB_W_MIN, AUI_SB_W_MAX);
        nav_spr_set(&sb->w, sb->w_target << 8);
    }
    sb->key_pick = sb->key_open = -1;
    nav_sb_keys(sb);
    sb->r = r; sb->flags = flags;
    sb->wx = X_(r.x); sb->wy = Y_(r.y);

    /* the divider: 6 pt right of the panel, live while dragged, then the
     * nearest 4 pt on snappy */
    W = nav_r24(sb->w.sp.x);
    div = ++id_ctr;
    st = wpoll(div, sb->wx + W, sb->wy, AUI_SB_DIVIDER, r.h, 1, 0);
    if (st.clicked) { sb->drag = 1; sb->grab = in.mx - (sb->wx + W); }
    if (sb->drag) {
        if (in.active == div && in.down) {
            int live = nav_clampi(in.mx - sb->wx - sb->grab, AUI_SB_W_MIN, AUI_SB_W_MAX);
            nav_spr_set(&sb->w, live << 8);
            sb->w_target = live;
        } else {
            sb->drag = 0;
            sb->w_target = nav_clampi((W + 2) / 4 * 4, AUI_SB_W_MIN, AUI_SB_W_MAX);
        }
    }
    if (!sb->drag) nav_spr_go(&sb->w, sb->w_target << 8, LOGIT_SPRING_SNAPPY);
    sb->W = W = nav_r24(sb->w.sp.x);
    sb->gen = anim_gen;

    nav_sb_container(sb, aui_r(r.x, r.y, W, r.h));

    sb->nrows = 0; sb->nsec = 0; sb->y0 = sb->cur_y = 12; sb->sec_cur = -1;
    sb->clicked = -1; sb->sel_now = -1;
    aim_take(&sb->aim);
    nav_cur = sb;
}

struct aui_rect aui_sidebar_search_slot(void)
{
    struct nav_sb *sb = nav_cur;
    if (!sb) return aui_r(0, 0, 0, 0);
    if (!sb->nrows && !sb->nsec) sb->y0 = sb->cur_y = 50;
    return aui_r(sb->r.x + 12, sb->r.y + 10, sb->W - 24, 28);
}

/* Close the open section: its content height rides `smooth` toward its rows
 * (0 when collapsed), and what follows starts below that. */
static void nav_sec_close(struct nav_sb *sb)
{
    struct nav_sec *s;
    int full, tgt;
    if (sb->sec_cur < 0) return;
    s = &sb->secs[sb->sec_cur];
    full = s->nrows * AUI_SB_PITCH;
    tgt = (sb->collapsed >> sb->sec_cur) & 1 ? 0 : full;
    if (!s->hs.init) nav_spr_set(&s->hs, tgt << 8);
    s->h = nav_clampi(nav_r24(nav_spr_go(&s->hs, tgt << 8, LOGIT_SPRING_SMOOTH)), 0, full);
    sb->cur_y = s->rows_y + s->h;
    sb->sec_cur = -1;
}

int aui_sidebar_section(const char *title, int collapsible)
{
    struct nav_sb *sb = nav_cur;
    struct nav_sec *s;
    struct wres st;
    int i, wid, k;
    if (!sb || sb->nsec >= NAV_SECS) return 1;
    nav_sec_close(sb);
    i = sb->nsec++;
    s = &sb->secs[i];
    s->y = sb->cur_y + ((i == 0 && sb->nrows == 0) ? 8 : 20);
    s->rows_y = s->y + NAV_HDR_H + 6;
    s->nrows = 0;
    s->collapsible = collapsible;
    for (k = 0; title && title[k] && k < (int)sizeof s->title - 1; k++) s->title[k] = title[k];
    if (title && title[k]) while (k > 0 && ((unsigned char)title[k] & 0xC0) == 0x80) k--;   /* UTF-8 cut */
    s->title[k] = 0;
    wid = ++id_ctr;
    st = wpoll(wid, sb->wx, sb->wy + s->y - 4, sb->W, NAV_HDR_H + 8,
               collapsible && s->y + NAV_HDR_H <= sb->r.h, 0);
    s->hover = (st.st & AUI_HOVER) != 0;
    if (st.clicked && collapsible) sb->collapsed ^= 1u << i;
    if (!collapsible) sb->collapsed &= ~(1u << i);
    sb->sec_cur = i;
    sb->cur_y = s->rows_y;
    return !((sb->collapsed >> i) & 1);
}

int aui_sidebar_row(int icon_or_chip, const char *label, int count, int selected, unsigned flags)
{
    struct nav_sb *sb = nav_cur;
    struct nav_row *rw;
    struct wres st;
    int i, y, can, wid, res = 0, k;
    if (!sb || sb->nrows >= NAV_ROWS) return 0;
    i = sb->nrows++;
    rw = &sb->rows[i];
    if (sb->sec_cur >= 0) {
        struct nav_sec *s = &sb->secs[sb->sec_cur];
        y = s->rows_y + s->nrows * AUI_SB_PITCH;
        s->nrows++;
        /* hittable only when fully shown: open, not mid-animation past it */
        can = !((sb->collapsed >> sb->sec_cur) & 1) &&
              (!s->hs.init || y + AUI_SB_PITCH <= s->rows_y + nav_r24(s->hs.sp.x));
        rw->sec = sb->sec_cur;
    } else {
        y = sb->cur_y;
        sb->cur_y += AUI_SB_PITCH;
        can = 1;
        rw->sec = -1;
    }
    can = can && y >= 0 && y + AUI_SB_PITCH <= sb->r.h;
    rw->y = y; rw->icon = icon_or_chip; rw->count = count; rw->flags = flags; rw->can = (unsigned char)can;
    for (k = 0; label && label[k] && k < NAV_LBL - 1; k++) rw->label[k] = label[k];
    if (label && label[k]) while (k > 0 && ((unsigned char)label[k] & 0xC0) == 0x80) k--;
    rw->label[k] = 0;
    wid = ++id_ctr;
    st = wpoll(wid, sb->wx, sb->wy + y, sb->W, AUI_SB_PITCH, can && !(flags & AUI_SB_ROW_OFF), 0);
    rw->st = (unsigned char)(st.st & (AUI_HOVER | AUI_ACTIVE));
    if (st.clicked) {
        res = AUI_SB_PICK;
        focus_id = sb->wid; focus_vis = 0;
        if (!(flags & AUI_SB_ROW_ACTION)) sb->clicked = i;
    }
    if (i == sb->key_pick) res = AUI_SB_PICK;
    if (i == sb->key_open) res = AUI_SB_OPEN;
    if (selected && sb->sel_now < 0) sb->sel_now = i;
    return res;
}

/* The row pill / wash rect, sidebar-local. */
static struct aui_rect nav_pill_rect(const struct nav_sb *sb, int y)
{
    return aui_r(sb->r.x + AUI_SB_PILL_INSET, sb->r.y + y + 1, sb->W - 2 * AUI_SB_PILL_INSET, AUI_SB_PILL_H);
}

/* Clip to a section's visible rows while it animates; 1 if a clip was pushed. */
static int nav_sec_clip(const struct nav_sb *sb, int sec)
{
    const struct nav_sec *s;
    if (sec < 0) return 0;
    s = &sb->secs[sec];
    if (s->h >= s->nrows * AUI_SB_PITCH) return 0;
    clip_push(aui_r(X_(sb->r.x), Y_(sb->r.y + s->rows_y), sb->W, s->h));
    return 1;
}

static void nav_count_text(char *buf, int cap, int v)
{
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    logit_fmtw_u(&w, (uint64_t)(v < 0 ? 0 : v), 1);
    logit_fmtw_end(&w);
}

static void nav_sb_draw_row(const struct nav_sb *sb, int i, int eff, int focused)
{
    const struct logit_tokens *t = aui_tok();
    const struct nav_row *rw = &sb->rows[i];
    int X0 = sb->r.x, Y0 = sb->r.y + rw->y, W = sb->W;
    int sel = i == eff, off = (rw->flags & AUI_SB_ROW_OFF) != 0, lx = 40, right = W - AUI_SB_PILL_INSET - 8;
    int act = (rw->flags & AUI_SB_ROW_ACTION) != 0;
    /* accent symbols (4.1); in dark the accent.ink twin 3.7.1 allows, which
     * keeps a 16 pt stroke legible on bg.sidebar #26262D */
    unsigned acc = theme_dark ? t->accent_ink : t->accent;
    unsigned sym = off ? t->label_quaternary
                 : act ? t->label_tertiary
                 : sel ? (focused ? acc : t->label_secondary)
                 : (sb->flags & AUI_SB_TINT) ? (focused ? acc : t->label_secondary)
                 : (rw->st & AUI_HOVER) ? t->label_primary : t->label_secondary;
    unsigned ink = off ? t->label_quaternary : act ? t->label_tertiary : t->label_primary;
    if (rw->icon >= 0 && (rw->icon & 0x40000000)) {
        /* a 20 pt chip centred at x 22: squircle, top +8 % lighter, white symbol 13 */
        unsigned fill = (unsigned)rw->icon & 0xFFFFFFu;
        int cx = X0 + 12, cy = Y0 + (AUI_SB_PITCH - 20) / 2;
        int ic = (rw->icon >> 24) & 0x3F;
        if (path_ok()) {
            static unsigned pos[2] = { 0, 65536 };
            unsigned col[2];
            struct logit_paint2 pt;
            struct ap p;
            int X = X_(cx), Y = Y_(cy);
            long long x0 = (long long)aui_dev(X) * 256, x1 = (long long)aui_dev(X + 20) * 256;
            long long y0 = (long long)aui_dev(Y) * 256, y1 = (long long)aui_dev(Y + 20) * 256;
            long long e = (long long)devlen(X, 5) * 256 * 5 / 4, a = e * 87 / 256;
            struct logit_path2 pa;
            col[0] = nav_opaque(aui_mix(fill, 0xFFFFFF, 20)); col[1] = nav_opaque(fill);
            ap_begin(&p, 1);
            ap_m(&p, x0 + e, y0); ap_l(&p, x1 - e, y0); ap_c(&p, x1 - a, y0, x1, y0 + a, x1, y0 + e);
            ap_l(&p, x1, y1 - e); ap_c(&p, x1, y1 - a, x1 - a, y1, x1 - e, y1);
            ap_l(&p, x0 + e, y1); ap_c(&p, x0 + a, y1, x0, y1 - a, x0, y1 - e);
            ap_l(&p, x0, y0 + e); ap_c(&p, x0, y0 + a, x0 + a, y0, x0 + e, y0); ap_z(&p);
            gfx_zero(&pt, sizeof pt); gfx_zero(&pa, sizeof pa);
            pt.kind = LOGIT_PAINT2_LINEAR; pt.nstop = 2; pt.stop_pos = pos; pt.stop_argb = col;
            pt.argb = col[1]; pt.x0 = pt.x1 = X * 256; pt.y0 = Y * 256; pt.y1 = (Y + 20) * 256;
            pa.verbs = p.v; pa.xy = p.xy; pa.nverb = (unsigned)p.nv; pa.nxy = (unsigned)p.nxy;
            pa.rule = LOGIT_PATH2_NONZERO;
            ad_note_path(X, Y, 20, 20, 0, p.v, p.nv, p.xy, p.nxy, col[0] ^ col[1]);
            ol_window_path2(&pa, &pt, 0);
        } else aui_round(cx, cy, 20, 20, 5, fill);
        if (ic != 0x3F) gui_icon(ic, X_(cx + 3), Y_(cy + 3), 13, off ? t->label_quaternary : 0xFFFFFF);
        lx = 44;
    } else if (rw->icon >= 0) {
        gui_icon(rw->icon, X_(X0 + 22 - 8), Y_(Y0 + (AUI_SB_PITCH - 16) / 2), 16, sym);
    }
    if (rw->count > 0) {
        char buf[16];
        int cw;
        nav_count_text(buf, sizeof buf, rw->count);
        cw = aui_text_tnum_width(buf, 12, AUI_W_REGULAR);
        aui_text_tnum(X0 + W - 14, nav_text_top_on(Y0, AUI_SB_PITCH, 14, 12), 12, AUI_W_REGULAR,
                      t->label_tertiary, buf, AUI_ALIGN_RIGHT);
        right = W - 14 - cw - 8;
    }
    if (rw->label[0] && right > lx)
        aui_text_ellipsis_w(X0 + lx, nav_text_top(Y0, AUI_SB_PITCH, 14), right - lx, rw->label, 14,
                            sel ? AUI_W_MEDIUM : AUI_W_REGULAR, ink, AUI_TRUNC_TAIL);
}

void aui_sidebar_end(void)
{
    const struct logit_tokens *t = aui_tok();
    struct nav_sb *sb = nav_cur;
    int eff, focused, moving = 0, i, slot, hov = -1, press = -1;
    struct aui_rect C;
    if (!sb) return;
    nav_cur = 0;
    nav_sec_close(sb);
    focused = aui_nav_focused();
    C = aui_r(sb->r.x, sb->r.y, sb->W, sb->r.h);

    /* the pill's row: picked this frame (press or key) wins over the app's
     * state, so the glide starts in the frame that took the input */
    eff = sb->clicked >= 0 ? sb->clicked : sb->key_pick >= 0 ? sb->key_pick : sb->sel_now;
    if (eff >= sb->nrows) eff = -1;
    if (eff >= 0) {
        const struct nav_row *rw = &sb->rows[eff];
        if (rw->sec >= 0 && ((sb->collapsed >> rw->sec) & 1) && sb->secs[rw->sec].h <= 0) eff = -1;
    }
    sb->sel = eff;

    aui_clip_push_round(C, AUI_SB_RADIUS);
    if (eff >= 0) {
        const struct nav_row *rw = &sb->rows[eff];
        static const int r6[4] = { AUI_SB_PILL_R, AUI_SB_PILL_R, AUI_SB_PILL_R, AUI_SB_PILL_R };
        int32_t y24;
        /* a pill that was not on screen last frame appears settled */
        if (!sb->pill.init || sb->pill.gen + 1 != anim_gen) nav_spr_set(&sb->pill, rw->y << 8);
        y24 = nav_spr_go(&sb->pill, rw->y << 8, LOGIT_SPRING_SNAPPY);
        unsigned col = focused ? t->accent_tint : nav_opaque(t->fill_inactive_sel);
        int pushed;
        moving = sb->pill.live;
        pushed = nav_sec_clip(sb, rw->sec);
        nav_wash(nav_pill_rect(sb, nav_r24(y24)), r6, col, t->bg_sidebar);
        if (NAV_TWO_PILLS) nav_wash(nav_pill_rect(sb, rw->y), r6, col, t->bg_sidebar);
        if (pushed) clip_pop();
    }

    /* hover / press washes: two slots, issued every frame (file header) */
    for (i = 0; i < sb->nrows; i++) {
        if (i == eff || !sb->rows[i].can) continue;
        if (sb->rows[i].st & AUI_ACTIVE) press = i;
        if (sb->rows[i].st & AUI_HOVER) hov = i;
    }
    if (press >= 0) hov = press;
    /* both slots exist from the first frame on (parked on row 0 at alpha 0),
     * so a first hover adds no call and shifts nothing after it */
    for (slot = 0; slot < 2; slot++)
        if (sb->hv_rect[slot].w <= 0 && sb->nrows > 0) sb->hv_rect[slot] = nav_pill_rect(sb, sb->rows[0].y);
    if (hov != sb->hv[0]) {
        /* the slot leaving keeps fading where it was */
        int el = (int)(frame_ms - sb->hv_t0[0]);
        int a = sb->hv[0] < 0 ? 0 : el >= 90 ? 255 : 255 * el / 90;
        sb->hv_a0[0] = 0;
        sb->hv[1] = sb->hv[0]; sb->hv_a0[1] = a; sb->hv_t0[1] = frame_ms; sb->hv_rect[1] = sb->hv_rect[0];
        sb->hv[0] = hov; sb->hv_t0[0] = frame_ms;
    }
    for (slot = 0; slot < 2; slot++) {
        static const int r6[4] = { AUI_SB_PILL_R, AUI_SB_PILL_R, AUI_SB_PILL_R, AUI_SB_PILL_R };
        int el = (int)(frame_ms - sb->hv_t0[slot]), a;
        unsigned col = t->fill_hover;
        if (slot == 0) {
            if (sb->hv[0] >= 0) sb->hv_rect[0] = nav_pill_rect(sb, sb->rows[sb->hv[0]].y);
            a = sb->hv[0] < 0 ? 0 : el >= 90 ? 255 : 255 * el / 90;
            if (sb->hv[0] >= 0 && (sb->rows[sb->hv[0]].st & AUI_ACTIVE)) { col = t->fill_pressed; a = 255; }
        } else {
            a = sb->hv[1] < 0 ? 0 : el >= 140 ? 0 : sb->hv_a0[1] * (140 - el) / 140;
            if (a <= 0) sb->hv[1] = -1;
        }
        if (slot == 0 ? (sb->hv[0] >= 0 && a < 255) : a > 0) anim_live++;
        if (sb->hv_rect[slot].w > 0) nav_wash(sb->hv_rect[slot], r6, nav_argb_scale(col, a), t->bg_sidebar);
    }

    /* section headers: title, then the chevron (always one call) */
    for (i = 0; i < sb->nsec; i++) {
        struct nav_sec *s = &sb->secs[i];
        int coll = (sb->collapsed >> i) & 1, chev;
        int32_t rot;
        if (s->y + NAV_HDR_H > sb->r.h) continue;
        aui_text_w(sb->r.x + 16, sb->r.y + nav_text_top(s->y, NAV_HDR_H, 12), 12, AUI_W_SEMIBOLD,
                   t->label_tertiary, s->title);
        if (!s->collapsible) continue;
        if (!s->rot.init) nav_spr_set(&s->rot, coll ? -90 * 256 : 0);
        rot = nav_spr_go(&s->rot, coll ? -90 * 256 : 0, LOGIT_SPRING_SNAPPY);
        chev = nav_fade_go(&s->chev, s->hover, 90, 140);
        nav_chevron(sb->r.x + sb->W - 12 - 10, sb->r.y + s->y + (NAV_HDR_H - 10) / 2, 10, rot,
                    nav_argb_scale(nav_opaque(t->label_tertiary), chev));
        if (s->rot.live || s->hs.live) moving = 1;
    }

    for (i = 0; i < sb->nrows; i++) {
        const struct nav_row *rw = &sb->rows[i];
        int pushed;
        if (rw->sec >= 0) {
            const struct nav_sec *s = &sb->secs[rw->sec];
            if (rw->y >= s->rows_y + s->h) continue;
        }
        if (rw->y >= sb->r.h) continue;
        pushed = nav_sec_clip(sb, rw->sec);
        nav_sb_draw_row(sb, i, eff, focused);
        if (pushed) clip_pop();
    }
    aui_clip_pop();

    /* aims: one per fully shown row, at rest */
    if (sb->aim.on && !moving)
        for (i = 0; i < sb->nrows; i++)
            if (sb->rows[i].can)
                aim_item(&sb->aim, i, sb->wx + sb->W / 2, sb->wy + sb->rows[i].y + AUI_SB_PITCH / 2);
}

/* ================================================================ 4.2 ==== */
struct nav_tbline { int key; struct nav_fade f; unsigned gen; };
static struct nav_tbline nav_tbl[2];
static struct {
    int on, faded, focused;
    struct aui_rect band;
    int x, xr, n, nr, gap;          /* cursor, right cursor, items placed, next spacing */
    int grp, gn, gi, gx;
} nav_tb;

static struct nav_fade *nav_tb_line(int key)
{
    struct nav_tbline *o = &nav_tbl[0];
    for (int i = 0; i < 2; i++) if (nav_tbl[i].gen && nav_tbl[i].key == key) { nav_tbl[i].gen = anim_gen; return &nav_tbl[i].f; }
    for (int i = 0; i < 2; i++) if (nav_tbl[i].gen < o->gen) o = &nav_tbl[i];
    gfx_zero(o, sizeof *o);
    o->key = key; o->gen = anim_gen;
    return &o->f;
}

void aui_toolbar_begin(struct aui_rect band, unsigned header_rgb, int scroll_id)
{
    const struct logit_tokens *t = aui_tok();
    int a;
    if (band.h <= 0) band.h = AUI_TB_H;
    nav_tb.on = 1; nav_tb.band = band;
    nav_tb.x = band.x + AUI_TB_INSET; nav_tb.xr = band.x + band.w - AUI_TB_INSET;
    nav_tb.n = nav_tb.nr = 0; nav_tb.gap = 8; nav_tb.grp = 0;
    aui_fill(band.x, band.y, band.w, band.h, header_rgb & 0xFFFFFFu);
    /* the scroll-edge hairline, inside the band's last device row */
    a = nav_fade_go(nav_tb_line(scroll_id), scroll_id > 0 && aui_scroll_offset(scroll_id) > 0, 90, 90);
    nav_hline_last_row(band.x, band.x + band.w, band.y + band.h, nav_argb_scale(t->sep_hairline, a));
    nav_tb.focused = aui_nav_focused();
    nav_tb.faded = !nav_tb.focused;
    if (nav_tb.faded) aui_fade_begin(band, 217);        /* 85 % */
}

static int nav_tb_ctl_y(void) { return nav_tb.band.y + (nav_tb.band.h - AUI_TB_CTL_H) / 2; }

struct aui_rect aui_tb_item(int w)
{
    struct aui_rect r;
    if (nav_tb.grp) {
        r = aui_r(nav_tb.gx + nav_tb.gi * AUI_TB_SEG_W, nav_tb_ctl_y(), AUI_TB_SEG_W, AUI_TB_CTL_H);
        if (nav_tb.gi < nav_tb.gn - 1) nav_tb.gi++;
        return r;
    }
    if (w < 0) w = 0;
    if (nav_tb.n) nav_tb.x += nav_tb.gap;
    r = aui_r(nav_tb.x, nav_tb_ctl_y(), w, AUI_TB_CTL_H);
    nav_tb.x += w; nav_tb.n++; nav_tb.gap = 8;
    return r;
}

void aui_tb_gap(void) { nav_tb.gap = 16; }

struct aui_rect aui_tb_trailing(int w)
{
    struct aui_rect r;
    if (w < 0) w = 0;
    if (nav_tb.nr) nav_tb.xr -= nav_tb.gap;
    nav_tb.xr -= w;
    r = aui_r(nav_tb.xr, nav_tb_ctl_y(), w, AUI_TB_CTL_H);
    nav_tb.nr++; nav_tb.gap = 8;
    return r;
}

void aui_tb_title(const char *title, const char *subtitle)
{
    const struct logit_tokens *t = aui_tok();
    struct aui_rect b = nav_tb.band;
    int x = nav_tb.n ? nav_tb.x + 12 : nav_tb.x, w;
    if (!title) return;
    w = aui_text_measure_w(title, 17, AUI_W_SEMIBOLD);
    aui_text_w(x, nav_text_top(b.y, b.h, 17), 17, AUI_W_SEMIBOLD, t->label_primary, title);
    x += w;
    if (subtitle && subtitle[0]) {
        x += 8;
        aui_text_w(x, nav_text_top_on(b.y, b.h, 17, 12), 12, AUI_W_REGULAR, t->label_tertiary, subtitle);
        x += aui_text_measure_w(subtitle, 12, AUI_W_REGULAR);
    }
    nav_tb.x = x; nav_tb.n++; nav_tb.gap = 16;
}

/* The Secondary capsule face (4.3): L #FFFFFF + 1 dev sep.strong + E1; D
 * #3A3A43 (design-pinned in 4.3) + a 1 dev inner top highlight @ 20 + E1. */
static void nav_secondary_face(struct aui_rect r)
{
    const struct logit_tokens *t = aui_tok();
    int rr = r.h / 2, rad[4] = { rr, rr, rr, rr };
    aui_elev(r, rr, LOGIT_E1);
    if (theme_dark) {
        aui_round4(r, rad, 0xFF3A3A43u);
        if (path_ok()) {
            /* the highlight: a 1 dev line inside the top edge, between the caps */
            struct ap p;
            long long dx0 = aui_dev(X_(r.x + rr)), dx1 = aui_dev(X_(r.x + r.w - rr));
            long long dy0 = aui_dev(Y_(r.y));
            ap_begin(&p, 0);
            ap_m(&p, dev_ceil_pt24(dx0 * 256), dev_ceil_pt24(dy0 * 256));
            ap_l(&p, dev_ceil_pt24(dx1 * 256), dev_ceil_pt24(dy0 * 256));
            ap_l(&p, dev_ceil_pt24(dx1 * 256), dev_ceil_pt24((dy0 + 1) * 256));
            ap_l(&p, dev_ceil_pt24(dx0 * 256), dev_ceil_pt24((dy0 + 1) * 256));
            ap_z(&p);
            path_emit(&p, 0x14FFFFFFu, 1, 0, 0, X_(r.x + rr), Y_(r.y), r.w - 2 * rr, 1, 1);
        }
    } else {
        aui_round4(r, rad, nav_opaque(t->bg_raised));
        nav_rim_solid(r, rr, t->sep_strong, t->bg_raised);
    }
}

void aui_tb_group_begin(int n)
{
    if (n < 1) n = 1;
    if (nav_tb.n) nav_tb.x += nav_tb.gap;
    nav_tb.grp = 1; nav_tb.gn = n; nav_tb.gi = 0; nav_tb.gx = nav_tb.x;
    nav_secondary_face(aui_r(nav_tb.gx, nav_tb_ctl_y(), n * AUI_TB_SEG_W, AUI_TB_CTL_H));
}

void aui_tb_group_end(void)
{
    const struct logit_tokens *t = aui_tok();
    int y = nav_tb_ctl_y();
    if (!nav_tb.grp) return;
    for (int i = 1; i < nav_tb.gn; i++) {
        int x = nav_tb.gx + i * AUI_TB_SEG_W;
        aui_hairline_dev(x, y + 6, x, y + AUI_TB_CTL_H - 6, t->sep_hairline);
    }
    nav_tb.grp = 0;
    nav_tb.x = nav_tb.gx + nav_tb.gn * AUI_TB_SEG_W;
    nav_tb.n++; nav_tb.gap = 16;
}

int aui_tb_icon(int icon, unsigned flags)
{
    const struct logit_tokens *t = aui_tok();
    struct aui_rect r;
    struct aim_ctx aim;
    struct wres st;
    int wid, seg = nav_tb.grp ? nav_tb.gi : -1, gn = nav_tb.gn, rad[4], hv;
    int enabled = !(flags & AUI_TB_OFF);
    unsigned wash, sym;
    aim_take(&aim);
    r = nav_tb.grp ? aui_tb_item(AUI_TB_SEG_W) : (flags & AUI_TB_TRAIL) ? aui_tb_trailing(AUI_TB_CTL_H)
                                                                     : aui_tb_item(AUI_TB_CTL_H);
    wid = ++id_ctr;
    st = wpoll(wid, X_(r.x), Y_(r.y), r.w, r.h, enabled, 1);
    hv = aui_anim(AUI_AK_FACE, (st.st & AUI_HOVER) ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    if (seg >= 0) {
        int rr = AUI_TB_CTL_H / 2;
        rad[0] = rad[3] = seg == 0 ? rr : 0;
        rad[1] = rad[2] = seg == gn - 1 ? rr : 0;
    } else rad[0] = rad[1] = rad[2] = rad[3] = AUI_TB_CTL_H / 2;
    wash = (flags & AUI_TB_ON) ? t->accent_tint : nav_argb_scale(t->fill_hover, hv);
    if (st.st & AUI_ACTIVE) wash = t->fill_pressed;
    if (!enabled) wash &= 0xFFFFFFu;
    nav_wash(r, rad, wash, t->bg_raised);
    sym = !enabled ? t->label_quaternary
        : !nav_tb.focused ? t->label_tertiary
        : (flags & AUI_TB_ON) ? t->accent_ink
        : aui_mix(t->label_secondary, t->label_primary, hv);
    gui_icon(icon, X_(r.x + (r.w - 16) / 2), Y_(r.y + (r.h - 16) / 2), 16, sym);
    if ((st.st & AUI_FOCUSED) && seg < 0) nav_focus_ring(r, AUI_TB_CTL_H / 2);
    if (hv == 0 || hv == 255) aim_one(&aim, X_(r.x + r.w / 2), Y_(r.y + r.h / 2));
    return st.clicked;
}

void aui_toolbar_end(void)
{
    if (!nav_tb.on) return;
    if (nav_tb.grp) aui_tb_group_end();
    if (nav_tb.faded) aui_fade_end();
    nav_tb.on = 0;
}

/* ================================================== 4.17 split, disclosure */
#define NAV_KEYED 12
enum { NAV_K_SPLIT = 1, NAV_K_DISC, NAV_K_INSP };
struct nav_keyed {
    int kind, id, used;
    unsigned gen;
    struct nav_spr a, b;            /* split: position; disclosure: angle, height; inspector: offset */
    int v0, v1, v2;                 /* split: drag, grab, target; disclosure: open; inspector: target, w */
};
static struct nav_keyed nav_ks[NAV_KEYED];

static struct nav_keyed *nav_key(int kind, int id)
{
    struct nav_keyed *o = &nav_ks[0];
    for (int i = 0; i < NAV_KEYED; i++)
        if (nav_ks[i].used && nav_ks[i].kind == kind && nav_ks[i].id == id) { nav_ks[i].gen = anim_gen; return &nav_ks[i]; }
    for (int i = 0; i < NAV_KEYED; i++) {
        if (!nav_ks[i].used) { o = &nav_ks[i]; break; }
        if (nav_ks[i].gen < o->gen) o = &nav_ks[i];
    }
    gfx_zero(o, sizeof *o);
    o->kind = kind; o->id = id; o->used = 1; o->gen = anim_gen;
    return o;
}

int aui_split(int id, struct aui_rect r, int *pos, int lo, int hi, int vertical)
{
    struct nav_keyed *k = nav_key(NAV_K_SPLIT, id);
    struct wres st;
    int wid, p, before, org, m;
    if (!pos) return 0;
    if (hi < lo) hi = lo;
    before = *pos;
    p = nav_clampi(*pos, lo, hi);
    if (!k->a.init || (!k->v0 && !k->a.live && nav_r24(k->a.sp.x) != p)) {
        nav_spr_set(&k->a, p << 8);              /* the app moved it: follow */
        k->v2 = p;
    }
    wid = ++id_ctr;
    org = vertical ? X_(r.x) : Y_(r.y);
    m = vertical ? in.mx : in.my;
    st = vertical ? wpoll(wid, org + p - 4, Y_(r.y), 8, r.h, 1, 0)
                  : wpoll(wid, X_(r.x), org + p - 4, r.w, 8, 1, 0);
    if (st.clicked) { k->v0 = 1; k->v1 = m - (org + p); }
    if (k->v0) {
        if (in.active == wid && in.down) {
            p = nav_clampi(m - org - k->v1, lo, hi);
            nav_spr_set(&k->a, p << 8);
            k->v2 = p;
        } else {
            k->v0 = 0;
            k->v2 = nav_clampi((p + 2) / 4 * 4, lo, hi);
        }
    }
    if (!k->v0) p = nav_clampi(nav_r24(nav_spr_go(&k->a, k->v2 << 8, LOGIT_SPRING_SNAPPY)), lo, hi);
    *pos = p;
    if (vertical) aui_hairline_dev(r.x + p, r.y, r.x + p, r.y + r.h, nav_opaque(aui_tok()->sep_opaque));
    else aui_hairline_dev(r.x, r.y + p, r.x + r.w, r.y + p, nav_opaque(aui_tok()->sep_opaque));
    return *pos != before;
}

int aui_disclosure_at(int id, struct aui_rect r, const char *label, int *open)
{
    const struct logit_tokens *t = aui_tok();
    struct nav_keyed *k = nav_key(NAV_K_DISC, id);
    struct wres st;
    int wid, changed = 0, o;
    int32_t ang;
    if (!open) return 0;
    wid = ++id_ctr;
    st = wpoll(wid, X_(r.x), Y_(r.y), r.w, r.h, 1, 1);
    if (st.clicked) { *open = !*open; changed = 1; }
    o = *open != 0;
    k->v0 = o;
    if (!k->a.init) nav_spr_set(&k->a, o ? 90 * 256 : 0);
    ang = nav_spr_go(&k->a, o ? 90 * 256 : 0, LOGIT_SPRING_SNAPPY);
    /* CHEVRON_R is the down chevron turned -90; open turns it to 0 */
    nav_chevron(r.x, r.y + (r.h - 10) / 2, 10, ang - 90 * 256, nav_opaque(t->label_tertiary));
    if (label) aui_text_ellipsis_w(r.x + 18, nav_text_top(r.y, r.h, 14), r.w - 18, label, 14, AUI_W_REGULAR,
                                   t->label_primary, AUI_TRUNC_TAIL);
    if (st.st & AUI_FOCUSED) nav_focus_ring(r, 6);
    return changed;
}

int aui_disclosure(int id, const char *label, int *open)
{
    struct aui_rect r;
    aui_row(&r, AUI_FILL, 28);
    if (r.w <= 0) r.w = win_w - X_(r.x);
    return aui_disclosure_at(id, r, label, open);
}

int aui_disclosure_h(int id, int full_h)
{
    struct nav_keyed *k = nav_key(NAV_K_DISC, id);
    int tgt = k->v0 ? (full_h > 0 ? full_h : 0) : 0;
    if (!k->b.init) nav_spr_set(&k->b, tgt << 8);
    return nav_clampi(nav_r24(nav_spr_go(&k->b, tgt << 8, LOGIT_SPRING_SMOOTH)), 0, full_h > 0 ? full_h : 0);
}

/* ================================================================ 4.18 === */
static struct { int on, sox, soy, first; } nav_in;

/* offset (points the panel is pushed right of r.x) this frame */
static int nav_insp_off(struct nav_keyed *k)
{
    if (!k->a.init) nav_spr_set(&k->a, k->v0 << 8);
    return nav_clampi(nav_r24(nav_spr_go(&k->a, k->v0 << 8, LOGIT_SPRING_SMOOTH)), 0, k->v1);
}

int aui_inspector_reflow_w(int id)
{
    struct nav_keyed *k;
    for (int i = 0; i < NAV_KEYED; i++)
        if (nav_ks[i].used && nav_ks[i].kind == NAV_K_INSP && nav_ks[i].id == id) {
            k = &nav_ks[i];
            k->gen = anim_gen;
            return k->v1 - nav_insp_off(k);
        }
    return 0;
}

int aui_inspector_begin(int id, struct aui_rect r, int shown)
{
    const struct logit_tokens *t = aui_tok();
    struct nav_keyed *k = nav_key(NAV_K_INSP, id);
    int off, px;
    if (r.w <= 0 || r.h <= 0) return 0;
    k->v1 = r.w;
    k->v0 = shown ? 0 : r.w;
    off = nav_insp_off(k);
    if (off >= r.w) return 0;
    px = r.x + off;
    clip_push(aui_r(X_(r.x), Y_(r.y), r.w, r.h));
    aui_fill(px, r.y, r.w, r.h, t->bg_inspector);
    aui_hairline_dev(px, r.y, px, r.y + r.h, nav_opaque(t->sep_opaque));
    nav_in.on = 1; nav_in.sox = ox_; nav_in.soy = oy_; nav_in.first = 1;
    ox_ = X_(px); oy_ = Y_(r.y);
    aui_vstack_w(16, 16, r.w - 32, 8);
    return 1;
}

int aui_inspector_group(const char *title)
{
    struct aui_rect rr;
    if (!nav_in.on) return 0;
    if (!nav_in.first) aui_spacer(16);
    nav_in.first = 0;
    aui_row(&rr, AUI_FILL, 20);
    if (title) aui_text_vc(rr, title, 14, AUI_W_SEMIBOLD, aui_tok()->label_primary, AUI_ALIGN_LEFT);
    return 1;
}

void aui_inspector_end(void)
{
    if (!nav_in.on) return;
    aui_stack_end();
    ox_ = nav_in.sox; oy_ = nav_in.soy;
    clip_pop();
    nav_in.on = 0;
}
