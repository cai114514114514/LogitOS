/* aui_overlay.inc -- the menu panel (select and menu-bar popups, context menus),
 * the in-window menu bar, the popover, the sheet and the alert.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include.
 * Design 4.9 and 4.10 (ui-aui-overlays): the popup, the sheet and the new
 * context menu, popover and alert are written here; aui_menubar keeps its bar
 * and opens the same panel. */

/* ---- the menu panel (design 4.9) ---- */

#define OV_PAD      6       /* above the first and below the last row */
#define OV_ROW      24
#define OV_SEP      9
#define OV_R        12
#define OV_PILL_IN  5       /* hover pill inset, radius 7 */
#define OV_PILL_R   7
#define OV_BODY     14      /* body 14 / 400 */
#define OV_CALL     13      /* callout 13 */
#define OV_CTX_ID   0x3FFF0001   /* the pop.owner of a context menu */
#define OV_AID      0x3FFF0002   /* the animation slot of the open/close fade.
                                  * ONE constant id, queried every frame by draw_popup: a fresh
                                  * owner would latch instantly (no fade), and the same slot
                                  * across owners is what makes a menu-bar title swap fade-free. */

#define OVF_DESTRUCTIVE 1
#define OVF_DISABLED    2
#define OVF_SUBMENU     4

struct ov_it { char label[96]; char sc[24]; int flags, check, sep; };

static void ov_parse(const char *s, struct ov_it *it)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    it->flags = 0; it->check = 0; it->sep = 0; it->label[0] = 0; it->sc[0] = 0;
    if (p[0] == '-' && !p[1]) { it->sep = 1; return; }
    while (*p >= 1 && *p <= 3) { it->flags |= 1 << (*p - 1); p++; }
    if (p[0] == 0xE2 && p[1] == 0x9C && p[2] == 0x93) { it->check = 1; p += 3; if (*p == ' ') p++; }
    for (; *p && *p != '\t' && n < 95; p++) it->label[n++] = (char)*p;
    it->label[n] = 0;
    if (*p == '\t') { p++; n = 0; while (*p && n < 23) it->sc[n++] = (char)*p++; it->sc[n] = 0; }
}

/* Bytes of the leading modifier glyphs of a shortcut (⌘ ⌥ ⇧ ⌃). */
static int ov_sc_split(const char *sc)
{
    int i = 0;
    for (;;) {
        const unsigned char *q = (const unsigned char *)sc + i;
        if (q[0] == 0xE2 && ((q[1] == 0x8C && (q[2] == 0x98 || q[2] == 0xA5 || q[2] == 0x83)) ||
                             (q[1] == 0x87 && q[2] == 0xA7))) i += 3;
        else break;
    }
    return i;
}

static int ov_row_h(const char *s) { return (s[0] == '-' && !s[1]) ? OV_SEP : OV_ROW; }

int aui_menu_height(const char *const *items, int n)
{
    int h = 2 * OV_PAD;
    for (int i = 0; i < n; i++) h += ov_row_h(items[i]);
    return h;
}

/* The width the labels and shortcut column need, before the 220..360 clamp. */
static int ov_natural_w(const char *const *items, int n)
{
    struct ov_it it;
    int lw = 0, any = 0;
    for (int i = 0; i < n; i++) {
        int w;
        ov_parse(items[i], &it);
        if (it.sep) continue;
        w = aui_text_measure_w(it.label, OV_BODY, AUI_W_REGULAR);
        if (w > lw) lw = w;
        if (it.sc[0] || (it.flags & OVF_SUBMENU)) any = 1;
    }
    return 28 + lw + (any ? 16 + 44 : 0) + 14;
}

int aui_menu_width(const char *const *items, int n)
{
    return iclamp(ov_natural_w(items, n), 220, 360);
}

static unsigned ov_c(unsigned c)       /* a colour, mixed toward the plate when a fade is a legacy mix */
{ return fade_leg >= 255 ? c : aui_mix(aui_tok()->bg_raised, c, fade_leg); }

/* The damage reach of an elevation, points, for the opacity group around a panel. */
static struct aui_rect ov_group_rect(struct aui_rect rc, enum logit_elev el)
{
    /* 3 sigma is the damage reach (aui_elev widens the flush by it); the group
     * only has to hold the part of the shadow that is visible while it fades:
     * past 40 pt the tail of even E6's widest layer (sigma 30, peak alpha 41)
     * is under 3 % alpha, which fading cannot make visible. The snapshot of a
     * group costs w * h * 4 bytes of the kernel's per-window arena (8 bytes per
     * surface pixel), and the old sheet's margin was 40 too, so it is capped. */
    int g = (int)(logit_elevation_reach24(el) >> 8) + 2;
    if (g > 40) g = 40;
    int x0 = imax(0, rc.x - g), y0 = imax(0, rc.y - g);
    int x1 = rc.x + rc.w + g, y1 = rc.y + rc.h + g;
    if (win_w > 0) x1 = imin(win_w, x1);        /* (win_w is 0 until the app calls aui_set_size) */
    if (win_h > 0) y1 = imin(win_h, y1);
    return aui_r(x0, y0, imax(0, x1 - x0), imax(0, y1 - y0));    /* inside the window: the kernel refuses a group that is not */
}

/* The plate of a menu or popover: elevation, then the material, then its inner
 * 1-device-pixel edge. THE SNAPSHOT RULE: the material (one GLASS2, or the RT
 * solid) is issued only for a panel at rest (`settled`); while it animates the
 * plate is the 3.2 solid inside the caller's opacity group, so an animation
 * frame costs no blur. -DUI_OVERLAYS_NEGCTL re-materialises every frame, which
 * make test-ui-aui-overlays-negctl must report as 'FAIL menu-snapshot'. */
static void ov_plate(struct aui_rect rc, int r, enum logit_mat mat, enum logit_elev el, int settled)
{
    const struct logit_material *m = logit_material(mat);
    aui_elev(rc, r, el);
#ifdef UI_OVERLAYS_NEGCTL
    settled = 1;
#endif
    if (settled) aui_material(rc, r, mat);
    else aui_round(rc.x, rc.y, rc.w, rc.h, r, aui_tok()->bg_raised);
    aui_stroke_a(rc, r, 25600 / aui_scale(), theme_dark ? m->edge.argb_d : m->edge.argb_l);
}

/* The rows, in absolute window points. `hi` is the highlighted row (-1 none);
 * the pill sits at `pill_y` when >= 0 (it glides there), else at the row. */
static void ov_rows(int x, int y, int w, const char *const *items, int n, int hi, int pill_y)
{
    const struct logit_tokens *k = aui_tok();
    struct ov_it it;
    int iy = y + OV_PAD;
    if (hi >= 0 && hi < n) {
        int py = iy;
        for (int i = 0; i < hi; i++) py += ov_row_h(items[i]);
        aui_round_a(x + OV_PILL_IN, pill_y >= 0 ? pill_y : py, w - 2 * OV_PILL_IN, OV_ROW,
                    OV_PILL_R, k->accent, fade_leg);
    }
    for (int i = 0; i < n; i++) {
        int rh = ov_row_h(items[i]);
        unsigned ink, sub;
        ov_parse(items[i], &it);
        if (it.sep) {
            aui_hairline_dev(x + 12, iy + OV_SEP / 2, x + w - 12, iy + OV_SEP / 2, k->sep_hairline);
            iy += rh;
            continue;
        }
        if (it.flags & OVF_DISABLED) ink = sub = k->label_quaternary;
        else if (i == hi) ink = sub = k->label_on_accent;
        else { ink = (it.flags & OVF_DESTRUCTIVE) ? k->error_ink : k->label_primary; sub = k->label_tertiary; }
        ink = ov_c(ink); sub = ov_c(sub);
        if (it.check) gui_icon(GICON_CHECK, x + 10, iy + (OV_ROW - 12) / 2, 12, ink);
        aui_text_vc(aui_r(x + 28, iy, w - 28 - 14, OV_ROW), it.label, OV_BODY, AUI_W_REGULAR, ink, AUI_ALIGN_LEFT);
        if (it.sc[0]) {
            int split = ov_sc_split(it.sc), kw, kc;
            char mods[24];
            const char *key = it.sc + split;
            for (int j = 0; j < split; j++) mods[j] = it.sc[j];
            mods[split] = 0;
            kw = key[0] ? aui_text_measure_w(key, OV_CALL, AUI_W_REGULAR) : 0;
            kc = kw > 14 ? kw : 14;
            if (split) aui_text_vc(aui_r(x + w - 14 - kc - 30, iy, 30, OV_ROW), mods, OV_CALL, AUI_W_REGULAR, sub, AUI_ALIGN_RIGHT);
            if (key[0]) aui_text_vc(aui_r(x + w - 14 - kc, iy, kc, OV_ROW), key, OV_CALL, AUI_W_REGULAR, sub, AUI_ALIGN_LEFT);
        }
        if (it.flags & OVF_SUBMENU) gui_icon(GICON_CHEVRON_R, x + w - 14 - 10, iy + (OV_ROW - 10) / 2, 10, sub);
        iy += rh;
    }
}

void aui_menu_panel(int x, int y, int w, const char *const *items, int n, int hi)
{
    struct aui_rect rc = aui_r(X_(x), Y_(y), w, aui_menu_height(items, n));
    ov_plate(rc, OV_R, LOGIT_MAT_MENU, LOGIT_E3, 1);
    ov_rows(rc.x, rc.y, w, items, n, hi, -1);
}

/* The next enabled row from `from` in direction dir (clamped, no wrap). */
static int ov_step(const char *const *items, int n, int from, int dir)
{
    struct ov_it it;
    int i = from < 0 ? (dir > 0 ? -1 : n) : from;
    for (;;) {
        i += dir;
        if (i < 0 || i >= n) return from;
        ov_parse(items[i], &it);
        if (!it.sep && !(it.flags & OVF_DISABLED)) return i;
    }
}

static int ov_sel_y;            /* a select popup's panel y, fixed at open */
static int ov_ctx_sel;          /* the item a context menu returned */
static struct aui_rect ov_pov_rect;   /* the popover of this frame, for pop_prev (below) */
static unsigned ov_pov_gen;

static void draw_popup_menu(void)
{
    int open = pop.kind == 1;
    int saved_id = id_ctr, t, v, live0 = anim_live;
    int isctx = pop.owner == OV_CTX_ID, isbar = pop.owner < 0, issel = !isctx && !isbar;
    if (open && issel && pop_gen == anim_gen) {
        /* open over the selected item: its row centre lands on the control's */
        int off = OV_PAD + OV_ROW / 2, sel = pop.hi < 0 ? 0 : pop.hi;
        for (int i = 0; i < sel && i < pop.n; i++) off += ov_row_h(pop.items[i]);
        ov_sel_y = pop.y - 4 - AUI_H_CTL / 2 - off;
    }
    id_ctr = OV_AID;
    t = aui_anim(AUI_AK_EXTRA, open ? 255 : 0, open ? T_FADE_IN : T_FADE_OUT,
                 open ? AUI_EASE_STANDARD : AUI_EASE_IN_CUBIC);
    v = aui_spring_ex(AUI_AK_VALUE, open ? 256 : 0, AUI_SPRING_SMOOTH);
    id_ctr = saved_id;
    live0 = anim_live - live0;             /* frames that THIS panel's fade and spring still want */
    if (t <= 0 && !open) { pop_prev.w = 0; return; }   /* an opening panel is drawn (and blocks clicks) from its first frame, at opacity 0 */

    int h = aui_menu_height(pop.items, pop.n);
    int w = issel ? imax(pop.w, ov_natural_w(pop.items, pop.n)) : aui_menu_width(pop.items, pop.n);
    int x = pop.x, y = pop.y;
    if (issel) y = ov_sel_y;
    if (x + w > win_w - 2) x = win_w - w - 2;
    if (x < 2) x = 2;
    if (y + h > win_h - 2) y = win_h - h - 2;
    if (y < 2) y = 2;

    int settled = open && t >= 255 && v >= 256 && live0 == 0;
    int dy = -4 * (256 - iclamp(v, 0, 256)) / 256;
    struct aui_rect rc = aui_r(x, y + dy, w, h);

    if (open) {
        /* Input and dismissal only apply while genuinely open; a fading-out
         * ghost takes no input but keeps blocking the rect it still occupies. */
        pop_prev = aui_r(x, y, w, h);
        if (in.ev == EV_KEY) {
            if (in.a == KEY_DOWN)      pop.hi = ov_step(pop.items, pop.n, pop.hi, 1);
            else if (in.a == KEY_UP)   pop.hi = ov_step(pop.items, pop.n, pop.hi, -1);
            else if (in.a == '\n') {
                /* Nothing highlighted: Enter cancels (2026-09-16 audit). */
                if (pop.hi >= 0) { *pop.sel = pop.hi; pop_changed_id = pop.owner; }
                pop.kind = 0;
            }
            else if (in.a == 27)       pop.kind = 0;
            in.key_used = 1;
        }
    }

    int hit = -1, rowy = rc.y + OV_PAD, pill = -1;
    for (int i = 0; i < pop.n; i++) {
        struct ov_it it;
        int rh = ov_row_h(pop.items[i]);
        ov_parse(pop.items[i], &it);
        if (!it.sep && !(it.flags & OVF_DISABLED)) {
            int over = pop.kind == 1 && in.mx >= x && in.mx < x + w && in.my >= rowy && in.my < rowy + rh;
            if (over) { pop.hi = i; hit = i; }
            if (settled && pop.kind == 1)
                aim_fixed_i(isctx ? "context" : "popup", i, x + w / 2, rowy + rh / 2);
        }
        if (i == pop.hi && pop.kind == 1) pill = rowy;
        rowy += rh;
    }
    if (pill >= 0) {                       /* the pill glides between rows on snappy */
        id_ctr = OV_AID + 1;
        pill = aui_spring_ex(AUI_AK_VALUE, pill * 256, AUI_SPRING_SNAPPY) / 256;
        id_ctr = saved_id;
    }

    fade_open(ov_group_rect(rc, LOGIT_E3), t, 1);
    ov_plate(rc, OV_R, LOGIT_MAT_MENU, LOGIT_E3, settled);
    ov_rows(rc.x, rc.y, w, pop.items, pop.n, pop.kind == 1 ? pop.hi : -1, pill);
    fade_close();
    /* A click closes it -- except the click that OPENED it this very frame
     * (aui_select / aui_menubar open the popup on the press). A click on a
     * separator, a disabled row or the padding leaves it open. */
    if (pop.kind == 1 && in.ev == EV_MOUSE && pop_gen != anim_gen) {
        int inside = in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h;
        if (hit >= 0) { *pop.sel = hit; pop_changed_id = pop.owner; pop.kind = 0; }
        else if (!inside) pop.kind = 0;
    }
}

/* The popover is not a pop.* popup, but widgets drawn before it must refuse
 * clicks in its rect like they do for a menu: pop_prev carries it. */
static void draw_popup(void)
{
    draw_popup_menu();
    if (ov_pov_gen == anim_gen && pop_prev.w <= 0) pop_prev = ov_pov_rect;
}

int aui_context_menu(int x, int y, const char *const *items, int n)
{
    if (x >= 0 && n > 0 && pop.kind == 0) {
        pop.kind = 1; pop.owner = OV_CTX_ID; pop.x = X_(x); pop.y = Y_(y);
        pop_gen = anim_gen;
        pop.w = 0; pop.itemh = OV_ROW; pop.n = n;
        pop.items = items; pop.sel = &ov_ctx_sel; pop.hi = -1;
    }
    if (pop_changed_id == OV_CTX_ID) { pop_changed_id = 0; return ov_ctx_sel; }
    return -1;
}

int aui_menubar(int x, int y, int w, const char *const *titles,
                const char *const *const *items, int n, int *mi, int *ii)
{
    static int open_menu = -1;
    const struct logit_tokens *k = aui_tok();
    int h = 28, chosen = 0;
    aui_fill(x, y, w, h, k->bg_toolbar);
    aui_hairline(x, y + h - 1, w);
    int cx = x + AUI_SP(2);
    for (int i = 0; i < n; i++) {
        int id = ++id_ctr;
        int tw_ = tw(titles[i]) + AUI_SP(4);
        struct wres r = wpoll(id, X_(cx), Y_(y), tw_, h - 1, 1, 0);
        if (r.st & AUI_HOVER && open_menu >= 0 && open_menu != i) open_menu = i;
        if (r.clicked) open_menu = (open_menu == i) ? -1 : i;
        if (open_menu == i) aui_round(cx, y + 2, tw_, h - 5, OV_PILL_R, k->accent);
        else if (r.st & AUI_HOVER) aui_round_a(cx, y + 2, tw_, h - 5, OV_PILL_R, k->label_primary, 16);
        aui_text_in(aui_r(cx, y, tw_, h - 1), titles[i],
                    open_menu == i ? k->label_on_accent : k->label_primary, AUI_FS_LABEL, AUI_ALIGN_CENTER);
        if (open_menu == i && pop.kind == 0) {
            int cnt = 0; while (items[i][cnt]) cnt++;
            pop.kind = 1; pop.owner = -2 - i; pop.x = X_(cx); pop.y = Y_(y) + h;
            pop_gen = anim_gen;
            pop.w = 0; pop.itemh = OV_ROW; pop.n = cnt;
            pop.items = items[i]; pop.sel = ii; pop.hi = -1;
        }
        cx += tw_;
    }
    if (pop_changed_id <= -2) { *mi = -pop_changed_id - 2; pop_changed_id = 0; open_menu = -1; chosen = 1; }
    if (pop.kind == 0 && open_menu >= 0 && in.ev == EV_MOUSE) open_menu = -1;
    return chosen;
}

/* ---- the rim of an opaque raised surface (design 3.2, sheets and alerts) ----
 * Dark: a 1-device-pixel INNER stroke, LINEAR #FFFFFF @ 38 at the top, @ 10 at
 * 40 % of the height, @ 18 at the bottom. Light: a 1-device-pixel OUTER stroke
 * of #141428 @ 20. A kernel without PATH gets the flat one-point stroke. */
static void ov_rim(struct aui_rect rc, int r)
{
    int dark = theme_dark, w256 = 25600 / aui_scale();
    if (rc.w <= 0 || rc.h <= 0) return;
    r = clamp_radius(rc.w, rc.h, r);
    if (!path_ok()) { aui_stroke_a(rc, r, w256, dark ? 0x1AFFFFFFu : 0x14141428u); return; }
    {
        struct ap p;
        struct logit_path2 pa;
        struct logit_paint2 pt;
        struct logit_stroke2 st;
        unsigned pos[3] = { 0, 26214, 65536 };
        unsigned col[3] = { 0x26FFFFFFu, 0x0AFFFFFFu, 0x12FFFFFFu };
        long long rx[4], ry[4];
        int X = X_(rc.x), Y = Y_(rc.y), sg = dark ? 1 : -1;
        long long hw = ap_fdiv((long long)w256 * aui_scale(), 100) / 2;
        long long x0 = (long long)aui_dev(X) * 256 + sg * hw, x1 = (long long)aui_dev(X + rc.w) * 256 - sg * hw;
        long long y0 = (long long)aui_dev(Y) * 256 + sg * hw, y1 = (long long)aui_dev(Y + rc.h) * 256 - sg * hw;
        int ax[4] = { X, X + rc.w - r, X + rc.w - r, X };
        int ay[4] = { Y, Y, Y + rc.h - r, Y + rc.h - r };
        for (int i = 0; i < 4; i++) {
            rx[i] = (long long)devlen(ax[i], r) * 256 - sg * hw;
            ry[i] = (long long)devlen(ay[i], r) * 256 - sg * hw;
            if (rx[i] < 0 || ry[i] < 0) rx[i] = ry[i] = 0;
        }
        ap_begin(&p, 1);
        ap_rrect(&p, x0, y0, x1, y1, rx, ry);
        gfx_zero(&pa, sizeof pa); gfx_zero(&pt, sizeof pt); gfx_zero(&st, sizeof st);
        pa.verbs = p.v; pa.xy = p.xy; pa.nverb = (unsigned)p.nv; pa.nxy = (unsigned)p.nxy;
        pa.rule = LOGIT_PATH2_NONZERO;
        if (dark) {
            pt.kind = LOGIT_PAINT2_LINEAR;
            pt.x0 = X * 256; pt.y0 = Y * 256; pt.x1 = X * 256; pt.y1 = (Y + rc.h) * 256;
            pt.nstop = 3; pt.stop_pos = pos; pt.stop_argb = col;
        } else { pt.kind = LOGIT_PAINT2_SOLID; pt.argb = 0x14141428u; }
        st.width = w256; st.cap = LOGIT_STROKE2_CAP_BUTT; st.join = LOGIT_STROKE2_JOIN_MITER; st.miter = 4 * 65536;
        ad_note_path(X, Y, rc.w, rc.h, 2, p.v, p.nv, p.xy, p.nxy, (dark ? 0x4C000026u : 0x14141428u) ^ (unsigned)w256);
        ol_window_path2(&pa, &pt, &st);
    }
}

/* The scrim over the owner window: black @ 46 (light) / @ 89 (dark) at e = 256. */
static void ov_scrim(int e)
{ aui_fill_a(0, 0, win_w, win_h, 0x000000, (theme_dark ? 89 : 46) * e / 256); }

/* ---- the sheet (design 4.10) ----
 *
 * THE ONE PLACE THIS FILE DOES NOT ROUTE THROUGH aui_anim()'s SLOT TABLE, and
 * it earns the exception rather than assuming it. aui_dialog_begin() and
 * aui_alert() are only called by the app WHILE THE DIALOG IS OPEN -- unlike a
 * hover fade or the popup above (which run every frame via aui_end()), there is
 * no counterpart that keeps polling during the closed frames. So the slot's
 * continuity rule -- "not queried on the immediately preceding frame means
 * fresh, latch instantly" -- fires on the very first frame of every single
 * open, and the entrance would silently never animate.
 *
 * The fix is the SAME continuity test the slot table runs, done by hand
 * against the same frame counter (`anim_gen`), with the design's curves: the
 * fade is STANDARD over t.fadeIn and the rise is an OUT_QUART over the 230 ms
 * `smooth` settles in. A dialog held open across frames re-observes its own t0
 * and does not restart; closing and reopening (a frame gap, since this
 * function was not called in between) is a fresh entrance. Close itself stays
 * instant: nothing calls the dialog on the frames in which it would fade.
 * (The design's 0.96 -> 1 scale is not drawable: the GUI ABI has no transform,
 * so the entrance is the opacity fade plus the rise.) */
static struct { int x, y, w, h; } dlg;
static unsigned dlg_last_gen;   /* anim_gen this ran at, last time; 0 = never */
static unsigned dlg_t0;         /* frame_ms when the CURRENT entrance began   */
static int dlg_fade;            /* aui_dialog_begin opened a fade level that aui_dialog_end closes */
static int dlg_settled;         /* the entrance has finished: the buttons publish aims */

/* One entrance step: e = opacity 0..256, es = position 0..256; marks the
 * frame as live while either is moving. Returns 1 when settled. */
static int ov_entrance(unsigned *last_gen, unsigned *t0, int *e, int *es)
{
    unsigned el;
    if (!*last_gen || *last_gen + 1 != anim_gen) *t0 = frame_ms;   /* a fresh open */
    *last_gen = anim_gen;
    el = frame_ms - *t0;
    *e  = anim_reduced || (int)el >= T_FADE_IN ? 256 : aui_ease_eval(AUI_EASE_STANDARD, (int)(el * 256u / T_FADE_IN));
    *es = anim_reduced || (int)el >= 230 ? 256 : aui_ease_eval(AUI_EASE_OUT_QUART, (int)(el * 256u / 230u));
    if (*e < 256 || *es < 256) anim_live++;      /* keep the wake contract awake */
    return *e >= 256 && *es >= 256;
}

int aui_dialog_begin(const char *title, int w, int h)
{
    const struct logit_tokens *k = aui_tok();
    int th = 40, e, es, rise;
    int x = (win_w - w) / 2, y = (win_h - h - th) / 3;
    struct aui_rect rc;
    dlg_open_now = 1;
    if (y < 12) y = 12;
    dlg.x = x; dlg.y = y + th; dlg.w = w; dlg.h = h;
    dlg_settled = ov_entrance(&dlg_last_gen, &dlg_t0, &e, &es);
    rise = 12 * (256 - es) / 256;
    rc = aui_r(x, y + rise, w, h + th);

    ov_scrim(e);
    /* The sheet (shadow, plate, rim, title and, until aui_dialog_end, its
     * content) fades in as one group. Without groups it only rises. */
    fade_open(ov_group_rect(rc, LOGIT_E6), 255 * e / 256, 0);
    dlg_fade = 1;
    aui_elev(rc, 18, LOGIT_E6);
    aui_squircle(rc, 18, 0xFF000000u | k->bg_raised);
    ov_rim(rc, 18);
    aui_text_vc(aui_r(x + 20, y + rise + 10, w - 40, 24), title, 17, AUI_W_SEMIBOLD, k->label_primary, AUI_ALIGN_LEFT);
    in_dialog = 1;
    clip_push(aui_r(x, y + rise + th, w, h));
    ox_ = x; oy_ = y + rise + th;
    return 1;
}

void aui_dialog_end(void)
{
    ox_ = 0; oy_ = 0;
    clip_pop();
    if (dlg_fade) { dlg_fade = 0; fade_close(); }
    in_dialog = 0;
}

int aui_dialog_buttons(const char *const *labels, int n)
{
    int bh = AUI_H_CTL, gap = 8, pressed = -1;
    int y = dlg.h - bh - 20;
    int x = dlg.w - 20;
    for (int i = 0; i < n; i++) {            /* labels[0] is the rightmost */
        int bw = imax(72, tw(labels[i]) + 32);
        x -= bw;
        if (aui_button_ex(x, y, bw, bh, labels[i],
                          i == 0 ? AUI_V_PRIMARY : AUI_V_SECONDARY, 1)) pressed = i;
        /* The sheet's aims: every button by index, Enter's and Escape's by
         * role. Only inside a settled dialog. */
        if (in_dialog && dlg_settled) {
            int ax = X_(x + bw / 2), ay = Y_(y + bh / 2);
            aim_fixed_i("dialog", i, ax, ay);
            if (i == 0) aim_fixed("dialog-confirm", ax, ay);
            if (i == n - 1 && n > 1) aim_fixed("dialog-cancel", ax, ay);
        }
        x -= gap;
    }
    if (in.ev == EV_KEY && !in.key_used) {
        if (in.a == '\n') { pressed = 0; in.key_used = 1; }
        else if (in.a == 27) { pressed = n - 1; in.key_used = 1; }
    }
    return pressed;
}

/* ---- the alert (design 4.10) ---- */

/* A capsule button drawn from the tokens: kind 0 Secondary, 1 Primary,
 * 2 Destructive. Returns 1 on a press. */
static int ov_button(int x, int y, int w, int h, const char *label, int kind)
{
    const struct logit_tokens *k = aui_tok();
    int id = ++id_ctr;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, 1, 1);
    int hv = aui_anim(AUI_AK_FACE, (r.st & AUI_HOVER) ? 255 : 0, T_HOVER_IN, AUI_EASE_LINEAR);
    unsigned base = kind == 1 ? k->accent : kind == 2 ? k->destructive_fill : k->fill_control;
    unsigned top = kind == 1 ? k->accent_hover : kind == 2 ? aui_shade(k->destructive_fill, -14)
                 : aui_mix(k->fill_control, k->label_primary, 22);
    unsigned fill = aui_mix(base, top, hv);
    unsigned ink = kind == 0 ? k->label_primary : kind == 1 ? k->label_on_accent : 0xFFFFFFu;
    if (r.st & AUI_ACTIVE) fill = kind == 1 ? k->accent_pressed : aui_shade(fill, -22);
    if (kind == 0) aui_elev(aui_r(x, y, w, h), h / 2, LOGIT_E1);
    aui_round(x, y, w, h, h / 2, fill);
    if (focus_id == id && focus_vis)
        aui_stroke_a(aui_r(x - 2, y - 2, w + 4, h + 4), h / 2 + 2, 2 * 256, k->accent_focus_ring);
    aui_text_vc(aui_r(x, y, w, h), label, OV_BODY, AUI_W_MEDIUM, ink, AUI_ALIGN_CENTER);
    return r.clicked;
}

static unsigned alert_last_gen, alert_t0;

int aui_alert(int icon, const char *title, const char *message,
              const char *const *buttons, int n, unsigned flags)
{
    const struct logit_tokens *k = aui_tok();
    const int W = 300, pad = 20, cw = 260, bh = 32, bgap = 8;
    int mh, ih = icon ? 48 : 0, stacked = n >= 3, btnh, H, x, y, e, es, rise, settled, ty, pressed = -1;
    int def = (flags & AUI_ALERT_DESTRUCTIVE) && n > 1 ? n - 1 : 0;
    struct aui_rect rc;
    if (n < 1) return -1;
    dlg_open_now = 1;
    mh = (message && message[0]) ? aui_text_measure_wrap_lh(cw, message, -1, OV_CALL, AUI_W_REGULAR, 18, 4) : 0;
    btnh = stacked ? n * bh + (n - 1) * bgap : bh;
    H = pad + (ih ? ih + 12 : 0) + 22 + (mh ? 6 + mh : 0) + 20 + btnh + pad;
    x = (win_w - W) / 2; y = (win_h - H) * 2 / 5;
    if (y < 12) y = 12;
    settled = ov_entrance(&alert_last_gen, &alert_t0, &e, &es);
    rise = 8 * (256 - es) / 256;
    rc = aui_r(x, y + rise, W, H);

    ov_scrim(e);
    fade_open(ov_group_rect(rc, LOGIT_E6), 255 * e / 256, 0);
    aui_elev(rc, 18, LOGIT_E6);
    aui_squircle(rc, 18, 0xFF000000u | k->bg_raised);
    ov_rim(rc, 18);
    ty = rc.y + pad;
    if (ih) {
        unsigned ink = (flags & AUI_ALERT_KIND) == AUI_ALERT_WARNING ? k->warning_ink
                     : (flags & AUI_ALERT_KIND) == AUI_ALERT_ERROR ? k->error_ink
                     : (flags & AUI_ALERT_KIND) == AUI_ALERT_SUCCESS ? k->success_ink : k->accent;
        aui_circle(x + W / 2, ty + 24, 24, aui_mix(k->bg_raised, ink, 40));
        gui_icon(icon, x + W / 2 - 14, ty + 24 - 14, 28, ink);
        ty += ih + 12;
    }
    aui_text_vc(aui_r(x + pad, ty, cw, 22), title, 17, AUI_W_SEMIBOLD, k->label_primary, AUI_ALIGN_CENTER);
    ty += 22;
    if (mh) {
        ty += 6;
        aui_text_wrap_lh(aui_r(x + pad, ty, cw, mh), message, -1, OV_CALL, AUI_W_REGULAR,
                         0xFF000000u | k->label_secondary, 18, 4, AUI_ALIGN_CENTER, AUI_TRUNC_TAIL);
        ty += mh;
    }
    ty += 20;
    in_dialog = 1;
    for (int i = 0; i < n; i++) {
        int bx, by, bw, kind = i == 0 ? ((flags & AUI_ALERT_DESTRUCTIVE) ? 2 : 1) : 0;
        if (stacked) { bx = x + pad; by = ty + i * (bh + bgap); bw = cw; }
        else { bw = (cw - bgap) / 2; bx = x + pad + (n - 1 - i) * (bw + bgap); by = ty; }
        if (n == 1) { bw = cw; bx = x + pad; }
        if (ov_button(bx, by, bw, bh, buttons[i], kind)) pressed = i;
        if (settled) {
            aim_fixed_i("alert", i, bx + bw / 2, by + bh / 2);
            if (i == 0) aim_fixed("alert-confirm", bx + bw / 2, by + bh / 2);
            if (i == n - 1 && n > 1) aim_fixed("alert-cancel", bx + bw / 2, by + bh / 2);
        }
    }
    in_dialog = 0;
    fade_close();
    if (in.ev == EV_KEY && !in.key_used) {
        if (in.a == '\n') { pressed = def; in.key_used = 1; }
        else if (in.a == 27) { pressed = n - 1; in.key_used = 1; }
    }
    return pressed;
}

/* ---- the popover (design 4.9) ---- */

static unsigned pov_last_gen, pov_t0;
static int pov_fade, pov_dismissed;

/* Window points (24.8) to device 24.8, the way the tile path scales. */
static long long ov_dev256(long long pt256) { return pt256 * aui_scale() / 100; }

int aui_popover_begin(struct aui_rect anchor, int w, int h, int edge)
{
    const struct logit_tokens *k = aui_tok();
    const struct logit_material *m = logit_material(LOGIT_MAT_POPOVER);
    struct aui_rect an = aui_r(X_(anchor.x), Y_(anchor.y), anchor.w, anchor.h), rc;
    int e = edge, px = 0, py = 0, vert, e1, es, settled, fresh, tries, ax, ay, ox, oy;
    for (tries = 0; tries < 2; tries++) {
        int fits;
        switch (e) {
        case AUI_EDGE_TOP:    px = an.x + an.w / 2 - w / 2; py = an.y - 6 - 8 - h; fits = py >= 4; break;
        case AUI_EDGE_LEFT:   px = an.x - 6 - 8 - w; py = an.y + an.h / 2 - h / 2; fits = px >= 4; break;
        case AUI_EDGE_RIGHT:  px = an.x + an.w + 6 + 8; py = an.y + an.h / 2 - h / 2; fits = px + w <= win_w - 4; break;
        default: e = AUI_EDGE_BOTTOM;
                              px = an.x + an.w / 2 - w / 2; py = an.y + an.h + 6 + 8; fits = py + h <= win_h - 4; break;
        }
        if (fits || tries) break;
        e = e == AUI_EDGE_TOP ? AUI_EDGE_BOTTOM : e == AUI_EDGE_BOTTOM ? AUI_EDGE_TOP
          : e == AUI_EDGE_LEFT ? AUI_EDGE_RIGHT : AUI_EDGE_LEFT;
    }
    vert = e == AUI_EDGE_TOP || e == AUI_EDGE_BOTTOM;
    px = imax(8, imin(px, win_w - w - 8));
    py = imax(8, imin(py, win_h - h - 8));
    ax = iclamp(an.x + an.w / 2, px + 22, px + w - 22);
    ay = iclamp(an.y + an.h / 2, py + 22, py + h - 22);

    fresh = !pov_last_gen || pov_last_gen + 1 != anim_gen;
    settled = ov_entrance(&pov_last_gen, &pov_t0, &e1, &es);
    /* the 4 pt settle toward the final place, from the anchor's side */
    ox = oy = 0;
    switch (e) {
    case AUI_EDGE_BOTTOM: oy = -4 * (256 - es) / 256; break;
    case AUI_EDGE_TOP:    oy =  4 * (256 - es) / 256; break;
    case AUI_EDGE_RIGHT:  ox = -4 * (256 - es) / 256; break;
    default:              ox =  4 * (256 - es) / 256; break;
    }
    rc = aui_r(px + ox, py + oy, w, h);

    /* dismissal: an outside click (the anchor excepted) or Escape, never on the
     * frame that opened it (that frame still carries the opening click) */
    if (!fresh) {
        if (in.ev == EV_MOUSE && !aui_hit(aui_r(px, py, w, h), in.mx, in.my) && !aui_hit(an, in.mx, in.my))
            pov_dismissed = 1;
        if (in.ev == EV_KEY && in.a == 27 && !in.key_used) { pov_dismissed = 1; in.key_used = 1; }
    }
    ov_pov_rect = aui_r(px, py, w, h); ov_pov_gen = anim_gen;

    fade_open(ov_group_rect(rc, LOGIT_E3), 255 * e1 / 256, 1);
    pov_fade = 1;
    ov_plate(rc, 14, LOGIT_MAT_POPOVER, LOGIT_E3, settled);
    {   /* the arrow, 16 x 8 with a rounded tip, pointing at the anchor */
        struct ap p;
        static const int pu[6] = { -8, -2, -1, 1, 2, 8 };    /* along the edge, points */
        static const int pv[6] = { -10, 62, 75, 75, 62, -10 }; /* out of the panel, tenths of a point */
        unsigned argb = settled ? (theme_dark ? m->tint_d : m->tint_l) : (0xFF000000u | k->bg_raised);
        int bx = vert ? ax + ox - 8 : (e == AUI_EDGE_RIGHT ? px + ox - 8 : px + ox + w);
        int by = vert ? (e == AUI_EDGE_BOTTOM ? py + oy - 8 : py + oy + h) : ay + oy - 8;
        ap_begin(&p, 1);
        for (int i = 0; i < 6; i++) {
            long long u = (long long)pu[i] * 256, v = (long long)pv[i] * 256 / 10, X, Y;
            switch (e) {
            case AUI_EDGE_BOTTOM: X = (long long)(ax + ox) * 256 + u; Y = (long long)(py + oy) * 256 - v; break;
            case AUI_EDGE_TOP:    X = (long long)(ax + ox) * 256 + u; Y = (long long)(py + oy + h) * 256 + v; break;
            case AUI_EDGE_RIGHT:  X = (long long)(px + ox) * 256 - v; Y = (long long)(ay + oy) * 256 + u; break;
            default:              X = (long long)(px + ox + w) * 256 + v; Y = (long long)(ay + oy) * 256 + u; break;
            }
            if (i == 0) ap_m(&p, ov_dev256(X), ov_dev256(Y)); else ap_l(&p, ov_dev256(X), ov_dev256(Y));
        }
        ap_z(&p);
        path_emit(&p, argb, 0, 0, 0, bx, by, vert ? 16 : 8, vert ? 8 : 16, 1);
    }
    in_popup = 1;
    clip_push(aui_r(px + 16 + ox, py + 16 + oy, w - 32, h - 32));
    ox_ = px + 16 + ox; oy_ = py + 16 + oy;
    return 1;
}

void aui_popover_end(void)
{
    ox_ = 0; oy_ = 0;
    clip_pop();
    if (pov_fade) { pov_fade = 0; fade_close(); }
    in_popup = 0;
}

int aui_popover_dismissed(void)
{
    int d = pov_dismissed;
    pov_dismissed = 0;
    return d;
}
