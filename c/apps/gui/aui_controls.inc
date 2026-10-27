/* aui_controls.inc -- small controls: tooltip, tick, checkbox, radio, toggle,
 * slider, progress, spinner, badge and separator.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

void aui_tooltip(const char *s)
{
    /* The widget just polled is the one this belongs to. A tooltip that appears
     * instantly is noise, so it waits for the pointer to settle. */
    if (!s || in.hot != id_ctr) return;
    if (frame_ms - in.hot_t0 < 450) return;
    tip_text = s; tip_x = in.mx; tip_y = in.my;
    tip_last_text = s; tip_last_x = in.mx; tip_last_y = in.my; tip_last_owner = id_ctr;
}

static void draw_tip(void)
{
    if (!tip_last_text) return;             /* never shown this app run: nothing to fade */
    /* AUI_AK_EXTRA on the OWNING WIDGET's id, not a slot of its own -- the
     * tooltip has no widget identity of its own to key on, but it borrows the
     * owner's for exactly as long as the fade needs it. Reading through a
     * borrowed id_ctr is safe here specifically because this function, unlike
     * aui_dialog_begin, runs EVERY frame regardless of whether a tooltip is
     * currently requested -- so the slot never goes stale and the continuity
     * rule that would otherwise swallow this fade never fires. See the
     * comment on tip_last_text above. */
    int saved_id = id_ctr;
    id_ctr = tip_last_owner;
    int t = aui_anim(AUI_AK_EXTRA, tip_text ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    id_ctr = saved_id;
    if (t <= 0) { tip_last_text = 0; return; }

    int pad = AUI_SP(2), px = AUI_FS_LABEL;
    int w = aui_text_w(tip_last_text, px) + 2 * pad, h = px + 2 * pad;
    int x = tip_last_x + 12, y = tip_last_y + 18;
    if (x + w > win_w) x = win_w - w - 2;
    if (y + h > win_h) y = tip_last_y - h - 6;
    if (x < 2) x = 2;
    if (y < 2) y = 2;
    /* The fade is one opacity group over the tooltip and its shadow when the
     * kernel has groups (the children are then drawn at full strength). Without
     * one it is the old faux alpha: fade_leg is `t`, and the colour is mixed
     * toward the background as it climbs -- true colour exactly at 255, invisible
     * at 0. Geometry never moves. */
    unsigned bg = aui_is_dark() ? rgb(70, 72, 84) : rgb(48, 50, 60);
    int hl[4];
    ad_halo_shadow(0, 3, 10, 0, hl);
    fade_open(aui_r(x - hl[0], y - hl[1], w + hl[0] + hl[2], h + hl[1] + hl[3]), t, 1);
    int tt = fade_leg;
    aui_shadow_ex(x, y, w, h, AUI_R_SM, 3, 10, (aui_is_dark() ? 120 : 55) * tt / 255);
    aui_round_a(x, y, w, h, AUI_R_SM, bg, tt);
    aui_text_sz(x + pad, y + pad - 1, tip_last_text, aui_mix(bg, rgb(250, 250, 252), tt), px);
    fade_close();
}

/* The checkmark of an 18-point box at (x,y): one 2-point stroked polyline with
 * round caps and joins, which stays sharp at any scale (the bars it replaces
 * are 3-point rounded rects that fatten at 150%). 0 when the kernel has no
 * SYS_GUI_PATH, so the caller draws the bars. */
static int draw_tick(int x, int y, unsigned color)
{
    static const unsigned char verbs[3] = { LOGIT_VERB_M, LOGIT_VERB_L, LOGIT_VERB_L };
    struct logit_path2 pa;
    struct logit_paint2 pt;
    struct logit_stroke2 st;
    int xy[6];
    if (!path_ok()) return 0;
    xy[0] = x * 256 + 1152; xy[1] = y * 256 + 2304;      /* (4.5, 9)    */
    xy[2] = x * 256 + 1920; xy[3] = y * 256 + 3072;      /* (7.5, 12)   */
    xy[4] = x * 256 + 3456; xy[5] = y * 256 + 1536;      /* (13.5, 6)   */
    gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
    pa.verbs = verbs; pa.xy = xy; pa.nverb = 3; pa.nxy = 6;
    pt.kind = LOGIT_PAINT2_SOLID; pt.argb = 0xFF000000u | (color & 0xFFFFFFu);
    st.width = 2 * 256; st.cap = LOGIT_STROKE2_CAP_ROUND; st.join = LOGIT_STROKE2_JOIN_ROUND;
    st.miter = 4 * 65536;
    return aui_path(&pa, &pt, &st) == 0;
}

int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled)
{
    int id = ++id_ctr, box = 18, lw = label ? tw(label) : 0;
    int w = box + (label ? AUI_SP(2) + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, enabled, 1);
    /* AUI_T_FAST, not BASE: this is a colour change IN PLACE, not geometry --
     * the box never moves or resizes, only its fill tone slides between
     * unchecked and checked. Queried every frame regardless of *state, so the
     * slot stays continuous across the click and the release fades instead of
     * snapping, the same reasoning as face_for(). */
    int vt = aui_anim(AUI_AK_VALUE, *state ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    unsigned fill = (r.st & AUI_OFF) ? (*state ? AUI_DISABLED_TX : AUI_DISABLED)
                                      : aui_mix(AUI_SURFACE, AUI_ACCENT, vt);

    focus_ring(r.st & AUI_FOCUSED, x, y, box, box, AUI_R_SM);
    aui_round(x, y, box, box, AUI_R_SM, fill);
    /* The hover wash stays an instant overlay rather than a second animated
     * fill: a pointer sweeping a column of checkboxes crosses a dozen of them
     * in a couple of hundred milliseconds, and a fade there would still be
     * arriving when the pointer has already left three boxes down. */
    if (!(r.st & AUI_OFF) && (r.st & AUI_HOVER))
        aui_round_a(x, y, box, box, AUI_R_SM, *state ? rgb(255, 255, 255) : AUI_TEXT, *state ? 26 : 14);
    if (!*state) aui_stroke(x, y, box, box, AUI_R_SM, 1, (r.st & AUI_OFF) ? AUI_DISABLED : AUI_BORDER);
    if (*state) {
        /* The tick is three anti-aliased rounded bars, not a bitmap: it has to
         * stay sharp at 2x like the text next to it. */
        unsigned c = AUI_ACCENT_TEXT;
        if (!draw_tick(x, y, c)) {
            /* No SYS_GUI_PATH: the three-bar tick */
            aui_round(x + 4,  y + box / 2 - 1, 5, 3, 1, c);
            aui_round(x + 6,  y + box / 2 + 1, 3, 3, 1, c);
            aui_round(x + 8,  y + box / 2 - 1, 3, 3, 1, c);
            aui_round(x + 10, y + box / 2 - 3, 3, 3, 1, c);
            aui_round(x + 12, y + 5,           3, 3, 1, c);
        }
    }
    if (label) txt(x + box + AUI_SP(2), y + (box - PX) / 2 - 1,
                   (r.st & AUI_OFF) ? AUI_DISABLED_TX : AUI_TEXT, label);
    if (r.clicked) { *state = !*state; return 1; }
    return 0;
}

int aui_checkbox(int x, int y, const char *label, int *state)
{ return aui_checkbox_ex(x, y, label, state, 1); }

int aui_radio(int x, int y, const char *label, int *group, int value)
{
    int id = ++id_ctr, box = 18, lw = label ? tw(label) : 0;
    int w = box + (label ? AUI_SP(2) + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, 1, 1);
    int on = (*group == value);
    if (rg_ptr != group) { rg_ptr = group; rg_lo_a = rg_hi_a = value; }
    else { if (value < rg_lo_a) rg_lo_a = value; if (value > rg_hi_a) rg_hi_a = value; }
    int cx = x + box / 2, cy = y + box / 2;
    focus_ring(r.st & AUI_FOCUSED, x, y, box, box, box / 2);
    /* Same shape as the checkbox: FAST colour cross-fade for the selected
     * state, an instant hover wash on top (a radio group is swept the same
     * way a checkbox column is), and the inner dot stays an instant pop --
     * matching the checkbox tick's rejection, see anim-no-checkbox-tick. */
    int vt = aui_anim(AUI_AK_VALUE, on ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    aui_circle(cx, cy, box / 2, aui_mix(AUI_SURFACE, AUI_ACCENT, vt));
    if (!on && (r.st & AUI_HOVER)) aui_round_a(x, y, box, box, box / 2, AUI_TEXT, 14);
    if (!on) aui_ring(cx, cy, box / 2, 1, AUI_BORDER);
    else     aui_circle(cx, cy, box / 5, AUI_ACCENT_TEXT);
    if (label) txt(x + box + AUI_SP(2), y + (box - PX) / 2 - 1, AUI_TEXT, label);
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

/* THE PROOF WIDGET for the motion core in section 5c, and it is this one for a
 * reason that is not aesthetic: aui.h has documented the switch knob as a
 * clock-driven animation since the toggle shipped, and `kx` teleported the knob
 * 22 points in a single frame. Implementing it closes a documentation lie
 * rather than adding a feature.
 *
 * TWO THINGS BECOME ONE CLOCK. The knob's position and the track's colour both
 * read the SAME slot, so they cannot drift apart into a knob that has arrived
 * over a track that has not. The knob's SIZE never changes, so its circle mask
 * is a gfx cache hit on every frame of the slide -- only x moves.
 *
 * THE CLICK IS HANDLED BEFORE THE ANIMATION IS SAMPLED, which is not the order
 * the old code used. Toggling *state after drawing would have made the frame
 * that handles the press draw the OLD position, so the knob would sit still for
 * one frame -- ~21 ms of a 180 ms motion -- and read as a dropped click. */
int aui_toggle(int x, int y, int *state, int enabled)
{
    int id = ++id_ctr, w = 44, h = 24;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, enabled, 1);
    int changed = 0;
    if (r.clicked) { *state = !*state; changed = 1; }

    /* AUI_T_BASE, not AUI_T_FAST: this is GEOMETRY. In a 640x480 pt window at
     * 150% the canvas is ~691,200 device px, so 90 ms delivers four frames and
     * the knob's 33 device-px travel would arrive in four 8-px jumps -- a
     * strobe. 180 ms delivers eight, i.e. ~4 px a step, which reads as motion.
     * The same split is why the track colour could have been FAST and is not:
     * it shares this slot so the two cannot disagree. */
    int t = aui_anim(AUI_AK_VALUE, *state ? 255 : 0, AUI_T_BASE, AUI_EASE_OUT);

    unsigned track = aui_mix(AUI_TRACK, AUI_ACCENT, t);
    if (r.st & AUI_OFF) track = AUI_DISABLED;
    else if (r.st & AUI_HOVER) track = aui_shade(track, *state ? 14 : -8);
    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, h / 2);
    if (!glass_face(x, y, w, h, h / 2, track, GLASS_TRACK_OPACITY)) aui_round(x, y, w, h, h / 2, track);
    int kx = x + 2 + (w - h) * t / 255;        /* t=0 -> x+2; t=255 -> x+w-h+2 */
    aui_shadow_ex(kx, y + 2, h - 4, h - 4, (h - 4) / 2, 1, 3, 70);
    aui_circle(kx + (h - 4) / 2, y + h / 2, (h - 4) / 2, (r.st & AUI_OFF) ? AUI_DISABLED_TX : rgb(255, 255, 255));
    return changed;
}

int aui_slider(int x, int y, int w, int *value, int lo, int hi)
{
    int id = ++id_ctr, h = 22, kr = 9;
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll(id, wx - kr, wy, w + 2 * kr, h, 1, 1);
    if (hi <= lo) hi = lo + 1;
    int v = iclamp(*value, lo, hi), changed = 0;

    /* Drag: the press captured the widget, and motion goes to the capture
     * target, so this keeps tracking after the pointer leaves the groove. */
    int dragging = (in.active == id && in.down);
    if (r.clicked || (dragging && in.ev == EV_MOUSE_MOVE)) {
        int nv = lo + (in.mx - wx) * (hi - lo) / (w > 0 ? w : 1);
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; }
    }
    if ((r.st & AUI_FOCUSED) && in.ev == EV_KEY && !in.key_used) {
        int step = imax(1, (hi - lo) / 50), nv = v;
        if (in.a == KEY_LEFT || in.a == KEY_DOWN) nv = v - step;
        else if (in.a == KEY_RIGHT || in.a == KEY_UP) nv = v + step;
        else if (in.a == KEY_HOME) nv = lo;
        else if (in.a == KEY_END)  nv = hi;
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; in.key_used = 1; }
    }
    *value = v;

    /* Value motion is driven by what moved it. A dragged knob must sit under
     * the finger with zero lag -- an eased knob under a pointer reads as a
     * bug, not as polish -- so a drag or a click bypasses aui_anim entirely
     * and draws the raw position. An arrow key is a discrete event with no
     * finger to track, so THAT SAME KNOB animates over AUI_T_BASE. Because the
     * animated branch is only reached while not dragging, the slot goes stale
     * for the duration of any drag; the continuity rule then makes the very
     * next animated frame latch fresh at the raw position with zero jump --
     * which is what buys the drag its zero-cost exemption without a second
     * code path drawing the knob. */
    int fill_raw = (v - lo) * w / (hi - lo);
    int fill;
    if (dragging || r.clicked) {
        fill = fill_raw;
    } else {
        int target = (hi > lo) ? (v - lo) * 255 / (hi - lo) : 0;
        int t = aui_anim(AUI_AK_VALUE, target, AUI_T_BASE, AUI_EASE_OUT);
        fill = t * w / 255;
    }
    int ty = y + h / 2 - 2;
    aui_round(x, ty, w, 4, 2, AUI_TRACK);
    aui_round(x, ty, fill, 4, 2, AUI_ACCENT);
    int kx = x + fill;
    focus_ring(r.st & AUI_FOCUSED, kx - kr, y + h / 2 - kr, 2 * kr, 2 * kr, kr);
    aui_shadow_ex(kx - kr, y + h / 2 - kr, 2 * kr, 2 * kr, kr, 1, 4, 70);
    aui_circle(kx, y + h / 2, kr, rgb(255, 255, 255));
    if (r.st & (AUI_HOVER | AUI_ACTIVE)) aui_ring(kx, y + h / 2, kr, 2, AUI_ACCENT);
    else aui_ring(kx, y + h / 2, kr, 1, AUI_BORDER);
    return changed;
}

void aui_progress(int x, int y, int w, int pct)
{
    int h = 8;
    aui_round(x, y, w, h, h / 2, AUI_TRACK);
    if (pct >= 0) {
        int fw = iclamp(pct, 0, 100) * w / 100;
        aui_hgrad(x, y, fw, h, AUI_ACCENT, aui_shade(AUI_ACCENT, 30));
        aui_round(x, y, fw, h, h / 2, AUI_ACCENT);
    } else {
        /* Indeterminate: a bar that travels. Driven off the monotonic clock, so
         * it moves at the same speed whatever the repaint rate is -- except
         * that until aui_anim_loop() existed NOTHING SET THE REPAINT RATE. In
         * any app sleeping on wait_idle(0) this was a still picture, and the
         * only place in the tree where it moved was gallery.c, which
         * hand-rolled a 50 ms tick of its own. The wake is the fix; the phase
         * arithmetic below is unchanged. */
        int seg = w / 3;
        int t = (int)((aui_anim_loop() / 6) % (unsigned)(w + seg));
        int bx = x + t - seg;
        int x0 = imax(bx, x), x1 = imin(bx + seg, x + w);
        if (x1 > x0) aui_round(x0, y, x1 - x0, h, h / 2, AUI_ACCENT);
    }
}

void aui_spinner(int cx, int cy, int r)
{
    aui_ring(cx, cy, r, 2, AUI_TRACK);
    /* aui_anim_loop() rather than frame_ms: same value, plus the wake that
     * makes it advance in an app that is otherwise asleep. Read aui.h's price
     * for this before putting a spinner in a large window -- a loop repaints
     * the WHOLE canvas at AUI_ANIM_TICK_LOOP, forever. */
    unsigned t = aui_anim_loop();
    if (path_ok()) {
        /* A 100-degree round-capped arc on the track, one revolution per 900
         * ms. The arc is one path record whose box is the spinner's own, so
         * the frame's damage is that box and nothing else. */
        aui_arc(cx, cy, r - 1, (int)(t % 900) * 360 / 900 - 90, 100, 2, ap_argb(AUI_ACCENT, 255));
        return;
    }
    /* Eight fading dots around the ring: no trigonometry, a fixed unit circle
     * in eighths scaled by r (integer only, like everything else here). */
    static const int ux[8] = { 0, 181, 256, 181, 0, -181, -256, -181 };
    static const int uy[8] = { -256, -181, 0, 181, 256, 181, 0, -181 };
    unsigned phase = (t / 100) % 8;
    for (int i = 0; i < 8; i++) {
        int a = 40 + (int)((i + 8 - phase) % 8) * 27;
        aui_round_a(cx + ux[i] * r / 256 - 2, cy + uy[i] * r / 256 - 2, 4, 4, 2, AUI_ACCENT, a);
    }
}

void aui_badge(int x, int y, const char *s, unsigned color)
{
    int px = AUI_FS_CAPTION, pad = AUI_SP(2);
    int w = aui_text_w(s, px) + 2 * pad, h = px + AUI_SP(2) + 2;
    aui_round_a(x, y, w, h, h / 2, color, 46);
    aui_stroke(x, y, w, h, h / 2, 1, aui_mix(color, AUI_BG, 120));
    aui_text_in(aui_r(x, y, w, h), s, color, px, AUI_ALIGN_CENTER);
}

void aui_separator(int x, int y, int w) { aui_hairline(x, y, w); }
