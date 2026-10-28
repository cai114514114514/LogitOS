/* aui_lists.inc -- lists, tables, tabs, the segmented control and document tabs.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include.
 *
 * ui-aui-lists: design 4.5 (segmented), 4.8 (lists and tables), 4.11 (document
 * tabs). Tokens come from aui_tok(); the tokens' ARGB fields are drawn with
 * aui_round_a / aui_fill_a. Nothing in here is glass. */

/* ---- shared helpers ---- */

/* An ARGB token as a rounded alpha fill (fill.hover, sep.strong, ...). */
static void ls_round_argb(int x, int y, int w, int h, int r, unsigned argb)
{ aui_round_a(x, y, w, h, r, argb & 0xFFFFFFu, (int)(argb >> 24)); }

/* Window focus. aui has no focus state of its own: a list widget remembers the
 * last EV_WINDOW_FOCUS a frame it drew carried, and treats the window as
 * focused until it has seen one (the WM sends one after create, so a window
 * that is not focused learns it on its first frame). */
static int ls_wfocus = 1;
static void ls_sync_focus(void)
{
    if (in.ev == EV_WINDOW_FOCUS) ls_wfocus = in.a != 0;
}

/* `s` on a metric-centred baseline in a rect rh tall, cut with U+2026 when it
 * does not fit in maxw. */
static void ls_text(int x, int ry, int rh, int maxw, const char *s, int px, int weight,
                    unsigned col, int align)
{
    if (!s || !s[0] || maxw <= 0) return;
    if (aui_text_measure_w(s, px, weight) <= maxw) {
        aui_text_vc(aui_r(x, ry, maxw, rh), s, px, weight, col, align);
        return;
    }
    aui_text_ellipsis_w(x, ry + (rh * 1000 - px * 1448) / 2000, maxw, s, px, weight, col, AUI_TRUNC_TAIL);
}

/* ---- segmented control (design 4.5) ---- */

#define SEG_PAD 24          /* label + 24 */
#define SEG_ICON_W 36

static int seg_seg_w(const char *const *items, const int *icons, int n)
{
    int seg = SEG_ICON_W;
    if (icons) return seg;
    seg = 0;
    for (int i = 0; i < n; i++) seg = imax(seg, aui_text_measure_w(items[i], 13, AUI_W_MEDIUM) + SEG_PAD);
    return seg;
}

/* One segmented control. w <= 0 hugs the labels. */
static int seg_body(int x, int y, int w, int h, const char *const *items, const int *icons,
                    int n, int *sel)
{
    int id = ++id_ctr, changed = 0;
    if (n <= 0) return 0;
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    const struct logit_tokens *T = aui_tok();
    struct aim_ctx aim;
    aim_take(&aim);
    int seg = seg_seg_w(items, icons, n);
    if (w > 0) seg = imax(1, (w - 4) / n);
    int tw_ = seg * n + 4, th = imax(4, h - 4), r = h / 2;
    int s = iclamp(*sel, 0, n - 1);

    /* input first: a click lands on the frame it arrives, and the thumb then
     * glides to the new segment. */
    for (int i = 0; i < n; i++) {
        int over = input_ok(X_(x + 2 + i * seg), Y_(y), seg, h);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE) {
            if (*sel != i) { *sel = i; changed = 1; }
            focus_id = id; focus_vis = 0;
        }
    }
    if (focused && in.ev == EV_KEY && !in.key_used) {
        if (in.a == KEY_LEFT)  { *sel = iclamp(*sel - 1, 0, n - 1); changed = 1; in.key_used = 1; }
        if (in.a == KEY_RIGHT) { *sel = iclamp(*sel + 1, 0, n - 1); changed = 1; in.key_used = 1; }
    }
    s = iclamp(*sel, 0, n - 1);

    aui_round(x, y, tw_, h, r, T->fill_control);
    focus_ring(focused, x, y, tw_, h, r);

    /* the thumb: x on the snappy spring (the width never changes, segments are
     * equal), E1, white / #5A5A64, NO glass. */
    int target = s * seg * 256;
    int tx24 = aui_spring_ex(AUI_AK_VALUE, target, AUI_SPRING_SNAPPY);
    int thx = x + 2 + (tx24 + 128) / 256;
    {
        unsigned thumb = aui_is_dark() ? 0x5A5A64u : 0xFFFFFFu;
        aui_elev(aui_r(thx, y + 2, seg, th), th / 2, LOGIT_E1);
#ifdef UI_LISTS_NEGCTL
        /* THE CONTROL: the pre-「澄」 look too, a glass thumb under the solid one. */
        glass_face(thx, y + 2, seg, th, th / 2, thumb, GLASS_BTN_OPACITY);
#endif
        aui_round(thx, y + 2, seg, th, th / 2, thumb);
    }

    for (int i = 0; i < n; i++) {
        int sx = x + 2 + i * seg;
        int over = input_ok(X_(sx), Y_(y), seg, h);
        if (over && i != s) ls_round_argb(sx, y + 2, seg, th, th / 2, T->fill_hover);
        /* 1 x 12 divider between two unselected neighbours (never beside the thumb) */
        if (i > 0 && i - 1 != s && i != s)
            ls_round_argb(sx, y + (h - 12) / 2, 1, 12, 0, T->sep_strong);
    }
    for (int i = 0; i < n; i++) {
        int sx = x + 2 + i * seg;
        int on = (i == s);
        unsigned col = on ? T->label_primary : T->label_secondary;
        if (icons) gui_icon(icons[i], sx + (seg - 16) / 2, y + (h - 16) / 2, 16, col);
        else aui_text_vc(aui_r(sx, y, seg, h), items[i], 13, on ? AUI_W_MEDIUM : AUI_W_REGULAR, col, AUI_ALIGN_CENTER);
        /* aims only with the thumb at rest */
        if (aim.on && tx24 == target) aim_item(&aim, i, X_(sx + seg / 2), Y_(y + h / 2));
    }
    return changed;
}

int aui_segmented(int x, int y, int w, int h, const char *const *items, int n, int *sel)
{ return seg_body(x, y, w, h, items, 0, n, sel); }

int aui_segmented_icons(int x, int y, int w, int h, const int *icons, int n, int *sel)
{ return seg_body(x, y, w, h, 0, icons, n, sel); }

/* A segmented control h 28, centred in w. */
int aui_tabs(int x, int y, int w, const char *const *items, int n, int *sel)
{
    if (n <= 0) return 0;
    int nat = seg_seg_w(items, 0, n) * n + 4;
    int tw_ = (w > 0 && nat > w) ? w : nat;
    return seg_body(x + (w - tw_) / 2, y, tw_, 28, items, 0, n, sel);
}

/* ---- lists and tables (design 4.8) ---- */

#define LS_MAXROWS 30
#define LS_MAXCOLS 16
#define LS_HEAD_H 28
#define LS_PROG (16 * 256)      /* the progress spring's travel: its 0.25 pt settle band is then 1.6 %, under half a device pixel of a 28 pt row */
#define LS_K_ROWS 0x10          /* spring keys, scoped to the table's widget id */
#define LS_K_PILL 0x11

/* Row-slide state of one table, found by widget id like an animation slot: a
 * state not touched in the previous frame is dropped, so a table that
 * reappears shows its rows settled. */
struct ls_tbl {
    int id; unsigned gen;
    int n;                              /* rows last frame; -1 = fresh */
    unsigned key[LS_MAXROWS];           /* identity: first cell (+ occurrence) */
    int from24[LS_MAXROWS], disp24[LS_MAXROWS];   /* y at the start of the slide / as drawn */
    int base24, target24, x24;          /* the progress spring runs base -> base + LS_PROG; x24 = last value */
    int preset;
};
static struct ls_tbl ls_st[4];

static struct ls_tbl *ls_state(int id)
{
    struct ls_tbl *v = 0;
    for (int k = 0; k < 4; k++) {
        struct ls_tbl *t = &ls_st[k];
        if (t->id == id && (t->gen == anim_gen || t->gen + 1 == anim_gen)) { t->gen = anim_gen; return t; }
    }
    for (int k = 0; k < 4; k++) {                  /* a free or stale slot, else the oldest */
        struct ls_tbl *t = &ls_st[k];
        if (t->id == 0 || t->gen + 1 < anim_gen) { v = t; break; }
        if (!v || t->gen < v->gen) v = t;
    }
    v->id = id; v->gen = anim_gen; v->n = -1;
    v->base24 = 0; v->target24 = LS_PROG; v->x24 = LS_PROG; v->preset = AUI_SPRING_SMOOTH;
    return v;
}

static unsigned ls_hash(const char *s, int occ)
{
    unsigned h = 2166136261u;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h + (unsigned)occ * 0x9E3779B1u;
}

/* Column drag (resize handles). One drag at a time. */
static struct { int id, col, mx0, w0; } ls_drag;

static int ls_body(int x, int y, int w, int h, struct aui_col *cols, int ncol, int resizable,
                   const char *const *cells, int nrow, int *sel, int *scroll,
                   struct aui_sort *sort, unsigned flags, int rh)
{
    int id = ++id_ctr, activated = -1;
    int range = (flags & AUI_TBL_RANGE) != 0 && sel;
    int head = (flags & AUI_TBL_NOHEAD) ? 0 : LS_HEAD_H;
    const struct logit_tokens *T = aui_tok();
    struct aim_ctx aim;
    int nc = ncol > 0 ? ncol : 1;
    int colx[LS_MAXCOLS + 1], colw[LS_MAXCOLS];
    if (nc > LS_MAXCOLS) nc = LS_MAXCOLS;
    if (rh <= 0) rh = 28;
    if (flags & AUI_TBL_EMPTY) nrow = 0;      /* the body stays clear for the empty state */
    ls_sync_focus();
    aim_take(&aim);
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);

    /* ---- columns: given widths, the last fills ---- */
    colx[0] = 0;
    for (int c = 0; c < nc; c++) {
        colw[c] = (ncol > 0 && c < nc - 1) ? imax(40, cols[c].width) : imax(40, w - colx[c]);
        colx[c + 1] = colx[c] + colw[c];
    }

    int wx = X_(x), wy = Y_(y);
    if (input_ok(wx, wy, w, h) && in.ev == EV_MOUSE) { focus_id = id; focus_vis = 0; }

    /* the whole widget sits on bg.content: header and body one surface (4.8) */
    aui_fill(x, y, w, h, T->bg_content);

    /* ---- header ---- */
    if (head) {
        int hov = input_ok(wx, wy, w, head);
        if (ncol > 0) {
            /* resize handles */
            if (resizable && ls_drag.id == id) {
                if (in.down) cols[ls_drag.col].width = imax(40, ls_drag.w0 + in.mx - ls_drag.mx0);
                else ls_drag.id = 0;
            }
            for (int c = 0; c < nc - 1 && resizable; c++) {
                int bx = colx[c + 1];
                int over = input_ok(X_(x + bx - 3), wy, 6, head);
                if (over && in.ev == EV_MOUSE) {
                    ls_drag.id = id; ls_drag.col = c; ls_drag.mx0 = in.mx; ls_drag.w0 = colw[c];
                    in.ev = 0;      /* the press is the handle's, not a sort click */
                }
                if (hov || ls_drag.id == id)
                    aui_hairline_dev(x + bx, y + 7, x + bx, y + head - 7, T->sep_hairline);
            }
            for (int c = 0; c < nc; c++) {
                int cx = x + colx[c], num = (cols[c].flags & AUI_COL_NUM) != 0;
                int sortable = sort && !(cols[c].flags & AUI_COL_NOSORT);
                int sorted = sort && sort->col == c;
                int over = sortable && input_ok(X_(cx), wy, colw[c] - (c < nc - 1 ? 3 : 0), head);
                if (over && in.ev == EV_MOUSE) {
                    if (sort->col == c) sort->desc = !sort->desc; else { sort->col = c; sort->desc = 0; }
                    sort->changed = 1;
                    sorted = 1;
                }
                int padl = c == 0 ? 12 : 8;
                int chev = sorted ? 14 : 0;                 /* 10 + 4 */
                if (num) {
                    ls_text(cx + 8, y, head, colw[c] - 16 - chev, cols[c].title, 12, AUI_W_MEDIUM,
                            T->label_secondary, AUI_ALIGN_RIGHT);
                    if (sorted) {
                        /* the label ends at cx + colw - 22 (right inset 8 + chevron 14) */
                        gui_icon(sort->desc ? GICON_CHEVRON_D : GICON_CHEVRON_U,
                                 cx + colw[c] - 18, y + (head - 10) / 2, 10, T->label_tertiary);
                    }
                } else {
                    ls_text(cx + padl, y, head, colw[c] - padl - 8 - chev, cols[c].title, 12, AUI_W_MEDIUM,
                            T->label_secondary, AUI_ALIGN_LEFT);
                    if (sorted) {
                        int lw = aui_text_measure_w(cols[c].title, 12, AUI_W_MEDIUM);
                        gui_icon(sort->desc ? GICON_CHEVRON_D : GICON_CHEVRON_U,
                                 cx + padl + imin(lw, colw[c] - padl - 8 - chev) + 4, y + (head - 10) / 2, 10,
                                 T->label_tertiary);
                    }
                }
            }
        }
        aui_hairline_dev(x, y + head - 1, x + w, y + head - 1, T->sep_strong);
    }

    /* ---- body ---- */
    int by = y + head, bh = h - head;
    if (bh <= 0) return activated;
    int cur = nrow > 0 ? (range ? sel[1] : (sel ? *sel : -1)) : -1;
    if (focused && in.ev == EV_KEY && !in.key_used && nrow > 0 && sel) {
        int page = imax(1, bh / rh), ext = range && (in.mods & EV_MOD_SHIFT);
        int nv = cur, used = 1;
        if (in.a == KEY_DOWN)      nv = iclamp(cur + 1, 0, nrow - 1);
        else if (in.a == KEY_UP)   nv = iclamp(cur - 1, 0, nrow - 1);
        else if (in.a == KEY_HOME) nv = 0;
        else if (in.a == KEY_END)  nv = nrow - 1;
        else if (in.a == KEY_PGDN) nv = iclamp(cur + page, 0, nrow - 1);
        else if (in.a == KEY_PGUP) nv = iclamp(cur - page, 0, nrow - 1);
        else if (in.a == '\n')     { activated = cur; used = 0; in.key_used = 1; }
        else used = 0;
        if (used) {
            if (range) { if (!ext || sel[0] < 0) sel[0] = nv; sel[1] = nv; } else *sel = nv;
            in.key_used = 1;
            int top = nv * rh, bot = top + rh;               /* keep it visible */
            if (top < *scroll) *scroll = top;
            if (bot > *scroll + bh) *scroll = bot - bh;
            cur = nv;
        }
    }

    aui_scroll_begin(x, by, w, bh, scroll, nrow * rh);
    int sc_i = scrn > 0 && scr[scrn - 1].off == scroll;
    int aim_ok = aim.on && !(sc_i && scr[scrn - 1].moving);
    int aim_w = w - (sc_i && scr[scrn - 1].show_bar ? 12 : 0);

    /* ---- row identity and the slide ---- */
    int slide = nrow > 0 && nrow <= LS_MAXROWS;
    int rows_settled = 1;
    int save_id = id_ctr;
    int disp[LS_MAXROWS];
    id_ctr = id;
    if (slide) {
        struct ls_tbl *t = ls_state(id);
        unsigned key[LS_MAXROWS];
        for (int i = 0; i < nrow; i++) {
            key[i] = ls_hash(cells[i * nc], 0);
            for (int j = 0; j < i; j++) if (key[j] == key[i]) { key[i] += 0x9E3779B1u; j = -1; }   /* duplicates: next occurrence */
        }
        int changed = 0;
        if (t->n >= 0) {
            if (t->n != nrow) changed = 1;
            else for (int i = 0; i < nrow; i++) if (t->key[i] != key[i]) { changed = 1; break; }
        }
        if (changed) {
            int from[LS_MAXROWS];
            for (int i = 0; i < nrow; i++) {
                from[i] = i * rh * 256;
                for (int j = 0; j < t->n; j++) if (t->key[j] == key[i]) { from[i] = t->disp24[j]; break; }
            }
            for (int i = 0; i < nrow; i++) t->from24[i] = from[i];
            t->base24 = t->x24; t->target24 = t->x24 + LS_PROG;
            t->preset = (t->n == nrow) ? AUI_SPRING_SMOOTH : AUI_SPRING_SNAPPY;
        }
        for (int i = 0; i < nrow; i++) t->key[i] = key[i];
        int px24 = aui_spring_ex(LS_K_ROWS, t->target24, (enum aui_spring)t->preset);
        int frac = iclamp((int)(((long long)(px24 - t->base24) * 256) / LS_PROG), 0, 256);
        if (t->n < 0) { t->base24 = t->target24 - LS_PROG; frac = 256; for (int i = 0; i < nrow; i++) t->from24[i] = i * rh * 256; }
        t->n = nrow; t->x24 = px24;
        for (int i = 0; i < nrow; i++) {
            int to24 = i * rh * 256;
            t->disp24[i] = t->from24[i] + (int)(((long long)(to24 - t->from24[i]) * frac) / 256);
            disp[i] = (t->disp24[i] + 128) / 256;
        }
        rows_settled = (px24 == t->target24);
    } else {
        for (int i = 0; i < nrow && i < LS_MAXROWS; i++) disp[i] = i * rh;
    }
#define LS_RY(i) (slide ? disp[i] : (i) * rh)

    /* ---- clicks (before the pill is placed, so it glides this frame) ---- */
    for (int i = 0; i < nrow && sel; i++) {
        int ry = LS_RY(i);
        if (ry + rh < *scroll || ry > *scroll + bh) continue;
        if (in.ev == EV_MOUSE && input_ok(X_(0), Y_(ry), aim_w, rh)) {
            if (range) {
                if ((in.mods & EV_MOD_SHIFT) && sel[0] >= 0) sel[1] = i; else sel[0] = sel[1] = i;
            } else *sel = i;
            activated = i; cur = i;
        }
    }
    int a = -1, b = -1;
    if (sel && nrow > 0) {
        if (range) { a = imin(sel[0], sel[1]); b = imax(sel[0], sel[1]); if (sel[0] < 0 || sel[1] < 0) a = b = -1; }
        else a = b = *sel;
        if (a < 0 || b >= nrow) a = b = -1;
    }

    /* the pill's top y glides on snappy (single selection); a block is placed */
    int pill_settled = 1, pill_y = 0;
    if (a >= 0) {
        int tgt = a * rh * 256;
        int py24 = aui_spring_ex(LS_K_PILL, tgt, AUI_SPRING_SNAPPY);
        pill_y = (py24 + 128) / 256;
        pill_settled = (py24 == tgt);
        if (a != b) pill_y = a * rh;
    }
    id_ctr = save_id;

    /* ---- backgrounds: zebra, hover pill, selection ---- */
    for (int i = 0; i < nrow; i++) {
        int ry = LS_RY(i);
        if (ry + rh < *scroll || ry > *scroll + bh) continue;
        if (nc >= 4 && (i & 1)) aui_fill(0, ry, w, rh, T->fill_zebra);
        if (!(i >= a && i <= b) && input_ok(X_(0), Y_(ry), aim_w, rh))
            ls_round_argb(6, ry + 1, aim_w - 12, rh - 2, 6, T->fill_hover);
    }
    if (a >= 0)
        aui_round(6, pill_y + 1, aim_w - 12, (b - a + 1) * rh - 2, 6,
                  ls_wfocus ? T->accent : T->fill_inactive_sel);
    else if (focused && focus_vis && nrow > 0)
        aui_stroke(6, 1, aim_w - 12, rh - 2, 6, 1, T->accent);       /* keyboard focus, no selection */

    /* ---- text ---- */
    for (int i = 0; i < nrow; i++) {
        int ry = LS_RY(i);
        if (ry + rh < *scroll || ry > *scroll + bh) continue;
        int on = (i >= a && i <= b);
        int onacc = on && ls_wfocus;
        for (int c = 0; c < nc; c++) {
            const char *s = cells[i * nc + c];
            int num = ncol > 0 && (cols[c].flags & AUI_COL_NUM);
            int pad = c == 0 ? 12 : 8;
            unsigned col = onacc ? T->label_on_accent : (c == 0 || on) ? T->label_primary : T->label_secondary;
            int px = c == 0 ? 14 : 13;
            if (num)
                aui_text_tnum(colx[c] + colw[c] - 8, ry + (rh * 1000 - px * 1448) / 2000, px, AUI_W_REGULAR, col, s, AUI_ALIGN_RIGHT);
            else
                ls_text(colx[c] + pad, ry, rh, colw[c] - pad - 8, s, px, AUI_W_REGULAR, col, AUI_ALIGN_LEFT);
        }
        if (aim_ok && rows_settled && pill_settled)
            aim_item(&aim, i, X_(aim_w / 2), Y_(i * rh + rh / 2));
    }
    /* AUI_TBL_EMPTY draws nothing more: the body stays clear (no rows, no zebra)
     * and the app centres its empty state (4.13, aui_empty_state) over it. */
    aui_scroll_end();
    return activated;
}
#undef LS_RY

int aui_list(int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll)
{ return ls_body(x, y, w, h, 0, 0, 0, items, n, sel, scroll, 0, AUI_TBL_NOHEAD, 28); }

int aui_table(int x, int y, int w, int h, const char *const *cols, const int *widths,
              int ncol, const char *const *cells, int nrow, int *sel, int *scroll)
{
    struct aui_col c[LS_MAXCOLS];
    int n = ncol > LS_MAXCOLS ? LS_MAXCOLS : ncol;
    for (int i = 0; i < n; i++) { c[i].title = cols[i]; c[i].width = widths[i]; c[i].flags = 0; }
    return ls_body(x, y, w, h, c, n, 0, cells, nrow, sel, scroll, 0, 0, 28);
}

int aui_table_ex(int x, int y, int w, int h, struct aui_col *cols, int ncol,
                 const char *const *cells, int nrow, int *sel, int *scroll,
                 struct aui_sort *sort, unsigned flags, int row_h)
{
    if (sort) sort->changed = 0;
    return ls_body(x, y, w, h, cols, ncol, 1, cells, nrow, sel, scroll, sort, flags, row_h);
}

/* ---- document tabs (design 4.11) ---- */

#define DT_MAX 16
#define DT_H 30
struct dt_ent { int key, live, pos, icon; unsigned flags; char title[40]; };
struct dt_state { int id; unsigned gen; struct dt_ent e[DT_MAX]; };
static struct dt_state dt_st[2];

static struct dt_state *dt_state(int id)
{
    struct dt_state *v = &dt_st[0];
    for (int k = 0; k < 2; k++) {
        struct dt_state *t = &dt_st[k];
        if (t->id == id && (t->gen == anim_gen || t->gen + 1 == anim_gen)) { t->gen = anim_gen; return t; }
        if (t->id == 0 || t->gen + 1 < anim_gen) v = t;
    }
    for (int i = 0; i < DT_MAX; i++) v->e[i].key = 0;
    v->id = id; v->gen = anim_gen;
    return v;
}

static void dt_copy(char *d, const char *s)
{
    int n = 0;
    while (s && s[n] && n < 39) { d[n] = s[n]; n++; }
    while (n > 0 && s[n] && ((unsigned char)s[n] & 0xC0) == 0x80) n--;     /* not inside a sequence */
    d[n] = 0;
}

int aui_doctabs(int x, int y, int w, const struct aui_doctab *items, int n, int *active, unsigned flags)
{
    int id = ++id_ctr, result = 0;
    const struct logit_tokens *T = aui_tok();
    struct aim_ctx aim;
    struct dt_state *st = dt_state(id);
    int ord[DT_MAX], oc = 0, born[DT_MAX], wpx[DT_MAX];
    int settled = 1;
    if (n > DT_MAX) n = DT_MAX;
    aim_take(&aim);

    /* ---- identity: live items, then the keys that vanished keep a ghost ---- */
    int seen[DT_MAX] = {0};
    int item_of[DT_MAX];
    for (int k = 0; k < DT_MAX; k++) born[k] = 0, item_of[k] = -1;
    for (int i = 0; i < n; i++) {
        int key = items[i].key ? items[i].key : i + 1, e = -1;
        for (int k = 0; k < DT_MAX; k++) if (st->e[k].key == key) { e = k; break; }
        if (e < 0) {
            for (int k = 0; k < DT_MAX; k++) if (!st->e[k].key) { e = k; break; }
            if (e < 0) continue;
            st->e[e].key = key; st->e[e].live = 0; born[e] = 1;
        }
        st->e[e].live = 1; st->e[e].pos = i; seen[e] = 1; item_of[e] = i;
        st->e[e].icon = items[i].icon; st->e[e].flags = items[i].flags;
        dt_copy(st->e[e].title, items[i].title);
        ord[oc++] = e;
    }
    for (int k = 0; k < DT_MAX; k++) {
        if (!st->e[k].key || seen[k]) continue;
        st->e[k].live = 0;
        int at = imin(st->e[k].pos, oc);
        for (int j = oc; j > at; j--) ord[j] = ord[j - 1];
        ord[at] = k; oc++;
        if (oc >= DT_MAX) break;
    }

    /* ---- widths: 120..220, springing from / to 0 on snappy ---- */
    int avail = w - 8 - ((flags & AUI_DT_NO_PLUS) ? 0 : 32) - 8;
    int tw_ = n > 0 ? iclamp(avail / n, 120, 220) : 160;
    int tgt[DT_MAX];
    int save_id = id_ctr;
    id_ctr = id;
    for (int j = 0; j < oc; j++) {
        int e = ord[j];
        tgt[j] = st->e[e].live ? tw_ * 256 : 0;
        int sk = 0x100 + e;                 /* one spring per entry slot: keys never collide */
        if (born[e]) aui_spring_ex(sk, 0, AUI_SPRING_SNAPPY);       /* opens from 0 */
        int wk = aui_spring_ex(sk, tgt[j], AUI_SPRING_SNAPPY);
        wpx[j] = (wk + 128) / 256;
        if (wk != tgt[j]) settled = 0;
        if (!st->e[e].live && wk < 256) { st->e[e].key = 0; wpx[j] = 0; }   /* collapsed: gone */
    }
    id_ctr = save_id;

    /* ---- strip ---- */
    aui_fill(x, y, w, DT_H, T->bg_toolbar);
    int cx = x + 8, act_item = active ? iclamp(*active, 0, n - 1) : -1;
    int prev_inactive = 0, plus_x;
    for (int j = 0; j < oc; j++) {
        int e = ord[j], wj = wpx[j];
        if (!st->e[e].key || wj <= 0) continue;
        int live = st->e[e].live, ii = item_of[e];
        int isact = live && ii == act_item;
        int over = live && input_ok(X_(cx), Y_(y), wj, DT_H);
        int showx = live && (isact || over) && wj >= 100;
        int xz_x = cx + wj - 22, xz_y = y + 7;
        int over_x = showx && input_ok(X_(xz_x), Y_(xz_y), 16, 16);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE && !result) {
            if (over_x) result = AUI_DT_CLOSE | (ii << 8);
            else { if (active && *active != ii) { *active = ii; act_item = ii; isact = 1; result = AUI_DT_ACTIVATE | (ii << 8); } }
        }
        /* divider between two inactive neighbours, hidden beside hover or the active tab */
        if (!isact && !over && prev_inactive) ls_round_argb(cx, y + 7, 1, 16, 0, T->sep_strong);
        if (isact) {
            int rr[4] = { 8, 8, 0, 0 };
            aui_round4(aui_r(cx, y, wj, DT_H), rr, 0xFF000000u | T->bg_content);
        } else if (over) {
            ls_round_argb(cx + 2, y + 3, wj - 4, DT_H - 6, 6, T->fill_hover);
        }
        prev_inactive = live && !isact && !over;
        if (wj >= 60) {
            unsigned tcol = isact ? T->label_primary : T->label_secondary;
            if (st->e[e].flags & AUI_DTAB_LOADING) aui_spinner(cx + 17, y + DT_H / 2, 6);
            else if (st->e[e].icon >= 0) gui_icon(st->e[e].icon, cx + 10, y + (DT_H - 14) / 2, 14, tcol);
            ls_text(cx + 30, y, DT_H, wj - 30 - 26, st->e[e].title, 13, isact ? AUI_W_MEDIUM : AUI_W_REGULAR, tcol, AUI_ALIGN_LEFT);
            if (showx) {
                if (over_x) ls_round_argb(xz_x, xz_y, 16, 16, 8, T->fill_hover);
                gui_icon(GICON_CLOSE, xz_x + 4, xz_y + 4, 8, T->label_secondary);
            } else if (live && (st->e[e].flags & AUI_DTAB_UNSAVED) && wj >= 100) {
                aui_circle(xz_x + 8, xz_y + 8, 3, T->label_secondary);
            }
        }
        if (aim.on && settled && live) aim_item(&aim, ii, X_(cx + wj / 2), Y_(y + DT_H / 2));
        cx += wj;
    }
    plus_x = cx + 4;
    if (!(flags & AUI_DT_NO_PLUS)) {
        int over = input_ok(X_(plus_x), Y_(y + 3), 24, 24);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE && !result) result = AUI_DT_NEW | (n << 8);
        if (over) ls_round_argb(plus_x, y + 3, 24, 24, 6, T->fill_hover);
        gui_icon(GICON_PLUS, plus_x + 5, y + 8, 14, T->label_secondary);
        if (aim.on && settled) aim_item(&aim, n, X_(plus_x + 12), Y_(y + DT_H / 2));
    }
    return result;
}
