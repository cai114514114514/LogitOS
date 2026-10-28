/* aui_controls.inc -- small controls: tooltip, checkbox, radio, switch, slider,
 * progress, spinner, stepper, badge and separator.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include.
 *
 * ui-aui-controls: design 4.6 / 4.7 / 4.14 / 4.15 / 4.17 (the 「澄」 look).
 * Colours come from aui_tok(); geometry that moves is drawn as 24.8 paths so a
 * spring is not quantised to whole points; every control degrades to integer
 * aui_round primitives on a kernel without SYS_GUI_PATH. */

#define CTL_DIS_ALPHA 102                /* a disabled control's group opacity (4.6) */
/* The accent TOKEN (aui_tok()), not the legacy aui_t.accent field. */
#define CTL_ACC (aui_tok()->accent)
/* The switch (4.6). UI_CONTROLS_NEGCTL is the negative control of
 * test-ui-aui-controls: the old 44 x 24 size. */
#ifdef UI_CONTROLS_NEGCTL
#define CTL_SW_W 44
#define CTL_SW_H 24
#else
#define CTL_SW_W 38
#define CTL_SW_H 22
#endif

/* ---- shared helpers ---------------------------------------------------- */

/* One device pixel, in 24.8 points, at the current backing scale. */
static int ctl_dev_q8(void)
{
    int pct = aui_scale();
    return pct > 0 ? 25600 / pct : 256;
}

static unsigned ctl_alpha(unsigned argb, int a)
{ return ((unsigned)((int)(argb >> 24) * iclamp(a, 0, 255) / 255) << 24) | (argb & 0xFFFFFFu); }

/* A rounded rect in 24.8 points (stroke_q8 == 0 fills; else a stroke whose
 * OUTER edge is the rect), straight ARGB. Falls back to whole-point tiles. */
static void ctl_rr(int x24, int y24, int w24, int h24, int r24, int stroke_q8, unsigned argb)
{
    if (w24 <= 0 || h24 <= 0 || (argb >> 24) == 0) return;
    if (r24 > w24 / 2) r24 = w24 / 2;
    if (r24 > h24 / 2) r24 = h24 / 2;
    if (path_ok()) {
        static const unsigned char verbs[10] = { LOGIT_VERB_M, LOGIT_VERB_L, LOGIT_VERB_C, LOGIT_VERB_L,
            LOGIT_VERB_C, LOGIT_VERB_L, LOGIT_VERB_C, LOGIT_VERB_L, LOGIT_VERB_C, LOGIT_VERB_Z };
        struct logit_path2 pa;
        struct logit_paint2 pt;
        struct logit_stroke2 st;
        int xy[34], n = 0;
        int x0 = x24, y0 = y24, x1 = x24 + w24, y1 = y24 + h24, r = r24;
        int c;
        if (stroke_q8 > 0) {
            x0 += stroke_q8 / 2; y0 += stroke_q8 / 2; x1 -= stroke_q8 / 2; y1 -= stroke_q8 / 2;
            r -= stroke_q8 / 2; if (r < 0) r = 0;
        }
        c = (int)((long long)r * 36195 / 65536);                 /* r * (4/3) tan(pi/8) */
#define P_(a, b) (xy[n++] = (a), xy[n++] = (b))
        P_(x0 + r, y0);  P_(x1 - r, y0);
        P_(x1 - r + c, y0); P_(x1, y0 + r - c); P_(x1, y0 + r);
        P_(x1, y1 - r);
        P_(x1, y1 - r + c); P_(x1 - r + c, y1); P_(x1 - r, y1);
        P_(x0 + r, y1);
        P_(x0 + r - c, y1); P_(x0, y1 - r + c); P_(x0, y1 - r);
        P_(x0, y0 + r);
        P_(x0, y0 + r - c); P_(x0 + r - c, y0); P_(x0 + r, y0);
#undef P_
        gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
        pa.verbs = verbs; pa.xy = xy; pa.nverb = 10; pa.nxy = (unsigned)n;
        pa.rule = LOGIT_PATH2_NONZERO;
        pt.kind = LOGIT_PAINT2_SOLID; pt.argb = argb;
        if (stroke_q8 > 0) { st.width = stroke_q8; st.cap = LOGIT_STROKE2_CAP_BUTT; st.join = LOGIT_STROKE2_JOIN_ROUND; st.miter = 4 * 65536; }
        if (aui_path(&pa, &pt, stroke_q8 > 0 ? &st : 0) == 0) return;
    }
    {
        int x = (x24 + 128) >> 8, y = (y24 + 128) >> 8, w = (w24 + 128) >> 8, h = (h24 + 128) >> 8;
        if (stroke_q8 > 0) aui_stroke(x, y, w, h, (r24 + 128) >> 8, imax(1, (stroke_q8 + 255) >> 8), argb & 0xFFFFFFu);
        else aui_round_a(x, y, w, h, (r24 + 128) >> 8, argb & 0xFFFFFFu, (int)(argb >> 24));
    }
}

/* A polyline of 2..3 points (24.8 pt, window-local) stroked with round caps and
 * joins. 0 when the kernel has no PATH (the caller draws its fallback). */
static int ctl_poly(const int *xy, int npts, int width_q8, unsigned argb)
{
    static const unsigned char verbs[3] = { LOGIT_VERB_M, LOGIT_VERB_L, LOGIT_VERB_L };
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    if (!path_ok() || npts < 2 || npts > 3 || (argb >> 24) == 0) return 0;
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = verbs; pa.xy = xy; pa.nverb = (unsigned)npts; pa.nxy = (unsigned)(2 * npts);
    pt.kind = LOGIT_PAINT2_SOLID; pt.argb = argb;
    st.width = width_q8; st.cap = LOGIT_STROKE2_CAP_ROUND; st.join = LOGIT_STROKE2_JOIN_ROUND;
    st.miter = 4 * 65536;
    return aui_path(&pa, &pt, &st) == 0;
}

/* The box around `r` that a control with its E1 / E2 shadow touches. */
static struct aui_rect ctl_halo(struct aui_rect r, int pad)
{ return aui_r(r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad + 2); }

static void ctl_itoa(char *buf, int v)
{
    char t[12];
    int n = 0, i = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 11);
    if (neg) buf[i++] = '-';
    while (n) buf[i++] = t[--n];
    buf[i] = 0;
}

/* ---- tooltip (4.15) ---------------------------------------------------- */

static const char *tip_sc, *tip_last_sc;          /* the optional shortcut */
static int tip_dw_id;                             /* dwell: widget, frame, start */
static unsigned tip_dw_gen, tip_dw_t0;

void aui_tooltip_ex(const char *s, const char *shortcut)
{
    /* The widget just polled is the one this belongs to. A tooltip that appears
     * instantly is noise, so it waits for the pointer to settle (450 ms). */
    if (!s || id_ctr <= 0 || in.hot != id_ctr) return;
    /* in.hot_t0 cannot be the dwell clock: aui_begin() clears in.hot every frame,
     * so wpoll restamps it every frame and the dwell would never reach 450 ms.
     * The dwell is counted here, over consecutive frames on the same widget. A
     * pointer that has stopped sends no events, so nothing would wake the app at
     * 450 ms: while dwelling this registers the endless-animation wake (50 ms
     * tick, at most nine frames) and stops asking once the tip is up. */
    if (tip_dw_id != id_ctr || tip_dw_gen + 1 != anim_gen) { tip_dw_id = id_ctr; tip_dw_t0 = frame_ms; }
    tip_dw_gen = anim_gen;
    if (frame_ms - tip_dw_t0 < 450) { anim_loop_want = 1; return; }
    tip_text = s; tip_x = in.mx; tip_y = in.my;
    tip_sc = shortcut;
    tip_last_text = s; tip_last_x = in.mx; tip_last_y = in.my; tip_last_owner = id_ctr;
    tip_last_sc = shortcut;
}

void aui_tooltip(const char *s) { aui_tooltip_ex(s, 0); }

static void draw_tip(void)
{
    if (!tip_last_text) return;             /* never shown this app run: nothing to fade */
    /* AUI_AK_EXTRA on the OWNING WIDGET's id (see aui_tooltip_ex): this function
     * runs EVERY frame from aui_end, so the slot never goes stale and the fade
     * out has something to start from. In: t.fadeIn 180 ms STANDARD; out:
     * t.fadeOut 120 ms IN_CUBIC (design 3.8, always faster than entering). */
    int saved_id = id_ctr;
    id_ctr = tip_last_owner;
    int t = tip_text ? aui_anim(AUI_AK_EXTRA, 255, 180, AUI_EASE_STANDARD)
                     : aui_anim(AUI_AK_EXTRA, 0, 120, AUI_EASE_IN_CUBIC);
    id_ctr = saved_id;
    if (t <= 0) { tip_last_text = 0; tip_last_sc = 0; return; }

    int dark = aui_is_dark(), px = 12, padx = 8, pady = 5;
    int lh = aui_text_line_h(px);
    int tw1 = aui_text_w(tip_last_text, px);
    int sw = tip_last_sc ? aui_text_w(tip_last_sc, px) : 0;
    int w = tw1 + (sw ? 8 + sw : 0) + 2 * padx, h = lh + 2 * pady;
    int x = tip_last_x + 12, y = tip_last_y + 22;     /* 6 below the 16-point pointer */
    if (x + w > win_w) x = win_w - w - 2;
    if (y + h > win_h) y = tip_last_y - h - 6;
    if (x < 2) x = 2;
    if (y < 2) y = 2;
    struct aui_rect box = aui_r(x, y, w, h);
    struct aui_rect grp = ctl_halo(box, 10);
    /* The fade is one opacity group over tooltip and shadow when the kernel has
     * groups (children then at full strength); without one fade_leg is the
     * legacy alpha and the shadow is the tile one scaled by it. */
    fade_open(aui_r(X_(grp.x), Y_(grp.y), grp.w, grp.h), t, 1);
    int tt = fade_leg;
    if (tt >= 255) aui_elev(box, 6, LOGIT_E2);
    else aui_shadow_ex(x, y, w, h, 6, 2, 6, (dark ? 87 : 31) * tt / 255);
    aui_round_a(x, y, w, h, 6, dark ? 0x3A3A44u : 0x2A2A32u, (dark ? 240 : 235) * tt / 255);
    aui_text_a(x + padx, y + pady + (lh - px) / 2 - 1, tip_last_text, 0xF4F4F8u, px, tt);
    if (sw) aui_text_a(x + padx + tw1 + 8, y + pady + (lh - px) / 2 - 1, tip_last_sc, 0xF4F4F8u, px, 160 * tt / 255);
    fade_close();
}

/* ---- checkbox (4.6) ---------------------------------------------------- */

/* The checkmark of a 16-point box at (x,y): one 1.75-point stroked polyline
 * with round caps and joins. 0 when the kernel has no PATH. */
static int draw_tick(int x, int y, unsigned argb)
{
    int xy[6];
    xy[0] = x * 256 + 1024; xy[1] = y * 256 + 2099;      /* (4.0, 8.2)  */
    xy[2] = x * 256 + 1715; xy[3] = y * 256 + 2765;      /* (6.7, 10.8) */
    xy[4] = x * 256 + 3021; xy[5] = y * 256 + 1408;      /* (11.8, 5.5) */
    return ctl_poly(xy, 3, 448, argb);                  /* 1.75 pt */
}

/* The unchecked face shared by checkbox and radio: L #FFFFFF, D #FFFFFF @ 13. */
static unsigned ctl_off_fill(void) { return aui_is_dark() ? 0x0DFFFFFFu : 0xFFFFFFFFu; }

int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled)
{
    const struct logit_tokens *k = aui_tok();
    int id = ++id_ctr, box = 16, lw = label ? aui_text_w(label, 14) : 0;
    int w = box + (label ? 8 + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, enabled, 1);
    int off = (r.st & AUI_OFF) != 0;
    /* t.hover (LINEAR) cross-fade of the fill, in place. Queried every frame so
     * the slot stays continuous across the click. */
    int vt = aui_anim(AUI_AK_VALUE, *state ? 255 : 0, 90, AUI_EASE_LINEAR);

    focus_ring(r.st & AUI_FOCUSED, x, y, box, box, 4);
    /* The disabled group must hold everything drawn inside it: the label is
     * laid out in (x + 24, y - 2, lw + 2, box + 4), and the run2 the kernel
     * draws there is wider than the legacy whole-point measure lw (the guest
     * cut "Disabled on" to "Disabled o" with a box-tall, lw-wide group, and
     * still clipped the last glyph with 4 pt of slack). The group is only a
     * compositing layer, so it is sized generously: a quarter of lw plus
     * 12 pt past the measure, and 4 pt above and below the box. */
    if (off) {
        int lx = label ? box + 8 + lw + lw / 4 + 12 : box;
        aui_fade_begin(aui_r(x, y - 4, lx, box + 8), CTL_DIS_ALPHA);
    }
    aui_round_a(x, y, box, box, 4, ctl_off_fill() & 0xFFFFFFu, (int)(ctl_off_fill() >> 24));
    if (vt > 0) aui_round_a(x, y, box, box, 4, CTL_ACC, vt);
    if (vt < 255) aui_stroke_a(aui_r(x, y, box, box), 4, ctl_dev_q8(), ctl_alpha(k->sep_strong, 255 - vt));
    if (!off && (r.st & AUI_HOVER))
        aui_round_a(x, y, box, box, 4, k->fill_hover & 0xFFFFFFu, (int)(k->fill_hover >> 24));
    if (*state == 2) {
        if (vt > 0) aui_round_a(x + 4, y + 7, 8, 2, 1, 0xFFFFFFu, vt);
    } else if (vt > 0) {
        unsigned c = ((unsigned)vt << 24) | 0xFFFFFFu;
        if (!draw_tick(x, y, c)) {
            /* No SYS_GUI_PATH: bars */
            aui_round_a(x + 3, y + 7, 5, 3, 1, 0xFFFFFFu, vt);
            aui_round_a(x + 6, y + 6, 7, 3, 1, 0xFFFFFFu, vt);
        }
    }
    if (label)
        aui_text_vc(aui_r(x + box + 8, y - 2, lw + 2, box + 4), label, 14, AUI_W_REGULAR, k->label_primary, AUI_ALIGN_LEFT);
    if (off) aui_fade_end();
    if (r.clicked) { *state = (*state != 1); return 1; }
    return 0;
}

int aui_checkbox(int x, int y, const char *label, int *state)
{ return aui_checkbox_ex(x, y, label, state, 1); }

/* ---- radio (4.6) ------------------------------------------------------- */

int aui_radio(int x, int y, const char *label, int *group, int value)
{
    const struct logit_tokens *k = aui_tok();
    int id = ++id_ctr, box = 16, lw = label ? aui_text_w(label, 14) : 0;
    int w = box + (label ? 8 + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, 1, 1);
    int on = (*group == value);
    if (rg_ptr != group) { rg_ptr = group; rg_lo_a = rg_hi_a = value; }
    else { if (value < rg_lo_a) rg_lo_a = value; if (value > rg_hi_a) rg_hi_a = value; }
    int cx = x + box / 2, cy = y + box / 2;
    focus_ring(r.st & AUI_FOCUSED, x, y, box, box, box / 2);
    int vt = aui_anim(AUI_AK_VALUE, on ? 255 : 0, 90, AUI_EASE_LINEAR);
    aui_round_a(x, y, box, box, box / 2, ctl_off_fill() & 0xFFFFFFu, (int)(ctl_off_fill() >> 24));
    if (vt > 0) aui_round_a(x, y, box, box, box / 2, CTL_ACC, vt);
    if (vt < 255) aui_stroke_a(aui_r(x, y, box, box), box / 2, ctl_dev_q8(), ctl_alpha(k->sep_strong, 255 - vt));
    if (!(r.st & AUI_OFF) && (r.st & AUI_HOVER))
        aui_round_a(x, y, box, box, box / 2, k->fill_hover & 0xFFFFFFu, (int)(k->fill_hover >> 24));
    if (vt > 0) aui_round_a(cx - 3, cy - 3, 6, 6, 3, 0xFFFFFFu, vt);       /* the white O 6 dot */
    if (label)
        aui_text_vc(aui_r(x + box + 8, y - 2, lw + 2, box + 4), label, 14, AUI_W_REGULAR, k->label_primary, AUI_ALIGN_LEFT);
    /* Arrows walk a radio group, which is what makes it a group and not three
     * unrelated buttons. */
    if ((r.st & AUI_FOCUSED) && in.ev == EV_KEY && !in.key_used &&
        (in.a == KEY_DOWN || in.a == KEY_RIGHT || in.a == KEY_UP || in.a == KEY_LEFT)) {
        int d = (in.a == KEY_DOWN || in.a == KEY_RIGHT) ? 1 : -1;
        int nv = (rg_hi > rg_lo) ? iclamp(value + d, rg_lo, rg_hi) : value + d;
        in.key_used = 1;
        if (nv != value) { *group = nv; aui_focus_next(d); return 1; }
        return 0;
    }
    if (r.clicked && !on) { *group = value; return 1; }
    return 0;
}

/* ---- switch (4.6) ------------------------------------------------------ */

/* THE CLICK IS HANDLED BEFORE THE ANIMATION IS SAMPLED: toggling *state after
 * drawing would make the frame that handles the press draw the OLD position
 * and read as a dropped click. The knob rides AUI_SPRING_SNAPPY (~120 ms) on
 * a 24.8 position, so it settles and then asks for no more frames; its width
 * has its own spring (18 -> 22 while pressed, toward the travel direction).
 * The track colour tweens over t.hover, LINEAR. No glass: the track is solid. */
int aui_toggle(int x, int y, int *state, int enabled)
{
    const struct logit_tokens *k = aui_tok();
    int id = ++id_ctr, w = CTL_SW_W, h = CTL_SW_H, kd = 18, trav = 16;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, enabled, 1);
    int changed = 0, off = (r.st & AUI_OFF) != 0;
    if (r.clicked) { *state = !*state; changed = 1; }

    int t = aui_anim(AUI_AK_VALUE, *state ? 255 : 0, 90, AUI_EASE_LINEAR);
    /* Snappy on the raw 24.8 travel (16 pt): it settles in ~148 ms. The design's
     * ~120 ms is for a ~10 pt move (tests/ui_aui_motion_api_test.c limits a
     * 10 pt step to 140 ms); scaling the spring's units to meet 140 here would
     * snap the last 0.5 pt, so the knob keeps the honest physics. */
    int pos24 = aui_spring_ex(AUI_AK_VALUE, *state ? trav * 256 : 0, AUI_SPRING_SNAPPY);
    int pressed = (r.st & AUI_ACTIVE) && in.down && !off;
    int kw24 = aui_spring_ex(AUI_AK_FACE, pressed ? (kd + 4) * 256 : kd * 256, AUI_SPRING_SNAPPY);
    kw24 = iclamp(kw24, kd * 256, (kd + 4) * 256);

    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, h / 2);
    if (off) aui_fade_begin(aui_r(x, y, w, h), CTL_DIS_ALPHA);
    /* track: fill.control with a 1 dev inner sep.opaque, tweening to accent */
    aui_round(x, y, w, h, h / 2, k->fill_control);
    if (t < 255) aui_stroke_a(aui_r(x, y, w, h), h / 2, ctl_dev_q8(), ctl_alpha(0xFF000000u | k->sep_opaque, 255 - t));
    if (t > 0) aui_round_a(x, y, w, h, h / 2, CTL_ACC, t);
    if (!off && (r.st & AUI_HOVER))
        aui_round_a(x, y, w, h, h / 2, k->fill_hover & 0xFFFFFFu, (int)(k->fill_hover >> 24));

    /* knob: white, E1, plus a 1 dev #141428 @ 20 ring */
    /* the leading edge (the side it is travelling toward) is the one that
     * stretches; the knob stays inside the track */
    int lx24 = (x + 2) * 256 + pos24, rx24;
    if (*state) { rx24 = lx24 + kw24; if (rx24 > (x + 38) * 256 - 2 * 256) { rx24 = (x + 38 - 2) * 256; lx24 = rx24 - kw24; } }
    else { rx24 = lx24 + kd * 256; lx24 = rx24 - kw24; if (lx24 < (x + 2) * 256) { lx24 = (x + 2) * 256; rx24 = lx24 + kw24; } }
    int ky = y + (h - kd) / 2;
    struct aui_rect kr = aui_r((lx24 + 128) >> 8, ky, (rx24 - lx24 + 128) >> 8, kd);
    aui_elev(kr, kd / 2, LOGIT_E1);
    ctl_rr(lx24, ky * 256, rx24 - lx24, kd * 256, kd * 128, 0, 0xFFFFFFFFu);
    ctl_rr(lx24, ky * 256, rx24 - lx24, kd * 256, kd * 128, ctl_dev_q8(), 0x14141428u);
    if (off) aui_fade_end();
    return changed;
}

/* ---- slider (4.7) ------------------------------------------------------ */

int aui_slider_ex(int x, int y, int w, int *value, int lo, int hi, unsigned flags,
                  const char *lo_cap, const char *hi_cap, int step)
{
    const struct logit_tokens *k = aui_tok();
    int id = ++id_ctr, h = 22;
    int enabled = !(flags & AUI_SLIDER_DISABLED);
    int vcol = (flags & AUI_SLIDER_VALUE) ? 44 + 8 : 0;
    int tw_ = w - vcol;
    if (tw_ < 8) tw_ = 8;
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll(id, wx - 12, wy, tw_ + 24, h, enabled, 1);
    if (hi <= lo) hi = lo + 1;
    int v = iclamp(*value, lo, hi), changed = 0;

    /* Drag: the press captured the widget, and motion goes to the capture
     * target, so this keeps tracking after the pointer leaves the groove. */
    int dragging = enabled && (in.active == id && in.down);
    if (enabled && (r.clicked || (dragging && in.ev == EV_MOUSE_MOVE))) {
        int nv = lo + (int)(((long long)(in.mx - wx) * (hi - lo) + tw_ / 2) / tw_);
        nv = iclamp(nv, lo, hi);
        if (step > 1) nv = iclamp(lo + ((nv - lo + step / 2) / step) * step, lo, hi);
        if (nv != v) { v = nv; changed = 1; }
    }
    if (enabled && (r.st & AUI_FOCUSED) && in.ev == EV_KEY && !in.key_used) {
        int ks = step > 0 ? step : imax(1, (hi - lo) / 50), nv = v;
        if (in.mods & EV_MOD_SHIFT) ks *= 10;
        if (in.a == KEY_LEFT || in.a == KEY_DOWN) nv = v - ks;
        else if (in.a == KEY_RIGHT || in.a == KEY_UP) nv = v + ks;
        else if (in.a == KEY_HOME) nv = lo;
        else if (in.a == KEY_END)  nv = hi;
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; in.key_used = 1; }
    }
    *value = v;

    /* A dragged knob sits under the finger with zero lag, so a drag or a click
     * draws the raw position; every other move (a key, a programmatic change)
     * rides the snappy spring. The spring slot goes stale for the duration of a
     * drag and the continuity rule re-latches it at the raw position. */
    int fill_raw24 = (int)((long long)(v - lo) * tw_ * 256 / (hi - lo));
    int fill24;
    if (dragging || r.clicked) fill24 = fill_raw24;
    else fill24 = aui_spring_ex(AUI_AK_VALUE, fill_raw24, AUI_SPRING_SNAPPY);
    fill24 = iclamp(fill24, 0, tw_ * 256);
    /* knob: 20, hover 22, drag 24 */
    int want = dragging ? 24 : (r.st & AUI_HOVER) ? 22 : 20;
    int d24 = aui_spring_ex(AUI_AK_FACE, want * 256, AUI_SPRING_SNAPPY);

    int ty = y + h / 2 - 2;
    if (!enabled) aui_fade_begin(aui_r(x - 12, y, tw_ + 24 + vcol, h + 24), CTL_DIS_ALPHA);
    /* ticks and captions sit under the track */
    int below = ty + 4;
    if ((flags & AUI_SLIDER_TICKS) && step > 0) {
        for (int tv = lo; tv <= hi; tv += step) {
            int tx = x + (int)((long long)(tv - lo) * tw_ / (hi - lo));
            aui_fill_a(tx - 1, below + 2, 2, 6, k->label_quaternary, 255);
        }
        below += 8;
    }
    if (lo_cap) aui_text_draw_w(x, below + 6, 12, AUI_W_REGULAR, k->label_tertiary, lo_cap);
    if (hi_cap) aui_text_draw_w(x + tw_ - aui_text_w(hi_cap, 12), below + 6, 12, AUI_W_REGULAR, k->label_tertiary, hi_cap);

    aui_round(x, ty, tw_, 4, 2, k->fill_control);
    ctl_rr(x * 256, ty * 256, fill24, 4 * 256, 2 * 256, 0, 0xFF000000u | CTL_ACC);

    int cx24 = x * 256 + fill24, cy = y + h / 2;
    int lx24 = cx24 - d24 / 2, ly24 = cy * 256 - d24 / 2;
    focus_ring(r.st & AUI_FOCUSED, ((lx24 + 128) >> 8), ((ly24 + 128) >> 8), (d24 + 128) >> 8, (d24 + 128) >> 8, 12);
    /* the drag halo: 4 pt of accent @ 51, growing in with the knob */
    {
        int g = iclamp(d24 - 22 * 256, 0, 2 * 256);
        if (g > 0)
            ctl_rr(lx24 - 4 * 256, ly24 - 4 * 256, d24 + 8 * 256, d24 + 8 * 256, d24 / 2 + 4 * 256, 4 * 256,
                   ctl_alpha(0xFF000000u | CTL_ACC, 51 * g / (2 * 256)));
    }
    struct aui_rect kr = aui_r((lx24 + 128) >> 8, (ly24 + 128) >> 8, (d24 + 128) >> 8, (d24 + 128) >> 8);
    aui_elev(kr, kr.w / 2, d24 > 23 * 256 ? LOGIT_E2 : LOGIT_E1);
    ctl_rr(lx24, ly24, d24, d24, d24 / 2, 0, 0xFFFFFFFFu);
    ctl_rr(lx24, ly24, d24, d24, d24 / 2, ctl_dev_q8(), 0x14141428u);

    if (flags & AUI_SLIDER_VALUE) {
        char buf[16];
        ctl_itoa(buf, v);
        aui_text_tnum(x + tw_ + 8 + 44, y + (h - 13) / 2 - 1, 13, AUI_W_REGULAR, k->label_secondary, buf, AUI_ALIGN_RIGHT);
    }
    if (!enabled) aui_fade_end();
    return changed;
}

int aui_slider(int x, int y, int w, int *value, int lo, int hi)
{ return aui_slider_ex(x, y, w, value, lo, hi, 0, 0, 0, 0); }

/* ---- progress (4.14) --------------------------------------------------- */

void aui_progress(int x, int y, int w, int pct)
{
    const struct logit_tokens *k = aui_tok();
    int h = 6, id = ++id_ctr;
    (void)id;
    if (w <= 0) return;
    aui_round(x, y, w, h, h / 2, k->fill_control);
    if (pct >= 0) {
        /* value changes ease on the smooth spring (no overshoot) */
        int target = (int)((long long)iclamp(pct, 0, 100) * w * 256 / 100);
        int fw24 = iclamp(aui_spring_ex(AUI_AK_VALUE, target, AUI_SPRING_SMOOTH), 0, w * 256);
        int fw = (fw24 + 128) >> 8;
        if (fw > 0) {
            /* accent with a +6 % lighter top */
            aui_vgrad_round(x, y, fw, h, imin(h / 2, fw / 2), aui_mix(CTL_ACC, 0xFFFFFFu, 15), CTL_ACC);
        }
    } else {
        /* Indeterminate: a 30 %-wide capsule sweeping the track in 1.2 s
         * INOUT_CUBIC. Under Reduce Motion it freezes mid-track and asks for no
         * frames. aui_anim_loop() registers the endless animation (50 ms tick:
         * 20 Hz, under the 30 Hz ceiling) and damages only this bar. */
        int seg = w * 3 / 10;
        int bx;
        if (anim_reduced) bx = x + (w - seg) / 2;
        else {
            unsigned ph = aui_anim_loop() % 1200u;
            int e = aui_ease_eval(AUI_EASE_INOUT_CUBIC, (int)(ph * 256u / 1200u));
            bx = x - seg + (w + seg) * e / 256;
        }
        int x0 = imax(bx, x), x1 = imin(bx + seg, x + w);
        if (x1 > x0) aui_round(x0, y, x1 - x0, h, h / 2, CTL_ACC);
    }
}

/* ---- spinner (4.14) ---------------------------------------------------- */

void aui_spinner_ex(int cx, int cy, int d)
{
    if (d <= 0) d = 24;
    /* Spinners carry meaning, so they keep turning under Reduce Motion: they
     * register their endless animation directly instead of through
     * aui_anim_loop(), which declines when motion is reduced. */
    anim_loop_want = 1;
    {
        /* one revolution per 900 ms; the angle moves at <= 20 Hz (24-32) and
         * <= 12 Hz for the 16-point size (a 84 ms quantum). */
        unsigned quantum = d <= 20 ? 84u : 50u;
        unsigned t = frame_ms - frame_ms % quantum;
        int ang = (int)(t % 900u) * 360 / 900;
        int sw24 = d <= 20 ? 512 : d <= 28 ? 640 : 768;           /* 2 / 2.5 / 3 pt */
        if (path_ok()) {
            static const unsigned char verbs[4] = { LOGIT_VERB_M, LOGIT_VERB_C, LOGIT_VERB_C, LOGIT_VERB_C };
            long long R = (long long)d * 128 - sw24 / 2, CX = (long long)X_(cx) * 256, CY = (long long)Y_(cy) * 256;
            int xy[20], n = 0;
            long long kq = 36195;                                   /* (4/3) tan(pi/8), 16.16 */
            long long hx = 0, hy = 0, tx = 0, ty = 0;
            struct logit_path2 pa;
            struct logit_paint2 pt;
            struct logit_stroke2 st;
            unsigned stop_pos[2], stop_argb[2];
            for (int i = 0; i < 3; i++) {
                int a0 = (ang + 90 * i) * 256, a1 = a0 + 90 * 256;
                long long s0 = gfx_sin(a0), c0 = gfx_cos(a0), s1 = gfx_sin(a1), c1 = gfx_cos(a1);
                long long x0 = CX + (R * c0 >> 16), y0 = CY + (R * s0 >> 16);
                long long x3 = CX + (R * c1 >> 16), y3 = CY + (R * s1 >> 16);
                if (i == 0) { xy[n++] = (int)x0; xy[n++] = (int)y0; tx = x0; ty = y0; }
                xy[n++] = (int)(x0 - (R * s0 >> 16) * kq / 65536); xy[n++] = (int)(y0 + (R * c0 >> 16) * kq / 65536);
                xy[n++] = (int)(x3 + (R * s1 >> 16) * kq / 65536); xy[n++] = (int)(y3 - (R * c1 >> 16) * kq / 65536);
                xy[n++] = (int)x3; xy[n++] = (int)y3;
                hx = x3; hy = y3;
            }
            gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
            pa.verbs = verbs; pa.xy = xy; pa.nverb = 4; pa.nxy = (unsigned)n;
            /* LINEAR paint head -> tail: accent to accent @ 0 approximates the tail */
            stop_pos[0] = 0; stop_pos[1] = 65536;
            stop_argb[0] = 0xFF000000u | (CTL_ACC & 0xFFFFFFu);
            stop_argb[1] = CTL_ACC & 0xFFFFFFu;
            pt.kind = LOGIT_PAINT2_LINEAR; pt.nstop = 2; pt.stop_pos = stop_pos; pt.stop_argb = stop_argb;
            pt.x0 = (int)hx; pt.y0 = (int)hy; pt.x1 = (int)tx; pt.y1 = (int)ty;
            st.width = sw24; st.cap = LOGIT_STROKE2_CAP_ROUND; st.join = LOGIT_STROKE2_JOIN_ROUND;
            st.miter = 4 * 65536;
            /* Straight to the kernel, not through aui_path(): its damage box is the
             * control-point bbox plus a round-join halo, a few points larger than
             * the spinner. The damage is the spinner's own square (the outer edge
             * of the stroke is d / 2 from the centre) plus 1 point of AA. */
            ad_note_path(X_(cx) - d / 2, Y_(cy) - d / 2, d, d, 1, verbs, 4, xy, n, (unsigned)sw24 ^ CTL_ACC);
            if (ol_window_path2(&pa, &pt, &st) == 0) return;
        }
        {
            /* No PATH: eight fading dots, integer only. */
            static const int ux[8] = { 0, 181, 256, 181, 0, -181, -256, -181 };
            static const int uy[8] = { -256, -181, 0, 181, 256, 181, 0, -181 };
            unsigned phase = (t / 100) % 8;
            int r = d / 2 - 2;
            for (int i = 0; i < 8; i++) {
                int a = 40 + (int)((i + 8 - phase) % 8) * 27;
                aui_round_a(cx + ux[i] * r / 256 - 2, cy + uy[i] * r / 256 - 2, 4, 4, 2, CTL_ACC, a);
            }
        }
    }
}

void aui_spinner(int cx, int cy, int r) { aui_spinner_ex(cx, cy, 2 * r); }

/* ---- stepper (4.17) ---------------------------------------------------- */

/* A 10-point glyph stroke centred on (cx,cy): minus, or plus with `plus`. */
static void ctl_glyph_pm(int cx, int cy, int plus, unsigned argb)
{
    int xy[4];
    xy[0] = (cx - 5) * 256; xy[1] = cy * 256; xy[2] = (cx + 5) * 256; xy[3] = cy * 256;
    if (!ctl_poly(xy, 2, 384, argb)) aui_round_a(cx - 5, cy - 1, 10, 2, 1, argb & 0xFFFFFFu, (int)(argb >> 24));
    if (plus) {
        xy[0] = cx * 256; xy[1] = (cy - 5) * 256; xy[2] = cx * 256; xy[3] = (cy + 5) * 256;
        if (!ctl_poly(xy, 2, 384, argb)) aui_round_a(cx - 1, cy - 5, 2, 10, 1, argb & 0xFFFFFFu, (int)(argb >> 24));
    }
}

int aui_stepper(int x, int y, int *value, int lo, int hi, int step, int show_value)
{
    const struct logit_tokens *k = aui_tok();
    int id = ++id_ctr, h = 28, hw = 24, vw = show_value ? 44 : 0, w = 2 * hw + vw;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, 1, 1);
    if (hi < lo) hi = lo;
    if (step < 1) step = 1;
    int v = iclamp(*value, lo, hi), changed = 0;
    int lx = in.mx - X_(0), ly = in.my - Y_(0);
    int hot_minus = (r.st & AUI_HOVER) && lx < x + hw;
    int hot_plus  = (r.st & AUI_HOVER) && lx >= x + hw + vw;
    (void)ly;
    if (r.clicked && in.ev == EV_MOUSE) {
        int nv = v;
        if (lx < x + hw) nv = v - step;
        else if (lx >= x + hw + vw) nv = v + step;
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; }
    }
    if ((r.st & AUI_FOCUSED) && in.ev == EV_KEY && !in.key_used) {
        int nv = v;
        if (in.a == KEY_LEFT || in.a == KEY_DOWN) nv = v - step;
        else if (in.a == KEY_RIGHT || in.a == KEY_UP) nv = v + step;
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; }
        if (in.a == KEY_LEFT || in.a == KEY_DOWN || in.a == KEY_RIGHT || in.a == KEY_UP) in.key_used = 1;
    }
    *value = v;

    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, h / 2);
    /* the Secondary capsule */
    struct aui_rect cap = aui_r(x, y, w, h);
    aui_elev(cap, h / 2, LOGIT_E1);
    aui_round(x, y, w, h, h / 2, k->fill_control);
    {
        int rl[4] = { 14, 0, 0, 14 }, rr[4] = { 0, 14, 14, 0 };
        if (hot_minus && v > lo)
            aui_round4(aui_r(x, y, hw, h), rl, (r.st & AUI_ACTIVE) && in.down ? k->fill_pressed : k->fill_hover);
        if (hot_plus && v < hi)
            aui_round4(aui_r(x + hw + vw, y, hw, h), rr, (r.st & AUI_ACTIVE) && in.down ? k->fill_pressed : k->fill_hover);
    }
    aui_hairline_dev(X_(x + hw), Y_(y + 7), X_(x + hw), Y_(y + h - 7), k->sep_hairline);
    if (vw) aui_hairline_dev(X_(x + hw + vw), Y_(y + 7), X_(x + hw + vw), Y_(y + h - 7), k->sep_hairline);
    ctl_glyph_pm(x + hw / 2, y + h / 2, 0, 0xFF000000u | (v > lo ? k->label_primary : k->label_quaternary));
    ctl_glyph_pm(x + hw + vw + hw / 2, y + h / 2, 1, 0xFF000000u | (v < hi ? k->label_primary : k->label_quaternary));
    if (vw) {
        char buf[16];
        ctl_itoa(buf, v);
        aui_text_tnum(x + hw + vw / 2 + aui_text_tnum_width(buf, 13, AUI_W_REGULAR) / 2, y + (h - 13) / 2 - 1,
                      13, AUI_W_REGULAR, k->label_primary, buf, AUI_ALIGN_RIGHT);
    }
    return changed;
}

/* ---- badge (4.15) ------------------------------------------------------ */

void aui_badge(int x, int y, const char *s, unsigned color)
{
    int px = AUI_FS_CAPTION, pad = AUI_SP(2);
    int w = aui_text_w(s, px) + 2 * pad, h = px + AUI_SP(2) + 2;
    /* 4.15: a tint fill (the colour @ 31 light / @ 51 dark) and the ink text,
     * no stroke -- a saturated ring read as an outlined button in dark. */
    aui_round_a(x, y, w, h, h / 2, color, aui_is_dark() ? 51 : 31);
    aui_text_in(aui_r(x, y, w, h), s, color, px, AUI_ALIGN_CENTER);
}

int aui_badge_ex(int x, int y, const char *s, int kind)
{
    const struct logit_tokens *k = aui_tok();
    int ascii = 1;
    for (const char *p = s; p && *p; p++) if ((unsigned char)*p >= 0x80) { ascii = 0; break; }
    int px = ascii ? 11 : 12, wt = ascii ? AUI_W_SEMIBOLD : AUI_W_MEDIUM;
    int h = 18, w = (s ? aui_text_measure_w(s, px, wt) : 0) + 12;
    unsigned fill, ink;
    if (w < h) w = h;
    switch (kind) {
    case AUI_BADGE_ACCENT:  fill = k->accent_tint; ink = k->accent_ink; break;
    case AUI_BADGE_SUCCESS: fill = 0x29000000u | (k->success & 0xFFFFFFu); ink = k->success_ink; break;
    case AUI_BADGE_WARNING: fill = 0x29000000u | (k->warning & 0xFFFFFFu); ink = k->warning_ink; break;
    case AUI_BADGE_ERROR:   fill = 0x29000000u | (k->error & 0xFFFFFFu);   ink = k->error_ink; break;
    default:                fill = 0xFF000000u | k->fill_control; ink = k->label_secondary; break;
    }
    aui_round_a(x, y, w, h, h / 2, fill & 0xFFFFFFu, (int)(fill >> 24));
    if (s) aui_text_vc(aui_r(x, y, w, h), s, px, wt, ink, AUI_ALIGN_CENTER);
    return w;
}

void aui_separator(int x, int y, int w) { aui_hairline(x, y, w); }
