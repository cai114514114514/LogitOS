/* wm_menu.c -- the menu model and dropdowns.
 *
 * What the menu bar's titles open: the model (the kernel's LogitOS menu,
 * then the top window's own SYS_GUI_MENU_SET menus or the kernel's File and
 * View, then Window), the dropdown's geometry, painting, hit-testing and
 * report lines, the actions an item fires, Cmd+<key> routing to an app's
 * menu, and the open menu's share of the pointer.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

/* ============================================================================
 * THE MENU BAR COMES ALIVE.
 *
 * "LogitOS  File  View" used to be three fb_text calls and nothing else --
 * paint of a menu bar, not one. This section is the machinery: hit rects for
 * the titles, a dropdown panel drawn like the dock (fresh from state every
 * time it is asked for, never a retained surface), and the handful of real
 * actions the WM can actually perform. The dropdown's items live here, in
 * wm_menu.c, and the About/Shut Down/Restart panel in wm_overlay.c; the
 * TITLE layout lives with draw_menubar() in wm_menubar.c, because that is
 * the one thing that paints it.
 *
 * "Edit" is gone on purpose. The WM cannot reach inside an app -- it has no
 * text buffer to cut or paste -- so a Cut/Copy/Paste menu here would be three
 * items that do nothing, which is worse than no menu: it teaches a user this
 * desktop's chrome is decoration. File and View both survive because both
 * gained something real: File routes to the close button and the minimise
 * gesture the traffic lights already perform; View is the on-screen home for
 * Expose, which otherwise has no menu presence at all (hot corner + Cmd+E,
 * neither of them discoverable by looking at the screen).
 *
 * ONE FUNCTION SAYS WHERE A TITLE IS -- same discipline win_draw_rect enforces
 * for a window's footprint. menu_bar_layout() publishes g_menu_tx/g_menu_tw;
 * draw_menubar() (paint) and menu_title_hit() (click + hover) both read them,
 * so a click and the word it lands on cannot disagree about where the word is.
 * Valid only after the first draw_menubar() -- the same "not on screen yet
 * either" caveat dock_hover_at documents for dock_x0/dock_y0. */
/* THE MODEL. Correction 2026-09-24 ("Fluid" P5): the titles used to be three
 * constants -- { "LogitOS", "File", "View" } -- and no app could put a command
 * on the menu bar. Now the bar is the kernel's LogitOS menu, then the menus the
 * top window published with SYS_GUI_MENU_SET followed by the kernel's Window
 * menu; a window that published nothing gets the old File and View, unchanged.
 * menu_model_sync() rebuilds it, and never while a menu is open, so a dropdown
 * cannot change shape under the pointer that is choosing from it. */
enum { MK_LOGO, MK_FILE, MK_VIEW, MK_WINDOW, MK_APP };
struct mrow { char label[MB_LABEL_MAX + 1]; uint16_t id; uint8_t flags; char key; };
int g_nmenu;
static uint8_t g_menu_kind[NMENU_MAX];
char g_menu_title[NMENU_MAX][MB_TITLE_MAX + 1];
static struct mrow g_menu_rows[NMENU_MAX][MB_MAX_ITEMS];
static int g_menu_nitems[NMENU_MAX];
static int g_menu_src = -2;              /* window whose menus these are, -1 none, -2 never built */
static unsigned g_menu_src_gen;
int g_menu_reported;                     /* menu_bar_layout's report, once per model */

/* -1 = closed; else an index into g_menu_title[]/g_menu_rows[]. */
int g_menu_open = -1;
static int g_menu_item_hov = -1;

/* ---- the dropdown's items -------------------------------------------------
 * One row array per menu (g_menu_rows), walked by both the painter and the
 * hit-tester below so a row's picture and its hitbox are the same
 * computation. MB_SEP is a separator: drawn as a hairline, never hit-tested.
 * The kernel's own menus are these tables; an app's come from its blob. The
 * `key` column is the Cmd shortcut drawn at the right. */
struct kmi { const char *label; uint16_t id; uint8_t flags; char key; };
static const struct kmi KM_LOGO[] = {
    { "关于本机", 0, 0, 0 }, { "设置…", 1, 0, ',' }, { "", 2, MB_SEP, 0 },
    { "关机…", 3, 0, 0 }, { "重新启动…", 4, 0, 0 },
};
/* File/View (no app menus) and Window (after them) share ids: 0 close,
 * 1 minimise, 2 Expose -- one switch in menu_item_fire serves all three. */
static const struct kmi KM_FILE[] = { { "关闭窗口", 0, 0, 'W' }, { "最小化", 1, 0, 'M' } };
static const struct kmi KM_VIEW[] = { { "显示所有窗口", 2, 0, 'E' } };
static const struct kmi KM_WINDOW[] = {
    { "最小化", 1, 0, 'M' }, { "关闭窗口", 0, 0, 'W' }, { "", 2, MB_SEP, 0 },
    { "显示所有窗口", 2, 0, 'E' },
};

static void menu_add_kernel(int kind, const char *title, const struct kmi *t, int n)
{
    int m = g_nmenu++;
    g_menu_kind[m] = (uint8_t)kind;
    scopy(g_menu_title[m], title, sizeof g_menu_title[m]);
    g_menu_nitems[m] = n;
    for (int i = 0; i < n; i++) {
        struct mrow *r = &g_menu_rows[m][i];
        scopy(r->label, t[i].label, sizeof r->label);
        r->id = t[i].id; r->flags = t[i].flags; r->key = t[i].key;
    }
}

/* Rebuild the bar for whatever window is on top, if that changed or it
 * published new menus. Cheap when nothing changed (two compares), so it is
 * called from wm_sync_focus, after a publish, and once per desktop pass. */
void menu_model_sync(void)
{
    if (g_menu_open >= 0) return;
    int src = g_locked ? -1 : top_visible();
    if (src >= 0 && (!wm_wins[src].used || wm_wins[src].kind != WK_APP)) src = -1;
    unsigned gen = src >= 0 ? wm_wins[src].menus_gen : 0;
    if (src == g_menu_src && gen == g_menu_src_gen) return;
    int had_app = g_menu_src >= 0 && g_menu_src_gen != 0;
    g_nmenu = 0;
    menu_add_kernel(MK_LOGO, "LogitOS", KM_LOGO, (int)(sizeof KM_LOGO / sizeof KM_LOGO[0]));
    const struct mb_set *ms = src >= 0 ? wm_wins[src].menus : 0;
#ifdef WM_MENU_NEGCTL_IGNORE_APP
    ms = 0;                               /* the control: a bar that never shows the app's menus */
#endif
    if (ms) {
        for (int k = 0; k < ms->n; k++) {
            int m = g_nmenu++;
            g_menu_kind[m] = MK_APP;
            scopy(g_menu_title[m], ms->m[k].title, sizeof g_menu_title[m]);
            g_menu_nitems[m] = ms->m[k].n;
            for (int i = 0; i < ms->m[k].n; i++) {
                struct mrow *r = &g_menu_rows[m][i];
                const struct mb_item *it = &ms->m[k].it[i];
                scopy(r->label, it->label, sizeof r->label);
                r->id = it->id; r->flags = it->flags; r->key = it->key;
            }
        }
        menu_add_kernel(MK_WINDOW, "窗口", KM_WINDOW, (int)(sizeof KM_WINDOW / sizeof KM_WINDOW[0]));
    } else {
        menu_add_kernel(MK_FILE, "文件", KM_FILE, (int)(sizeof KM_FILE / sizeof KM_FILE[0]));
        menu_add_kernel(MK_VIEW, "显示", KM_VIEW, (int)(sizeof KM_VIEW / sizeof KM_VIEW[0]));
    }
    int first = g_menu_src == -2;
    g_menu_src = src; g_menu_src_gen = ms ? gen : 0;
    if (first || had_app || ms) {
        /* Only a change of TITLES repaints the bar or re-reports it: focus
         * moving between two apps that both use File/View changes nothing a
         * person can see. */
        g_menu_reported = 0;
        if (!first) wm_damage_menubar();
        kprintf("[wm] menus win=%d n=%d", src, g_nmenu);
        for (int i = 0; i < g_nmenu; i++) kprintf(" %s", g_menu_title[i]);
        kprintf("\n");
    }
}

#define MI_ROW_H S(22)
#define MI_SEP_H S(9)
#define MI_PAD_X S(14)
#define MI_PAD_Y S(6)
#define MI_CHECK_W S(16)                  /* the tick column, only in a menu that has a checked row */
#define MI_KEY_GAP S(28)                  /* label to shortcut, at least */
static int menu_row_h(const struct mrow *it) { return (it->flags & MB_SEP) ? MI_SEP_H : MI_ROW_H; }
static int menu_has_check(int m)
{
    for (int i = 0; i < g_menu_nitems[m]; i++) if (g_menu_rows[m][i].flags & MB_CHECKED) return 1;
    return 0;
}
/* "⌘S" -- the glyph comes from /fonts/text.ttf through the shaper's fallback
 * chain; ui.ttf does not carry U+2318. */
static void menu_key_text(char key, char out[8])
{
    out[0] = (char)0xE2; out[1] = (char)0x8C; out[2] = (char)0x98;
    out[3] = key; out[4] = 0;
}

/* Same discipline as menu_title_hit: the dropdown's geometry is computed once
 * here and read by the painter, the hit-tester and dmg_expand, so none of the
 * three can disagree about where the panel is. */
void menu_dropdown_box(struct drect *r)
{
    if (g_menu_open < 0) { r->x0 = r->y0 = r->x1 = r->y1 = 0; return; }
    const struct mrow *items = g_menu_rows[g_menu_open];
    int n = g_menu_nitems[g_menu_open], maxw = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].flags & MB_SEP) continue;
        int w = fb_text_width(items[i].label);
        if (items[i].key) { char k[8]; menu_key_text(items[i].key, k); w += MI_KEY_GAP + fb_text_width(k); }
        if (w > maxw) maxw = w;
    }
    int panel_w = maxw + 2 * MI_PAD_X + (menu_has_check(g_menu_open) ? MI_CHECK_W : 0);
    if (panel_w < S(170)) panel_w = S(170);
    int panel_h = 2 * MI_PAD_Y;
    for (int i = 0; i < n; i++) panel_h += menu_row_h(&items[i]);
    int x0 = g_menu_tx[g_menu_open] - S(10);
    if (x0 < 0) x0 = 0;
    if (x0 + panel_w > wm_W) x0 = wm_W - panel_w;
    r->x0 = x0; r->y0 = MBH;
    r->x1 = x0 + panel_w; r->y1 = MBH + panel_h;
}
/* wm_glass_regions' provider: the open dropdown, if any. */
int wm_menu_glass_rect(struct wm_glass_region *out, int max)
{
    if (g_menu_open < 0 || max < 1) return 0;
    menu_dropdown_box(&out->r);
    out->radius = S(DROPDOWN_GLASS_PT);
    return 1;
}

/* The dropdown's drop shadow, and the box it paints: the panel grown by the
 * blur on every side and by the offset below. Damage for opening, switching
 * and closing a menu is THIS box, not menu_dropdown_box() -- the panel alone
 * left each closed menu's shadow on the desktop as a dark streak under its
 * title (reported from `make run`, 2026-10-26). Same rule as sw_box(). */
#define MENU_SH_DY   0
#define MENU_SH_BLUR wm_sh_ext(WMSH_MENU)
static void dirty_menu_dropdown(void)
{
    struct drect p; menu_dropdown_box(&p);
    if (p.x1 <= p.x0) return;
#ifdef WM_MENU_SHADOW_NEGCTL
    dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);   /* the control: the panel only */
#else
    dirty_rect(p.x0 - MENU_SH_BLUR, p.y0 - MENU_SH_BLUR + MENU_SH_DY,
               p.x1 - p.x0 + 2 * MENU_SH_BLUR, p.y1 - p.y0 + 2 * MENU_SH_BLUR);
#endif
}

/* One report, one call site each for "just opened" and "just switched" (see
 * both callers below) -- same "ask the guest, don't re-derive its geometry in
 * Python" reasoning as menu_bar_layout()'s report above: the panel's exact
 * pixel box depends on font metrics a test driver has no business computing.
 * Each row follows, so a driver can click "Select All" by name. */
static void menu_report_open(void)
{
    struct drect p; menu_dropdown_box(&p);
    kprintf("[wm] menu open %d x0 %d y0 %d x1 %d y1 %d\n", g_menu_open, p.x0, p.y0, p.x1, p.y1);
    int y = p.y0 + MI_PAD_Y;
    for (int i = 0; i < g_menu_nitems[g_menu_open]; i++) {
        const struct mrow *it = &g_menu_rows[g_menu_open][i];
        int h = menu_row_h(it);
        if (!(it->flags & MB_SEP))
            kprintf("[wm] menu row %d '%s' y0 %d y1 %d\n", i, it->label, y, y + h);
        y += h;
    }
}

/* The item under (px,py) in menu `mi`, or -1 (outside the panel, or a
 * separator). `*out_row` (if given) comes back as that row's own rect, which
 * is what the hover highlight paints. */
static int menu_item_at(int mi, int px, int py, struct drect *out_row)
{
    if (mi < 0 || mi != g_menu_open) return -1;
    struct drect p; menu_dropdown_box(&p);
    if (px < p.x0 || px >= p.x1 || py < p.y0 || py >= p.y1) return -1;
    int y = p.y0 + MI_PAD_Y;
    const struct mrow *items = g_menu_rows[mi];
    int n = g_menu_nitems[mi];
    for (int i = 0; i < n; i++) {
        int h = menu_row_h(&items[i]);
        if (!(items[i].flags & MB_SEP) && py >= y && py < y + h) {
            if (out_row) { out_row->x0 = p.x0; out_row->x1 = p.x1; out_row->y0 = y; out_row->y1 = y + h; }
            return i;
        }
        y += h;
    }
    return -1;
}

/* Close and Minimize need a real focused app window; an app's own item is
 * enabled unless it said otherwise; everything else is always available
 * (Expose is claimed even with zero windows -- see wm_shortcut). */
static int menu_item_enabled(int mi, int idx)
{
    const struct mrow *r = &g_menu_rows[mi][idx];
    if (r->flags & (MB_SEP | MB_DISABLED)) return 0;
    int k = g_menu_kind[mi];
    if ((k == MK_FILE || k == MK_WINDOW) && r->id <= 1) {
        int wi = top_visible();
        return wi >= 0 && wm_wins[wi].used && wm_wins[wi].kind == WK_APP;
    }
    return 1;
}

void menu_close(void)
{
    if (g_menu_open < 0) return;
    dirty_menu_dropdown();
    g_menu_open = -1; g_menu_item_hov = -1;
    kprintf("[wm] menu closed\n");        /* a test syncs on this rather than guessing a delay */
}

/* An app's item, chosen: EV_MENU to the window that published it -- if it is
 * still that window. The slot can have been reaped and reused since the model
 * was built; menus_gen is never reused, so it tells the two tenants apart. */
static void menu_send_app(int mi, int idx, const char *how)
{
    int wi = g_menu_src;
    if (wi < 0 || !wm_wins[wi].used || wm_wins[wi].kind != WK_APP ||
        !wm_wins[wi].menus || wm_wins[wi].menus_gen != g_menu_src_gen) return;
    const struct mrow *r = &g_menu_rows[mi][idx];
    kprintf("[wm] menu %s win=%d id=%d '%s'\n", how, wi, r->id, r->label);
#ifndef WM_MENU_NEGCTL_DROP_EVENT
    wm_enqueue(&wm_wins[wi], EV_MENU, r->id, mi - 1);   /* b: the menu's index in the app's blob */
#endif
}

/* The one place a menu item's action actually happens. Closes the menu on
 * every reachable path -- a fired action always dismisses, exactly like a
 * real menu -- so no caller has to remember to. */
static void menu_item_fire(int mi, int idx)
{
    int kind = g_menu_kind[mi], id = g_menu_rows[mi][idx].id;
    if (kind == MK_LOGO) {
        switch (id) {
        case 0: menu_close(); overlay_open(OV_ABOUT); return;
        case 1: menu_close(); wm_launch("settings.aex", ""); return;
        case 3: menu_close(); overlay_open(OV_CONFIRM_SHUTDOWN); return;
        case 4: menu_close(); overlay_open(OV_CONFIRM_RESTART); return;
        default: menu_close(); return;                    /* the separator: unreachable, but safe */
        }
    } else if (kind == MK_APP) {
        menu_close();
        menu_send_app(mi, idx, "fire");
    } else {                                               /* File, View, Window */
        int wi = top_visible();
        menu_close();
        if (id == 2) { ex_enter(); return; }
        if (wi < 0 || !wm_wins[wi].used || wm_wins[wi].kind != WK_APP) return;
        struct win *w = &wm_wins[wi];
        if (id == 0) wm_enqueue(w, EV_CLOSE, 0, 0);           /* same event the traffic light sends */
        else if (id == 1) win_set_min(w, 1);
    }
}

/* Cmd+<key> with no system meaning, and the top window published an item
 * with that shortcut: the same EV_MENU a click on it sends. This is what makes
 * the hint drawn beside the label TRUE -- an app's shortcuts are the ones in
 * its menus, and the app handles each command once, as a menu id. */
int menu_key(int c)
{
    menu_model_sync();
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    for (int m = 0; m < g_nmenu; m++) {
        if (g_menu_kind[m] != MK_APP) continue;
        for (int i = 0; i < g_menu_nitems[m]; i++) {
            const struct mrow *r = &g_menu_rows[m][i];
            int k = r->key;
            if (k >= 'a' && k <= 'z') k = k - 'a' + 'A';
            if (!k || k != c || !menu_item_enabled(m, i)) continue;
            menu_send_app(m, i, "key");
            return 1;
        }
    }
    return 0;
}

/* Per-pass idle work for the menus: none (menu_model_sync runs every pass on
 * its own, in wm_run). */
int wm_menu_tick(uint64_t now) { (void)now; return 0; }

/* The title under (x,y), or -1. */
int menu_title_at(int x, int y)
{
    for (int i = 0; i < g_nmenu; i++) {
        struct drect t; menu_title_hit(i, &t);
        if (in_rect(x, y, t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0)) return i;
    }
    return -1;
}

/* A CLOSED title, clicked (wm_menubar_click): open its menu. */
void menu_open(int i)
{
    g_menu_open = i; g_menu_item_hov = -1;
    dirty_menu_dropdown();
    menu_report_open();
}

/* The title that was open at the START of the event being handled: set by
 * wm_menu_pointer, read by wm_menu_click, which the capture always calls
 * after it within the same event (wm.c: wm_menu_capture). */
static int g_menu_was_open = -1;

/* The open menu's share of the pointer while it owns it (wm.c's
 * wm_menu_capture does the rest), on every event, moved or not. */
int wm_menu_pointer(int x, int y)
{
    g_menu_was_open = g_menu_open;
    /* Hovering a DIFFERENT title switches menus live -- the detail that makes
     * a menu bar read as one system instead of four separate buttons. */
    int hit_title = menu_title_at(x, y);
    if (hit_title >= 0 && hit_title != g_menu_open) {
        dirty_menu_dropdown();                    /* the old panel and its shadow */
        g_menu_open = hit_title; g_menu_item_hov = -1;
        dirty_menu_dropdown();                    /* the new one */
        menu_report_open();   /* hover switch */
    }

    struct drect row;
    int hov = menu_item_at(g_menu_open, x, y, &row);
    if (hov != g_menu_item_hov) {
        g_menu_item_hov = hov;
        struct drect p; menu_dropdown_box(&p);      /* whole panel -- it is small, see file header */
        dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
    }
    return 1;
}

/* A press while a menu is open: a title (the open one toggles off; another one
 * the hover switch already opened), a live item, or "away". */
int wm_menu_click(int x, int y, int btn)
{
    (void)btn;
    int hit_title = menu_title_at(x, y);
    if (hit_title >= 0 && hit_title == g_menu_was_open) {
        menu_close();                            /* the OPEN title, clicked again: toggle off */
    } else if (hit_title >= 0) {
        /* a different title: the hover switch already did the work */
    } else {
        int idx = menu_item_at(g_menu_open, x, y, NULL);
        if (idx >= 0 && menu_item_enabled(g_menu_open, idx)) menu_item_fire(g_menu_open, idx);
        else menu_close();                       /* not a title, not a live item: dismiss */
    }
    return 1;
}

void draw_menu_dropdown(void)
{
    struct drect p; menu_dropdown_box(&p);
    int w = p.x1 - p.x0, h = p.y1 - p.y0;
    wm_cast_shadow(WMSH_MENU, p.x0, p.y0, w, h, S(10), 256, 255);
    wm_glass_panel(WM_MAT_MENU, p.x0, p.y0, w, h, DROPDOWN_GLASS_PT, 0, WM_GLASS_ALPHA_KIND);

    unsigned ac = settings_get_color("ui.accent", 0x5E96FF);
    uint8_t ar = (uint8_t)(ac >> 16), ag = (uint8_t)(ac >> 8), ab = (uint8_t)ac;

    const struct mrow *items = g_menu_rows[g_menu_open];
    int n = g_menu_nitems[g_menu_open];
    int lx = p.x0 + MI_PAD_X + (menu_has_check(g_menu_open) ? MI_CHECK_W : 0);
    int y = p.y0 + MI_PAD_Y;
    for (int i = 0; i < n; i++) {
        int rh = menu_row_h(&items[i]);
        if (items[i].flags & MB_SEP) {
            fb_fill_rect(p.x0 + S(8), y + rh / 2, w - S(16), S(1),
                         g_ui_dark ? rgb(70, 70, 80) : rgb(214, 214, 220));
        } else {
            int enabled = menu_item_enabled(g_menu_open, i);
            int hov = enabled && i == g_menu_item_hov;
            if (hov) fb_blend_round_rect(p.x0 + S(4), y, w - S(8), rh, S(6), ar, ag, ab, 200);
            uint32_t ink;
            if (!enabled) ink = g_ui_dark ? rgb(112, 112, 120) : rgb(180, 180, 186);
            else if (hov)  ink = rgb(255, 255, 255);
            else           ink = g_ui_dark ? rgb(228, 229, 235) : rgb(40, 40, 48);
            if (items[i].flags & MB_CHECKED) fb_text(p.x0 + MI_PAD_X - S(2), y + S(3), "\xe2\x9c\x93", ink);
            fb_text(lx, y + S(3), items[i].label, ink);
            if (items[i].key) {
                char k[8]; menu_key_text(items[i].key, k);
                uint32_t kink = hov ? ink : (g_ui_dark ? rgb(150, 150, 160) : rgb(130, 130, 138));
                if (!enabled) kink = ink;
                fb_text(p.x1 - MI_PAD_X - fb_text_width(k), y + S(3), k, kink);
            }
        }
        y += rh;
    }
}
