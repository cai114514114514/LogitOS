/* aui_overlay.inc -- the deferred popup (select and menu lists), the in-window
 * menu bar and the modal dialog.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

/* ---- popup, menu bar ---- */

static void draw_popup(void)
{
    /* Fade in on open, alpha only -- no translate and no scale. Scale was
     * considered and rejected (see the design dossier): it would re-key every
     * rounded corner in the popup on every frame of the fade, which misses
     * the gfx mask cache by construction. Queried every frame regardless of
     * pop.kind, exactly like draw_tip(), so a dismiss has something continuous
     * to fade out from instead of vanishing. pop.owner is negative for a
     * menu-bar submenu (aui_menubar's `-2 - i` encoding) and aui_anim's id<=0
     * guard answers those instantly and on purpose: the menu bar is pure
     * glass, and animating anything that touches it is the single most
     * expensive mistake on this machine (dmg_expand grows the damage to the
     * whole panel -- see aui.h). */
    int saved_id = id_ctr;
    id_ctr = pop.owner;
    int t = aui_anim(AUI_AK_EXTRA, pop.kind == 1 ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    id_ctr = saved_id;
    if (t <= 0) { pop_prev.w = 0; return; }

    int h = pop.n * pop.itemh + AUI_SP(2);
    int x = pop.x, y = pop.y;
    if (y + h > win_h) y = imax(2, win_h - h - 2);

    if (pop.kind == 1) {
        /* Input and dismissal only apply while genuinely open; a fading-out
         * ghost takes no input but keeps blocking the rect it still occupies
         * on screen (pop_prev is simply left unrefreshed during the fade, so
         * it holds the last real geometry until t reaches 0 above). */
        pop_prev = aui_r(x, y, pop.w, h);
        if (in.ev == EV_KEY) {
            if (in.a == KEY_DOWN)      pop.hi = iclamp(pop.hi + 1, 0, pop.n - 1);
            else if (in.a == KEY_UP)   pop.hi = iclamp(pop.hi - 1, 0, pop.n - 1);
            else if (in.a == '\n')     {
                /* Nothing highlighted yet: Enter cancels. Writing pop.hi (-1)
                 * through pop.sel handed the caller an out-of-range index
                 * (2026-09-16 audit); the iclamp paths above never do. */
                if (pop.hi >= 0) { *pop.sel = pop.hi; pop_changed_id = pop.owner; }
                pop.kind = 0;
            }
            else if (in.a == 27)       pop.kind = 0;     /* Escape: see the note in aui.h */
            in.key_used = 1;
        }
    }

    /* One opacity group when the kernel has them; otherwise the same faux-alpha
     * mix as draw_tip (fade_leg = t), since aui_stroke has no alpha parameter. */
    int hl[4];
    ad_halo_shadow(0, 3, 10, 0, hl);
    fade_open(aui_r(x - hl[0], y - hl[1], pop.w + hl[0] + hl[2], h + hl[1] + hl[3]), t, 1);
    t = fade_leg;
    aui_shadow_ex(x, y, pop.w, h, AUI_R_MD, 3, 10, (aui_is_dark() ? 120 : 55) * t / 255);
    aui_round_a(x, y, pop.w, h, AUI_R_MD, AUI_SURFACE_2, t);
    aui_stroke(x, y, pop.w, h, AUI_R_MD, 1, aui_mix(AUI_SURFACE_2, AUI_BORDER, t));
    int hit = -1;
    for (int i = 0; i < pop.n; i++) {
        int iy = y + AUI_SP(1) + i * pop.itemh;
        int over = pop.kind == 1 &&
                   in.mx >= x && in.mx < x + pop.w && in.my >= iy && in.my < iy + pop.itemh;
        if (over) { pop.hi = i; hit = i; }
        if (i == pop.hi) aui_round_a(x + 3, iy, pop.w - 6, pop.itemh, AUI_R_SM, AUI_ACCENT, t);
        aui_text_ellipsis(x + AUI_SP(2), iy + (pop.itemh - PX) / 2 - 1, pop.w - AUI_SP(5),
                          pop.items[i], aui_mix(AUI_SURFACE_2, i == pop.hi ? AUI_ACCENT_TEXT : AUI_TEXT, t), PX);
    }
    fade_close();
    if (pop.kind == 1 && in.ev == EV_MOUSE) {
        if (hit >= 0) { *pop.sel = hit; pop_changed_id = pop.owner; }
        pop.kind = 0;                       /* a click anywhere closes it */
    }
}

int aui_menubar(int x, int y, int w, const char *const *titles,
                const char *const *const *items, int n, int *mi, int *ii)
{
    static int open_menu = -1;
    int h = 28, chosen = 0;
    aui_fill(x, y, w, h, aui_mix(AUI_BG, AUI_SURFACE, 160));
    aui_hairline(x, y + h - 1, w);
    int cx = x + AUI_SP(2);
    for (int i = 0; i < n; i++) {
        int id = ++id_ctr;
        int tw_ = tw(titles[i]) + AUI_SP(4);
        struct wres r = wpoll(id, X_(cx), Y_(y), tw_, h - 1, 1, 0);
        if (r.st & AUI_HOVER && open_menu >= 0 && open_menu != i) open_menu = i;
        if (r.clicked) open_menu = (open_menu == i) ? -1 : i;
        if (open_menu == i) aui_round(cx, y + 2, tw_, h - 5, AUI_R_SM, AUI_ACCENT);
        else if (r.st & AUI_HOVER) aui_round_a(cx, y + 2, tw_, h - 5, AUI_R_SM, AUI_TEXT, 16);
        aui_text_in(aui_r(cx, y, tw_, h - 1), titles[i],
                    open_menu == i ? AUI_ACCENT_TEXT : AUI_TEXT, AUI_FS_LABEL, AUI_ALIGN_CENTER);
        if (open_menu == i && pop.kind == 0) {
            int cnt = 0; while (items[i][cnt]) cnt++;
            pop.kind = 1; pop.owner = -2 - i; pop.x = X_(cx); pop.y = Y_(y) + h;
            pop.w = 160; pop.itemh = ROWH; pop.n = cnt;
            pop.items = items[i]; pop.sel = ii; pop.hi = -1;
        }
        cx += tw_;
    }
    if (pop_changed_id <= -2) { *mi = -pop_changed_id - 2; pop_changed_id = 0; open_menu = -1; chosen = 1; }
    if (pop.kind == 0 && open_menu >= 0 && in.ev == EV_MOUSE) open_menu = -1;
    return chosen;
}

/* ---- dialog ---- */

static struct { int x, y, w, h; } dlg;

/* THE ONE PLACE THIS FILE DOES NOT ROUTE THROUGH aui_anim()'s SLOT TABLE, and
 * it earns the exception rather than assuming it. aui_dialog_begin() is only
 * called by the app WHILE THE DIALOG IS OPEN -- unlike a hover fade or the
 * tooltip/popup above (which run every frame via aui_end() regardless of
 * their own visibility), there is no counterpart here that keeps polling
 * during the closed frames. So the slot's continuity rule -- "not queried on
 * the immediately preceding frame means fresh, latch instantly" -- fires on
 * the very first frame of every single open, and the entrance would silently
 * never animate. That is exactly the class of thing rule 5 (a control that
 * cannot be watched failing is worse than no control) warns about, so it is
 * written down here instead of hidden behind a plausible-looking aui_anim()
 * call that would always return 255.
 *
 * The fix is the SAME continuity test the slot table runs, done by hand
 * against the same frame counter (`anim_gen`, a static in this file), and it
 * reuses the shared curve and duration -- gfx_ease_out(), AUI_T_SLOW -- rather
 * than inventing a second clock. A dialog held open across frames re-observes
 * its own t0 and does not restart; closing and reopening (a frame gap, by
 * definition, since this function was not called in between) is a fresh
 * entrance, correctly. Close itself stays instant -- see aui.h / the design
 * dossier's DISMISS rule -- so there is nothing to animate on the way out. */
static unsigned dlg_last_gen;   /* anim_gen this ran at, last time; 0 = never */
static unsigned dlg_t0;         /* frame_ms when the CURRENT entrance began   */
static int dlg_fade;            /* aui_dialog_begin opened a fade level that aui_dialog_end closes */

int aui_dialog_begin(const char *title, int w, int h)
{
    dlg_open_now = 1;
    int th = 40;
    int x = (win_w - w) / 2, y = (win_h - h - th) / 3;
    if (y < 12) y = 12;
    dlg.x = x; dlg.y = y + th; dlg.w = w; dlg.h = h;

    if (!dlg_last_gen || dlg_last_gen + 1 != anim_gen) dlg_t0 = frame_ms;   /* a fresh open */
    dlg_last_gen = anim_gen;
    unsigned el = frame_ms - dlg_t0;
    int e = anim_reduced || (int)el >= AUI_T_SLOW ? 256 : ol_ease256(OL_EASE_OUT,(int)(el * 256u / AUI_T_SLOW));
    if (e < 256) anim_live++;      /* keep the wake contract awake for the rest of the entrance */
    int scrim_a = 110 * e / 256;
    int rise = 12 * (256 - e) / 256;

    aui_fill_a(0, 0, win_w, win_h, AUI_SCRIM, scrim_a);       /* the scrim */
    /* The sheet (shadow, panel, title and, until aui_dialog_end, its content)
     * fades in as one group. Without groups it only rises, as before. */
    fade_open(aui_r(x - 40, y + rise - 40, w + 80, h + th + 80), 255 * e / 256, 0);
    dlg_fade = 1;
    aui_shadow(x, y + rise, w, h + th, AUI_R_XL, AUI_ELEV_3);
    aui_round(x, y + rise, w, h + th, AUI_R_XL, AUI_SURFACE_2);
    aui_stroke(x, y + rise, w, h + th, AUI_R_XL, 1, AUI_BORDER);
    aui_text_in(aui_r(x, y + rise, w, th), title, AUI_TEXT, AUI_FS_TITLE, AUI_ALIGN_CENTER);
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
    int bh = AUI_H_CTL + 4, gap = AUI_SP(2), pressed = -1;
    int y = dlg.h - bh - AUI_SP(4);
    int x = dlg.w - AUI_SP(4);
    for (int i = n - 1; i >= 0; i--) {
        int bw = imax(84, tw(labels[i]) + AUI_SP(8));
        x -= bw;
        if (aui_button_ex(x, y, bw, bh, labels[i],
                          i == 0 ? AUI_V_PRIMARY : AUI_V_SECONDARY, 1)) pressed = i;
        x -= gap;
    }
    if (in.ev == EV_KEY && !in.key_used) {
        if (in.a == '\n') { pressed = 0; in.key_used = 1; }
        else if (in.a == 27) { pressed = n - 1; in.key_used = 1; }
    }
    return pressed;
}
