/* aui_button.inc -- buttons: the glass control face, button_face, aui_button_face,
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

/* The default button's face: a raised tile with a hairline all the way round.
 *
 * It was gui_glass() at 24 points (2026-10-26 release tour). The glass material
 * lights its rim from the upper left and pulls every rim pixel facing away from
 * that light toward a mid grey -- right for the dock, which has a wallpaper
 * behind it to refract. A button sits on a flat window panel, so there is
 * nothing to refract and the only visible part of the material was that grey:
 * two dark crescents, lower-left and upper-right, which read as a torn edge.
 * This face is symmetric by construction: a vertical gradient from near the
 * surface colour down to the control face, and one uniform border. `base` is
 * face_for()'s answer, so hover and press keep their colours. */
static void button_face(int x, int y, int w, int h, int rad, unsigned base)
{
    unsigned top, bot;
    if (glass_face(x, y, w, h, rad, base, GLASS_BTN_OPACITY)) return;
    if (theme_dark) { top = aui_shade(base, 8); bot = base; }
    else { top = aui_mix(AUI_SURFACE, base, 64); bot = aui_mix(AUI_SURFACE, base, 220); }
    aui_vgrad_round(x, y, w, h, rad, top, bot);
    aui_stroke(x, y, w, h, rad, 1, AUI_BORDER);
}

void aui_button_face(int x, int y, int w, int h, int radius)
{
    aui_ensure();
    button_face(x, y, w, h, radius, AUI_FACE);
}

int aui_button_ex(int x, int y, int w, int h, const char *label, enum aui_variant v, int enabled)
{
    int id = ++id_ctr;
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll(id, wx, wy, w, h, enabled, 1);
    int rad = imin(h / 2, AUI_R_MD + 3);
    unsigned fill, fg = AUI_TEXT;
    int off = (r.st & AUI_OFF) != 0;
    int vst = r.st & ~AUI_OFF;

    switch (v) {
    case AUI_V_PRIMARY:
        fill = AUI_ACCENT; fg = AUI_ACCENT_TEXT;
        if (r.st & AUI_ACTIVE) fill = aui_shade(fill, -26);
        else if (r.st & AUI_HOVER) fill = aui_shade(fill, 14);
        break;
    case AUI_V_DANGER:
        fill = AUI_ERROR; fg = rgb(255, 255, 255);
        if (r.st & AUI_ACTIVE) fill = aui_shade(fill, -26);
        else if (r.st & AUI_HOVER) fill = aui_shade(fill, 14);
        break;
    case AUI_V_GHOST:
        fill = 0; fg = AUI_ACCENT;
        break;
    case AUI_V_GLASS:
        fill = 0; break;
    default:
        fill = face_for(vst); break;
    }
    /* A disabled control is its ENABLED look drawn in an opacity group
     * (AUI_DISABLED_ALPHA), so what is behind it shows through. Without groups
     * it keeps the flat disabled tones. */
    int dfade = 0;
    if (off) {
        dfade = fade_open(aui_r(wx, wy, w, h), AUI_DISABLED_ALPHA, 0);
        if (!dfade) { fill = AUI_DISABLED; fg = AUI_DISABLED_TX; }
    }

    if (v == AUI_V_GLASS && (!off || dfade)) {
        /* Still called GLASS -- every aui_button() caller asks for it -- but
         * drawn as button_face(); see there for why. */
        button_face(x, y, w, h, imin(h / 2, 11), face_for(vst));
    } else if (v == AUI_V_GHOST) {
        if (r.st & AUI_ACTIVE)      aui_round_a(x, y, w, h, rad, AUI_ACCENT, 52);
        else if (r.st & AUI_HOVER)  aui_round_a(x, y, w, h, rad, AUI_ACCENT, 26);
        if (off && !dfade) fg = AUI_DISABLED_TX;
    } else {
        /* A control face is a gradient, not a flat fill: a single flat tone with
         * a hard edge is most of what "1998" looks like. Two steps of tone plus
         * an anti-aliased corner is most of what fixes it. */
        aui_vgrad_round(x, y, w, h, rad, aui_shade(fill, 10), aui_shade(fill, -10));
        if (v == AUI_V_SECONDARY) aui_stroke(x, y, w, h, rad, 1, AUI_BORDER);
    }
    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, rad);

    int lw = tw(label);
    txt(x + (w - lw) / 2, y + (h - PX) / 2 - 1, fg, label);
    if (off) fade_close();
    return r.clicked;
}

int aui_button(int x, int y, int w, int h, const char *label)
{ return aui_button_ex(x, y, w, h, label, AUI_V_GLASS, 1); }

int aui_icon_button(int x, int y, int size, int icon, int enabled)
{
    int id = ++id_ctr;
    struct wres r = wpoll(id, X_(x), Y_(y), size, size, enabled, 1);
    int rad = AUI_R_MD;
    if (r.st & AUI_ACTIVE)     aui_round_a(x, y, size, size, rad, AUI_ACCENT, 60);
    else if (r.st & AUI_HOVER) aui_round_a(x, y, size, size, rad, AUI_TEXT, 22);
    focus_ring(r.st & AUI_FOCUSED, x, y, size, size, rad);
    unsigned c = (r.st & AUI_OFF) ? AUI_DISABLED_TX : AUI_TEXT;
    int ip = size * 3 / 5;
    gui_icon(icon, X_(x) + (size - ip) / 2, Y_(y) + (size - ip) / 2, ip, c);
    return r.clicked;
}

/* ---- dropdown + menu (deferred popups) ---- */

int aui_select(int x, int y, int w, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, h = AUI_H_CTL;
    struct aim_ctx aim;
    aim_take(&aim);
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, 1, 1);
    unsigned fill = face_for(r.st);
    aui_vgrad_round(x, y, w, h, AUI_R_MD, aui_shade(fill, 10), aui_shade(fill, -10));
    focus_ring(focus_id == id, x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, AUI_BORDER);
    const char *cur = (*sel >= 0 && *sel < n) ? items[*sel] : "";
    aui_text_ellipsis(x + AUI_SP(2), y + (h - PX) / 2 - 1, w - AUI_SP(9), cur, AUI_TEXT, PX);
    /* chevron */
    int ax = x + w - AUI_SP(5), ay = y + h / 2 - 1;
    gui_icon(GICON_CHEVRON_D, ax - 1, ay - 5, 12, AUI_MUTED);
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
