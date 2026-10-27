/* aui_lists.inc -- lists, tables, tabs and the segmented control.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

/* ---- lists and tables ---- */

static int list_body(int x, int y, int w, int h, const char *const *items, int n,
                     int *sel, int *scroll, const int *widths, int ncol)
{
    int id = ++id_ctr, activated = -1;
    int wx = X_(x), wy = Y_(y);
    struct aim_ctx aim;
    aim_take(&aim);                                 /* one aim per visible row */
    /* The list itself is focusable; its rows are not, so Tab does not walk a
     * thousand-row table one row at a time. */
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    int nrow = ncol > 0 ? n / ncol : n;

    if (input_ok(wx, wy, w, h) && in.ev == EV_MOUSE) { focus_id = id; focus_vis = 0; }
    if (focused && in.ev == EV_KEY && !in.key_used) {
        if (in.a == KEY_DOWN)      { *sel = iclamp(*sel + 1, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == KEY_UP)   { *sel = iclamp(*sel - 1, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == KEY_HOME) { *sel = 0; in.key_used = 1; }
        else if (in.a == KEY_END)  { *sel = nrow - 1; in.key_used = 1; }
        else if (in.a == KEY_PGDN) { *sel = iclamp(*sel + h / ROWH, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == KEY_PGUP) { *sel = iclamp(*sel - h / ROWH, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == '\n')     { activated = *sel; in.key_used = 1; }
        if (in.key_used) {                              /* keep the selection visible */
            int top = *sel * ROWH, bot = top + ROWH;
            if (top < *scroll) *scroll = top;
            if (bot > *scroll + h) *scroll = bot - h;
        }
    }

    aui_round(x, y, w, h, AUI_R_MD, AUI_SURFACE);
    focus_ring(focused, x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, AUI_BORDER);

    aui_scroll_begin(x + 1, y + 1, w - 2, h - 2, scroll, nrow * ROWH);
    /* Rows have no aims while the scroll is still easing (added). */
    int aim_ok = aim.on && !(scrn > 0 && scr[scrn - 1].off == scroll && scr[scrn - 1].moving);
    int aim_w = w - 2 - (scrn > 0 && scr[scrn - 1].off == scroll && scr[scrn - 1].show_bar ? 12 : 0);
    for (int i = 0; i < nrow; i++) {
        int ry = i * ROWH;
        if (ry + ROWH < *scroll || ry > *scroll + h) continue;     /* culled */
        int hovered = input_ok(X_(0), Y_(ry), w - 2, ROWH);
        /* DELIBERATELY INSTANT, both of these. Selection must land where the
         * click landed on the frame the click landed -- a highlight that
         * travels toward a row the pointer has already left reads as lag, not
         * motion. And a list is the one place the pointer crosses a dozen
         * targets in a couple hundred milliseconds; a per-row fade would still
         * be arriving three rows after the pointer moved on. Keeping this
         * instant is itself an application of the vocabulary, not an omission
         * from it -- see the design dossier's list-selection item. */
        if (i == *sel)      aui_fill(0, ry, w - 2, ROWH, AUI_ACCENT);
        else if (hovered)   aui_fill_a(0, ry, w - 2, ROWH, AUI_TEXT, 14);
        unsigned fg = (i == *sel) ? AUI_ACCENT_TEXT : AUI_TEXT;
        if (ncol > 0) {
            int cx = AUI_SP(2);
            for (int c = 0; c < ncol; c++) {
                aui_text_ellipsis(cx, ry + (ROWH - PX) / 2 - 1, widths[c] - AUI_SP(2),
                                  items[i * ncol + c], c ? (i == *sel ? fg : AUI_MUTED) : fg, PX);
                cx += widths[c];
            }
        } else {
            aui_text_ellipsis(AUI_SP(2), ry + (ROWH - PX) / 2 - 1, w - AUI_SP(6), items[i], fg, PX);
        }
        if (i + 1 < nrow) aui_fill(AUI_SP(2), ry + ROWH - 1, w - AUI_SP(4) - 12, 1,
                                   aui_mix(AUI_SURFACE, AUI_BORDER, 140));
        if (in.ev == EV_MOUSE && hovered) { *sel = i; activated = i; }
        if (aim_ok) aim_item(&aim, i, X_(aim_w / 2), Y_(ry + ROWH / 2));
    }
    aui_scroll_end();
    return activated;
}

int aui_list(int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll)
{ return list_body(x, y, w, h, items, n, sel, scroll, 0, 0); }

int aui_table(int x, int y, int w, int h, const char *const *cols, const int *widths,
              int ncol, const char *const *cells, int nrow, int *sel, int *scroll)
{
    int hh = 26;
    aui_round(x, y, w, hh + AUI_R_MD, AUI_R_MD, aui_mix(AUI_SURFACE, AUI_BG, 120));
    aui_fill(x, y + hh - 1, w, 1, AUI_BORDER);
    int cx = x + AUI_SP(2);
    for (int c = 0; c < ncol; c++) {
        aui_text_ellipsis(cx, y + (hh - AUI_FS_LABEL) / 2 - 1, widths[c] - AUI_SP(2),
                          cols[c], AUI_MUTED, AUI_FS_LABEL);
        cx += widths[c];
    }
    aui_stroke(x, y, w, hh, AUI_R_MD, 1, AUI_BORDER);
    return list_body(x, y + hh, w, h - hh, cells, nrow * ncol, sel, scroll, widths, ncol);
}

/* ---- tabs / segmented ---- */

int aui_tabs(int x, int y, int w, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, h = 34, changed = 0;
    if (n <= 0) return 0;
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    int cx = x, selx = x, selw = 0;
    struct aim_ctx aim;
    int aim_cx[AIM_ITEMS_MAX];                      /* tab centres, for the aims */
    aim_take(&aim);
    aui_fill(x, y + h - 1, w, 1, AUI_BORDER);
    for (int i = 0; i < n; i++) {
        int tw_ = tw(items[i]) + AUI_SP(6);
        if (i == *sel) { selx = cx; selw = tw_; }
        int over = input_ok(X_(cx), Y_(y), tw_, h);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE) { if (*sel != i) { *sel = i; changed = 1; } focus_id = id; focus_vis = 0; }
        unsigned fg = (i == *sel) ? AUI_TEXT : AUI_MUTED;
        if (over && i != *sel) aui_fill_a(cx, y + 4, tw_, h - 5, AUI_TEXT, 12);
        aui_text_sz(cx + AUI_SP(3), y + (h - PX) / 2 - 1, items[i], fg, PX);
        if (i < AIM_ITEMS_MAX) aim_cx[i] = cx + tw_ / 2;
        cx += tw_;
    }
    /* THE MOVING INDICATOR, drawn once after the loop rather than once per tab,
     * and on AUI_EASE_INOUT: this is the one category that earns it, an
     * indicator that starts and ends at rest and whose midpoint the eye tracks
     * across open space (aui.h's own words for this and the segmented pill).
     * aui_anim's domain is 0..255, which cannot hold a raw pixel x or width on
     * a window wider than that -- so both are normalised to a fraction of
     * win_w first and scaled back afterward, the same trick the toggle knob
     * uses for its own travel, generalised from two stops to N. Width is
     * animated too, unlike the segmented pill, because tab labels are not a
     * fixed division of the strip. */
    int tx = aui_anim(AUI_AK_VALUE, selx * 255 / imax(1, win_w), AUI_T_BASE, AUI_EASE_INOUT);
    int twid = aui_anim(AUI_AK_EXTRA, selw * 255 / imax(1, win_w), AUI_T_BASE, AUI_EASE_INOUT);
    int ux = tx * win_w / 255, uw = twid * win_w / 255;
    /* Aims once the underline has arrived (added): none while it travels. */
    if (aim.on && tx == iclamp(selx * 255 / imax(1, win_w), 0, 255) &&
        twid == iclamp(selw * 255 / imax(1, win_w), 0, 255))
        for (int i = 0; i < n && i < AIM_ITEMS_MAX; i++) aim_item(&aim, i, X_(aim_cx[i]), Y_(y + h / 2));
    aui_round(ux + AUI_SP(2), y + h - 3, imax(0, uw - AUI_SP(4)), 3, 1, AUI_ACCENT);
    if (focused) {
        /* Ring the SELECTED TAB, not the whole strip: a box drawn around every
         * tab at once says "one of these is focused" and nothing about which,
         * which is the only thing a focus indicator is for. */
        if (in.ev == EV_KEY && !in.key_used) {
            if (in.a == KEY_LEFT)  { *sel = iclamp(*sel - 1, 0, n - 1); changed = 1; in.key_used = 1; }
            if (in.a == KEY_RIGHT) { *sel = iclamp(*sel + 1, 0, n - 1); changed = 1; in.key_used = 1; }
        }
    }
    focus_ring(focused, selx + 2, y + 3, selw - 4, h - 8, AUI_R_SM);
    return changed;
}

int aui_segmented(int x, int y, int w, int h, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, changed = 0;
    if (n <= 0) return 0;
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    int seg = w / n;
    struct aim_ctx aim;
    aim_take(&aim);
    aui_round(x, y, w, h, AUI_R_MD, AUI_TRACK);
    focus_ring(focused, x, y, w, h, AUI_R_MD);
    /* The moving pill is drawn first so the labels sit on top of it. Segment
     * width is CONSTANT across a change of selection -- only x moves -- so
     * only one aui_anim call is needed (unlike the tab underline, which also
     * has to animate width) and the pill's mask stays a cache hit through the
     * whole slide. AUI_EASE_INOUT for the same reason as the tab underline:
     * this is the indicator category that earns it. */
    int pt_to = iclamp(*sel, 0, n - 1) * seg * 255 / imax(1, w);
    int pt = aui_anim(AUI_AK_VALUE, pt_to, AUI_T_BASE, AUI_EASE_INOUT);
    int px = x + pt * w / 255 + 2;
    aui_shadow_ex(px, y + 2, seg - 4, h - 4, AUI_R_SM, 1, 3, 60);
    if (!glass_face(px, y + 2, seg - 4, h - 4, AUI_R_SM, AUI_SURFACE, GLASS_BTN_OPACITY))
        aui_round(px, y + 2, seg - 4, h - 4, AUI_R_SM, AUI_SURFACE);
    for (int i = 0; i < n; i++) {
        int sx = x + i * seg;
        int over = input_ok(X_(sx), Y_(y), seg, h);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE) { if (*sel != i) { *sel = i; changed = 1; } focus_id = id; focus_vis = 0; }
        aui_text_in(aui_r(sx, y, seg, h), items[i], i == *sel ? AUI_TEXT : AUI_MUTED,
                    AUI_FS_LABEL, AUI_ALIGN_CENTER);
        /* Aims only with the thumb at rest (added). */
        if (aim.on && pt == iclamp(pt_to, 0, 255)) aim_item(&aim, i, X_(sx + seg / 2), Y_(y + h / 2));
    }
    if (focused && in.ev == EV_KEY && !in.key_used) {
        if (in.a == KEY_LEFT)  { *sel = iclamp(*sel - 1, 0, n - 1); changed = 1; in.key_used = 1; }
        if (in.a == KEY_RIGHT) { *sel = iclamp(*sel + 1, 0, n - 1); changed = 1; in.key_used = 1; }
    }
    return changed;
}
