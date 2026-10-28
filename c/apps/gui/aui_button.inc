/* aui_button.inc -- buttons: the glass control face (still used by the segmented
 * pill and the toggle track), the design 4.3 button variants, aui_button_face,
 * aui_button/_ex, aui_icon_button and the control part of aui_select (its popup
 * is draw_popup, in aui_overlay.inc).
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

/* The damage note: the panel's rect grown by ad_halo_glass() (how far the
 * backdrop it samples reaches), computed in DEVICE pixels and converted back to
 * points rounding up, like ad_note_shadow2. */
static void ad_note_glass2(int x, int y, int w, int h, int r, unsigned tint, int opacity)
{
    int pct = aui_scale();
    int hd = ad_halo_glass(aui_dev(w), aui_dev(h), aui_dev(r), OL_GLASS_BLUR_THIN);
    int hp = sh_up100(hd, pct) + 1;
    unsigned sig = 2166136261u;
    struct ad_set s;
    AD_MIX(sig, r); AD_MIX(sig, tint); AD_MIX(sig, opacity); AD_MIX(sig, 0x6c55u);
    s.n = 0;
    ad_add_halo(&s, x, y, w, h, hp, hp, hp, hp);
    if (s.n) ad_record(s.r[0].x0, s.r[0].y0, s.r[0].x1 - s.r[0].x0, s.r[0].y1 - s.r[0].y0, sig);
    if (g_ad_gl_n < AD_GLMAX) {
        struct ad_gl *g = &g_ad_gl[g_ad_gl_n++];
        g->x = x; g->y = y; g->w = w; g->h = h; g->halo = hp;
    } else g_ad_overflow = 1;
}

/* One glass control face; 1 when the kernel drew it, 0 when the caller must
 * draw the gradient face. Rect and radius are decided in device pixels the way
 * shadow2_emit does. */
static int glass_face(int x, int y, int w, int h, int r, unsigned tint, int opacity)
{
#ifdef AUI_NO_AA
    (void)x; (void)y; (void)w; (void)h; (void)r; (void)tint; (void)opacity;
    return 0;
#else
    struct logit_glass2 g;
    long long x0, x1, y0, y1;
    int k, rr;
    if (w <= 0 || h <= 0 || !glass_ctl_ok()) return 0;
    r = clamp_radius(w, h, r);
    x0 = dev_ceil_pt24((long long)aui_dev(X_(x)) * 256); x1 = dev_ceil_pt24((long long)aui_dev(X_(x + w)) * 256);
    y0 = dev_ceil_pt24((long long)aui_dev(Y_(y)) * 256); y1 = dev_ceil_pt24((long long)aui_dev(Y_(y + h)) * 256);
    rr = (int)dev_ceil_pt24((long long)imax(0, aui_dev(r)) * 256);
    gfx_zero(&g, sizeof g);
    g.x = (int)x0; g.y = (int)y0; g.w = (int)(x1 - x0); g.h = (int)(y1 - y0);
    for (k = 0; k < 4; k++) g.radius[k] = rr;
    g.tint_argb = tint & 0xFFFFFFu;
    g.opacity = opacity; g.tone = 0;
    g.blur_level = OL_GLASS_BLUR_THIN; g.rim = GLASS_CTL_RIM;
    g.flags = LOGIT_RECT_SNAP;
    if (ol_window_glass2(&g) != 0) return 0;
    ad_note_glass2(x, y, w, h, r, tint, opacity);
    return 1;
#endif
}

#define GLASS_BTN_OPACITY 176
#define GLASS_TRACK_OPACITY 208

/* ===========================================================================
 * 「澄」 buttons (design 4.3; added by ui-aui-buttons).
 *
 * A button is a CAPSULE (radius h / 2) painted with ordinary paths: no button
 * makes a GLASS2 call. Variants:
 *
 *   Primary    accent, +-4 % L vertical gradient, 1 dev inner top highlight, E1
 *   Secondary  L: bg.raised + 1 dev sep.strong border; D: #3A3A43 + 1 dev inner
 *              top highlight (white @ 20), no border; E1
 *   Plain      no fill, accent.ink label (label.primary with AUI_BTN_F_TOOLBAR)
 *   Destructive  _F_CONFIRM: destructive.fill, white label; else Plain + error.ink
 *   Icon       28 / 24 capsule, symbol 16, label.secondary
 *   Toggle     Icon; ON: accent.tint fill, accent.ink symbol
 *   AI         Secondary + a 1.5 pt LINEAR Logit-gradient capsule stroke + a
 *              leading 4-point sparkle; the paint axis rotates 40 deg over 600 ms
 *              once per hover entry (it moves only that button's damage rect)
 *   Popup      Secondary, text left at 10, CHEVRON_D 10 at right inset 10
 *
 * State: hover and pressed fills are tokens (fill.hover / fill.pressed over the
 * face, accent.hover / .pressed); the colour moves by an aui_anim_color tween
 * (90 ms) except that PRESSED lands on the same frame; the shape (never the
 * label) scales to aui_press_scale_q8 (248 = 0.97). Disabled = an opacity group
 * of 102 over the enabled look (Plain and Icon: label.quaternary). Colours come
 * from aui_tok(); the only literal is the design-pinned Secondary D face. */
#define BTN_PX          13
#define BTN_WEIGHT      500
#define BTN_DIS_A       102
#define BTN_SEC_D_FACE  0x3A3A43u       /* design 4.3: Secondary, dark */
#define BTN_SEC_D_HI_A  20u
#define BTN_PRI_HI_A    46u
#define BTN_GRAD_T      10              /* 4 % L, as a 0..255 mix toward white / black */
#define BTN_SPARK       14
#define BTN_AI_DEG      40
#define BTN_AI_MS       600

#ifdef UI_BUTTONS_NEGCTL
#define BTN_RADIUS_CAP(r, pct) ({ long long c_ = 11LL * 256 * (pct) / 100; (r) > c_ ? c_ : (r); })
#else
#define BTN_RADIUS_CAP(r, pct) (r)
#endif

static unsigned btn_over(unsigned base, unsigned argb)
{ return aui_mix(base & 0xFFFFFFu, argb & 0xFFFFFFu, (int)(argb >> 24)); }

/* One capsule path: fill (stroke_q8 == 0) or an outline of stroke_q8 (24.8
 * POINTS; the outer edge is the rect's). s_q8 scales the shape about its centre
 * (256 = 1). `grad` replaces the solid colour with its paint (absolute 24.8
 * window points). */
static long btn_path(int x, int y, int w, int h, int s_q8, int stroke_q8, unsigned argb,
                     const struct logit_paint2 *grad)
{
    struct ap p;
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    long long rx[4], ry[4], hw = 0, r, wd, ht;
    int X = X_(x), Y = Y_(y), k;
    long long x0 = (long long)aui_dev(X) * 256, x1 = (long long)aui_dev(X + w) * 256;
    long long y0 = (long long)aui_dev(Y) * 256, y1 = (long long)aui_dev(Y + h) * 256;
    unsigned sig = argb ^ ((unsigned)stroke_q8 * 2654435761u) ^ ((unsigned)s_q8 * 40503u);
    if (s_q8 > 0 && s_q8 != 256) {
        wd = (x1 - x0) * s_q8 / 256; ht = (y1 - y0) * s_q8 / 256;
        x0 = (x0 + x1 - wd) / 2; x1 = x0 + wd;
        y0 = (y0 + y1 - ht) / 2; y1 = y0 + ht;
    }
    if (stroke_q8 > 0) {
        hw = ap_fdiv((long long)stroke_q8 * aui_scale(), 100) / 2;
        x0 += hw; y0 += hw; x1 -= hw; y1 -= hw;
    }
    r = (x1 - x0 < y1 - y0 ? x1 - x0 : y1 - y0) / 2;
    r = BTN_RADIUS_CAP(r + hw, aui_scale()) - hw;
    if (r < 0) r = 0;
    for (k = 0; k < 4; k++) rx[k] = ry[k] = r;
    ap_begin(&p, 1);
    ap_rrect(&p, x0, y0, x1, y1, rx, ry);
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = p.v; pa.xy = p.xy; pa.nverb = (unsigned)p.nv; pa.nxy = (unsigned)p.nxy;
    pa.rule = LOGIT_PATH2_NONZERO;
    if (grad) {
        pt = *grad;
        for (k = 0; k < grad->nstop; k++) sig = sig * 16777619u ^ grad->stop_argb[k];
        sig = sig * 16777619u ^ (unsigned)grad->x0; sig = sig * 16777619u ^ (unsigned)grad->y0;
        sig = sig * 16777619u ^ (unsigned)grad->x1; sig = sig * 16777619u ^ (unsigned)grad->y1;
    } else { pt.kind = LOGIT_PAINT2_SOLID; pt.argb = argb; }
    if (stroke_q8 > 0) {
        st.width = stroke_q8; st.cap = LOGIT_STROKE2_CAP_BUTT; st.join = LOGIT_STROKE2_JOIN_MITER;
        st.miter = 4 * 65536;
    }
    ad_note_path(X, Y, w, h, stroke_q8 > 0 ? 1 : 0, p.v, p.nv, p.xy, p.nxy, sig);
    return ol_window_path2(&pa, &pt, stroke_q8 > 0 ? &st : 0);
}

static void btn_fill(int x, int y, int w, int h, int s, unsigned argb)
{
    if ((argb >> 24) == 0) return;
    if (path_ok()) { btn_path(x, y, w, h, s, 0, argb, 0); return; }
    aui_round_a(x, y, w, h, h / 2, argb & 0xFFFFFFu, (int)(argb >> 24));
}

/* A vertical two-stop gradient capsule (one PATH with a LINEAR paint). */
static void btn_fill_grad(int x, int y, int w, int h, int s, unsigned top, unsigned bot)
{
    unsigned pos[2] = { 0, 65536 }, col[2];
    struct logit_paint2 pt;
    if (path_ok()) {
        col[0] = 0xFF000000u | top; col[1] = 0xFF000000u | bot;
        gfx_zero(&pt, sizeof pt);
        pt.kind = LOGIT_PAINT2_LINEAR; pt.nstop = 2; pt.stop_pos = pos; pt.stop_argb = col;
        pt.x0 = pt.x1 = X_(x) * 256 + w * 128;
        pt.y0 = Y_(y) * 256; pt.y1 = (Y_(y) + h) * 256;
        btn_path(x, y, w, h, s, 0, 0, &pt);
        return;
    }
    aui_vgrad_round(x, y, w, h, h / 2, top, bot);
}

/* 1 DEVICE pixel outline, ARGB. */
static void btn_stroke1(int x, int y, int w, int h, int s, unsigned argb)
{
    int pct = aui_scale();
    if (path_ok()) { btn_path(x, y, w, h, s, (256 * 100 + pct - 1) / pct, argb, 0); return; }
    aui_stroke(x, y, w, h, h / 2, 1, argb & 0xFFFFFFu);
}

/* The 4-point sparkle of the AI button: a concave star of 4 quadratic arcs. */
static void btn_sparkle(int cx, int cy, int size, unsigned rgb)
{
    struct ap p;
    long long X = (long long)X_(cx) * 256, Y = (long long)Y_(cy) * 256;
    long long R = (long long)size * 128, k = R * 20 / 100;
    if (!path_ok()) { gui_icon(GICON_STAR, X_(cx) - size / 2, Y_(cy) - size / 2, size, rgb); return; }
    ap_begin(&p, 0);
    ap_m(&p, X, Y - R);
#define BTN_Q(cxp, cyp, ex, ey) do { if (p.nv < AP_V) { p.v[p.nv++] = LOGIT_VERB_Q; ap_pt(&p, (cxp), (cyp)); ap_pt(&p, (ex), (ey)); } } while (0)
    BTN_Q(X + k, Y - k, X + R, Y);
    BTN_Q(X + k, Y + k, X, Y + R);
    BTN_Q(X - k, Y + k, X - R, Y);
    BTN_Q(X - k, Y - k, X, Y - R);
#undef BTN_Q
    ap_z(&p);
    path_emit(&p, 0xFF000000u | rgb, 0, 0, 0, X_(cx) - size / 2, Y_(cy) - size / 2, size, size, 0);
}

/* The AI stroke: a LINEAR Logit-gradient (3.1.6) 1.5 pt outline whose axis runs
 * along the button's diagonal, rotated by deg256 (24.8 degrees) about the centre. */
static void btn_ai_stroke(int x, int y, int w, int h, int s, int deg256)
{
    static const unsigned pos[3] = { 0, 36044, 65536 };       /* 55 % */
    unsigned col[3];
    struct logit_paint2 pt;
    long long cx = (long long)X_(x) * 256 + (long long)w * 128, cy = (long long)Y_(y) * 256 + (long long)h * 128;
    long long hx = (long long)w * 128, hy = (long long)h * 128, sn = gfx_sin(deg256), cs = gfx_cos(deg256);
    long long rx = (hx * cs - hy * sn) >> 16, ry = (hx * sn + hy * cs) >> 16;
    if (!path_ok()) { aui_stroke(x, y, w, h, h / 2, 2, aui_t.accent_ink); return; }
    col[0] = 0xFF000000u | LOGIT_BRAND_STOP0; col[1] = 0xFF000000u | LOGIT_BRAND_STOP1;
    col[2] = 0xFF000000u | LOGIT_BRAND_STOP2;
    gfx_zero(&pt, sizeof pt);
    pt.kind = LOGIT_PAINT2_LINEAR; pt.nstop = 3; pt.stop_pos = pos; pt.stop_argb = col;
    pt.x0 = (int)(cx - rx); pt.y0 = (int)(cy - ry); pt.x1 = (int)(cx + rx); pt.y1 = (int)(cy + ry);
    btn_path(x, y, w, h, s, 384, 0, &pt);
}

/* One button, drawn. st: AUI_HOVER / ACTIVE / FOCUSED / OFF. icon < 0: none.
 * The label is drawn at the rect's centre, never scaled. */
static void btn_draw(int x, int y, int w, int h, const char *label, int icon, int v, int st)
{
    const struct logit_tokens *t = aui_tok();
    int var = v & 0xFF, fl = v & ~0xFF;
    int off = (st & AUI_OFF) != 0, act = !off && (st & AUI_ACTIVE) != 0, hov = !off && (st & AUI_HOVER) != 0;
    int dark = aui_is_dark(), s = 256, filled = 1, grad = 0, elev = 1, line = 0;
    int dfade = 0, fopen = 0, popup = var == AUI_BTN_POPUP, ai = var == AUI_BTN_AI, iconv = 0, lw = 0;
    unsigned rest = 0, tgt = 0, cur, fg = t->label_primary, ovr = 0, ovt = 0, ovc = 0;
    int wx = X_(x), wy = Y_(y);

    if (var == AUI_V_GLASS || var == AUI_BTN_SECONDARY) var = AUI_BTN_SECONDARY;
    switch (var) {
    case AUI_BTN_PRIMARY:
        rest = t->accent; tgt = act ? t->accent_pressed : hov ? t->accent_hover : t->accent;
        fg = t->label_on_accent; grad = 1; break;
    case AUI_BTN_DESTRUCTIVE:
        if (fl & AUI_BTN_F_CONFIRM) {
            rest = t->destructive_fill;
            tgt = act ? aui_mix(rest, 0, 31) : hov ? aui_mix(rest, 0, 15) : rest;   /* -12 % / -6 % L */
            fg = t->label_on_accent; break;
        }
        filled = 0; elev = 0; fg = t->error_ink; break;
    case AUI_BTN_PLAIN:
        filled = 0; elev = 0; fg = (fl & AUI_BTN_F_TOOLBAR) ? t->label_primary : t->accent_ink; break;
    case AUI_BTN_ICON: case AUI_BTN_TOGGLE_ICON:
        filled = 0; elev = 0; iconv = 1;
        fg = hov || act ? t->label_primary : t->label_secondary; break;
    default:        /* Secondary, AI, Popup */
        rest = dark ? BTN_SEC_D_FACE : t->bg_raised;
        tgt = act ? btn_over(rest, t->fill_pressed) : hov ? btn_over(rest, t->fill_hover) : rest;
        break;
    }
    if (!filled) {
        /* a translucent overlay capsule: transparent at rest (the ON toggle rests at accent.tint) */
        int on = var == AUI_BTN_TOGGLE_ICON && (fl & AUI_BTN_F_ON);
        unsigned ha = act ? t->fill_pressed >> 24 : hov ? t->fill_hover >> 24 : 0;
        ovr = on ? t->accent_tint : (t->fill_hover & 0xFFFFFFu);
        ovt = on ? ((t->accent_tint & 0xFFFFFFu) | ((((t->accent_tint >> 24) + ha) > 255 ? 255 : ((t->accent_tint >> 24) + ha)) << 24))
                 : act ? t->fill_pressed : hov ? t->fill_hover : ovr;
        if (on) fg = t->accent_ink;
    }
    if (off && !filled) fg = t->label_quaternary;

    if (off && filled) {
        dfade = fade_open(aui_r(wx - 8, wy - 8, w + 16, h + 16), BTN_DIS_A, 0); fopen = 1;
        if (!dfade) { rest = tgt = AUI_DISABLED; fg = AUI_DISABLED_TX; grad = 0; elev = 0; }
    }
    /* the 90 ms colour tween; pressed lands at once (the slot still tracks it) */
    if (filled) {
        cur = aui_anim_color(AUI_AK_FACE, 0xFF000000u | rest, 0xFF000000u | tgt) & 0xFFFFFFu;
        if (act) cur = tgt;
    } else {
        ovc = aui_anim_color(AUI_AK_FACE, ovr, ovt);
        if (act) ovc = ovt;
        cur = 0;
    }
    if (hov || act) s = aui_press_scale_q8(AUI_AK_VALUE, act);

    if (filled) {
        if (elev) aui_elev(aui_r(x, y, w, h), h / 2, LOGIT_E1);
#ifdef AUI_BTN_SECONDARY_GLASS      /* the negative control of test-aui-glass-controls: glass_face is back on Secondary */
        if (var == AUI_BTN_SECONDARY) glass_face(x, y, w, h, h / 2, rest, GLASS_BTN_OPACITY);
#endif
        if (grad) btn_fill_grad(x, y, w, h, s, aui_mix(cur, 0xFFFFFF, BTN_GRAD_T), aui_mix(cur, 0, BTN_GRAD_T));
        else btn_fill(x, y, w, h, s, 0xFF000000u | cur);
        line = (grad || (dark && (var == AUI_BTN_SECONDARY || ai || popup))) && s == 256 && w > h;
        if (line) aui_hairline_dev(x + h / 2, y, x + w - h / 2, y,
                                   grad ? ((BTN_PRI_HI_A << 24) | 0xFFFFFFu) : ((BTN_SEC_D_HI_A << 24) | 0xFFFFFFu));
        if (!dark && !grad && !ai && (var == AUI_BTN_SECONDARY || popup)) btn_stroke1(x, y, w, h, s, t->sep_strong);
        if (ai) {
            int a = aui_anim(AUI_AK_EXTRA, hov ? 255 : 0, hov ? BTN_AI_MS : 1, AUI_EASE_LINEAR);
            btn_ai_stroke(x, y, w, h, s, BTN_AI_DEG * 256 * a / 255);
        }
    } else if ((ovc >> 24) != 0) {
        btn_fill(x, y, w, h, s, ovc);
    }
    focus_ring(!off && (st & AUI_FOCUSED), x, y, w, h, h / 2);

    /* content: never scaled */
    if (label && *label) lw = aui_text_measure_w(label, BTN_PX, BTN_WEIGHT);
    if (iconv) {
        int ip = h >= 28 ? 16 : 14;
        if (icon >= 0) gui_icon(icon, wx + (w - ip) / 2, wy + (h - ip) / 2, ip, fg);
    } else if (popup) {
        int avail = w - 10 - 10 - 10 - 6;
        if (lw > avail) aui_text_ellipsis_w(x + 10, y + (h - BTN_PX) / 2 - 1, avail, label, BTN_PX, BTN_WEIGHT, fg, AUI_TRUNC_TAIL);
        else if (lw) aui_text_vc(aui_r(x + 10, y, avail, h), label, BTN_PX, BTN_WEIGHT, fg, AUI_ALIGN_LEFT);
        gui_icon(GICON_CHEVRON_D, wx + w - 20, wy + (h - 10) / 2, 10, off && !dfade ? AUI_DISABLED_TX : t->label_secondary);
    } else {
        int iw = (ai || icon >= 0) ? BTN_SPARK : 0;
        int gap = (iw && lw) ? 6 : 0, tot = iw + gap + lw, sx = x + (w - tot) / 2;
        if (ai) btn_sparkle(sx + BTN_SPARK / 2, y + h / 2, BTN_SPARK, t->accent_ink);
        else if (icon >= 0) gui_icon(icon, X_(sx), wy + (h - BTN_SPARK) / 2, BTN_SPARK, fg);
        if (lw) aui_text_vc(aui_r(sx + (iw ? iw + gap : 0), y, lw + 1, h), label, BTN_PX, BTN_WEIGHT, fg, AUI_ALIGN_LEFT);
    }
    if (fopen) fade_close();
}

void aui_button_face(int x, int y, int w, int h, int radius)
{
    (void)radius;
    (void)face_for;     /* aui.c's legacy face colour: no button uses it now; ui-aui-theme removes it */
    aui_ensure();
    btn_draw(x, y, w, h, 0, -1, AUI_BTN_SECONDARY, 0);
}

int aui_button_width(const char *label, int has_icon)
{
    int w = (label && *label ? aui_text_measure_w(label, BTN_PX, BTN_WEIGHT) : 0) + (has_icon ? 12 * 2 + BTN_SPARK + 6 : 14 * 2);
    return w < AUI_BTN_MIN_W ? AUI_BTN_MIN_W : w;
}

static int btn_run(int x, int y, int w, int h, const char *label, int icon, int v, int enabled)
{
    int id = ++id_ctr;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, enabled, 1);
    btn_draw(x, y, w, h, label, icon, v, r.st);
    return r.clicked;
}

int aui_button_ex(int x, int y, int w, int h, const char *label, enum aui_variant v, int enabled)
{ return btn_run(x, y, w, h, label, -1, (int)v, enabled); }

int aui_button(int x, int y, int w, int h, const char *label)
{ return btn_run(x, y, w, h, label, -1, AUI_BTN_SECONDARY, 1); }

int aui_button_icon(int x, int y, int w, int h, const char *label, int icon, enum aui_variant v, int enabled)
{ return btn_run(x, y, w, h, label, icon, (int)v, enabled); }

int aui_button_ai(int x, int y, int w, int h, const char *label, int enabled)
{ return btn_run(x, y, w, h, label, -1, AUI_BTN_AI, enabled); }

int aui_icon_button(int x, int y, int size, int icon, int enabled)
{ return btn_run(x, y, size, size, 0, icon, AUI_BTN_ICON, enabled); }

int aui_icon_toggle(int x, int y, int size, int icon, int on, int enabled)
{ return btn_run(x, y, size, size, 0, icon, AUI_BTN_TOGGLE_ICON | (on ? AUI_BTN_F_ON : 0), enabled); }

/* ---- dropdown + menu (deferred popups) ---- */

int aui_select(int x, int y, int w, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, h = AUI_H_CTL;
    struct aim_ctx aim;
    aim_take(&aim);
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, 1, 1);
    const char *cur = (*sel >= 0 && *sel < n) ? items[*sel] : "";
    btn_draw(x, y, w, h, cur, -1, AUI_BTN_POPUP, r.st);     /* design 4.3: the select's face is a Popup button */
    aim_one(&aim, X_(x + w / 2), Y_(y + h / 2));    /* the control (added) */

    if (r.clicked && pop.kind == 0) {
        pop.kind = 1; pop.owner = id; pop.x = X_(x); pop.y = Y_(y) + h + 4; pop.w = w;
        pop_gen = anim_gen;
        pop.itemh = ROWH; pop.n = n; pop.items = items; pop.sel = sel; pop.hi = *sel;
    }
    if ((focus_id == id) && in.ev == EV_KEY && !in.key_used &&
        (in.a == KEY_DOWN || in.a == KEY_UP)) {
        *sel = iclamp(*sel + (in.a == KEY_DOWN ? 1 : -1), 0, n - 1);
        in.key_used = 1; return 1;
    }
    if (pop_changed_id == id) { pop_changed_id = 0; return 1; }
    return 0;
}
