/* wm_dock.c -- the dock and the app registry.
 *
 * The dock: its geometry (every dock box derives from dock_geom), the glass
 * slab and shadow, the tiles, hover magnification and tooltip, running dots,
 * the launch bounce, autohide -- and the app registry it shows (scan_apps,
 * the `[wm] dock` line, the file associations), so the dock's order is
 * decided in one file.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

static int dock_slide_px(void);
static int dock_hover_at(int x, int y);

/* app registry built by scanning the disk for *.aex
 *
 * `hidden` is AEX_CAT_TEST, and only that -- "built for a harness, not for a
 * person" (aex.h). An app tagged this way still gets a slot (scan_apps still
 * counts it into nreg) and is still PAINTED (draw_dock does not consult
 * `hidden` at all): dock_geom() centres the whole strip on nreg and every
 * other icon's x is `slot * (isz + gap)` from that centre, so on a disk that
 * packs one, dropping the slot -- or leaving a blank gap where it was --
 * would shift every OTHER icon's position. What `hidden` removes is
 * INTERACTION: dock_hover_at() will never return this slot (no magnify, no
 * tooltip) and the click test below will never launch it. A tile nothing can
 * hover or click is exactly as unlaunchable as a missing one, without moving
 * a pixel anything else measures.
 *
 * STATUS, DATED. This used to matter on $(DISK) itself: until 2026-09-02
 * Gallery shipped AEX_CAT_TEST specifically so BROWSER_SLOT/GALLERY_SLOT/
 * SETTINGS_SLOT (three hand-maintained constants that lived in qmp_ui.py at
 * the time) would not have to move and qmp_desktop_look.py's dock_pitch/rim
 * checks -- which back then counted a hardcoded ELEVEN evenly spaced, filled
 * tiles -- would not read a blanked slot as a structural defect. Both of
 * those reasons are gone now: qmp_ui.py derives every slot and the app count
 * from THIS file's own dock_publish() line rather than restating them, and
 * qmp_desktop_look.py counts whatever that line says (DOCK_N) rather than a
 * constant. So Task E's Widgets and Gallery removal (see the APPS/APP_RULE
 * comments in the Makefile) could just DROP both apps from the pack list
 * outright, and did -- the product disk carries no AEX_CAT_TEST app and no
 * `hidden` token at all today. This field and the mechanism above it are kept
 * because they are still correct, general machinery: anything that DOES want
 * a Dock tile a harness can see but nothing can click still has a one-line
 * way to ask for it (mkaex.py --category test), unrelated to whether it is
 * also on $(DISK) by default. */
struct regent { char file[48], name[32], ext[8]; char icon; uint32_t color; int hidden; };
static struct regent reg[MAXWIN];
static int nreg;
static uint64_t reg_bounce[MAXWIN];    /* monotonic ms a launch bounce started (0 = none) */

/* Which dock icon this window's app owns. Matched by the aex header NAME, the
 * same key wm_launch's single-instance branch and the running-dot indicator
 * both use -- not by file path, which reg[] and apps[] spell differently.
 * A window with no dock icon (none exists today, but a WK_FINDER or a future
 * app launched by path would be one) flies to the middle of the dock rather
 * than to slot 0, which reads as "into the dock" instead of as "into Clock". */
int dock_slot_for(const struct win *w)
{
    if (w->app)
        for (int i = 0; i < nreg; i++)
            if (streq(reg[i].name, w->app->name)) return i;
    return nreg > 0 ? nreg / 2 : 0;
}

/* Points; DOCK_ISZ/DOCK_GAP are the device-pixel values draw_dock() derives from
 * them each frame and the click handler tests against. */
#define DOCK_ISZ_PT 50
#define DOCK_GAP_PT 14
/* Vertical breathing room, top AND bottom, between an icon and the panel edge.
 * RETUNED (unit F): this used to be a bare `S(20)` / `S(10)` split three ways
 * across dock_geom, dock_hover_at and the click hit-test in the mouse handler
 * below, with no name tying them together -- exactly the trap the WSH_ and
 * DOCKSH_ single-definition macros elsewhere in this file exist to avoid, and
 * the reason the icons visibly crowded the top/bottom edges was that all
 * three copies agreed with EACH OTHER, just not with what "enough padding"
 * should have been. One constant: isz=50 in a dh=isz+2*DOCK_PAD_PT panel now
 * leaves 18pt each side instead of 10 (a panel 50/86=58% icon by height
 * instead of 50/70=71%), close to a real macOS dock's proportion instead of
 * the icons nearly touching the rim. */
#define DOCK_PAD_PT 18
static int dock_x0, dock_y0, dock_isz = DOCK_ISZ_PT, dock_gap = DOCK_GAP_PT;

/* The dock's geometry, derived rather than remembered.
 *
 * draw_dock used to be the only thing that knew where the dock was, and it
 * published its answer into the globals above on the way past. That is fine
 * while the dock is drawn every frame; it stops being fine the moment a frame
 * may skip it, because the DAMAGE path has to know where the dock is in order
 * to decide whether this frame touches it -- and asking a global that the
 * skipped draw would have written is a loop. One function computes it. */

/* Where the dock is DRAWN, hit and damaged: its resting place, lowered by the
 * autohide slide (dock_slide_px, 0 while shown). Every dock box in this file
 * derives from here, so the picture, the click and the damage move together. */
void dock_geom(int *x0, int *y0, int *dw, int *dh)
{
    int n = nreg < 1 ? 1 : nreg;
    int isz = S(DOCK_ISZ_PT), gap = S(DOCK_GAP_PT);
    *dw = gap + n * (isz + gap);
    *dh = isz + 2 * S(DOCK_PAD_PT);
    *x0 = (wm_W - *dw) / 2;
    *y0 = wm_H - *dh - S(12) + dock_slide_px();
}

/* ONE dock icon's tile, in device pixels -- the target a minimised window flies
 * into and out of.
 *
 * Derived from dock_geom() and S(DOCK_PAD_PT), which is the same arithmetic
 * draw_dock() places the icon with and the same the click hit-test uses. That
 * is not tidiness: this file has already been bitten once by three copies of
 * the dock's vertical padding agreeing with each other and not with the
 * drawing (see DOCK_PAD_PT's comment), and a fly that lands next to the icon
 * instead of on it is that bug in its most visible possible form. */
void dock_icon_box(int slot, struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    (void)dw; (void)dh;
    int isz = S(DOCK_ISZ_PT), gap = S(DOCK_GAP_PT);
    if (slot < 0) slot = 0;
    if (nreg > 0 && slot >= nreg) slot = nreg - 1;
    r->x0 = x0 + gap + slot * (isz + gap);
    r->y0 = y0 + S(DOCK_PAD_PT);
    r->x1 = r->x0 + isz;
    r->y1 = r->y0 + isz;
}

/* The GLASS PANEL: the rounded slab whose frost samples the live backdrop. This
 * is the rectangle a damage rectangle may not cut in half (see dmg_expand); the
 * rest of the dock's footprint is ordinary read-modify-write drawing that clips
 * exactly and needs no such promise. */
/* Same single-definition rule as WSH_* above: the dock's shadow is painted in
 * draw_dock and its extent is declared here, and if the two disagree a damage
 * rectangle clips the shadow in half. */
#define DOCKSH_DY   0                       /* wm_shadow_extent includes the offset */
#define DOCKSH_BLUR wm_sh_ext(WMSH_DOCK)

static void dock_panel_box(struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    int b = DOCKSH_BLUR;
    r->x0 = x0 - b;          r->y0 = y0 - b;
    r->x1 = x0 + dw + b;     r->y1 = y0 + dh + b + DOCKSH_DY;   /* + the drop shadow */
}
/* wm_glass_regions' provider: the dock's SLAB (with its shadow, as above),
 * not its whole footprint. */
int wm_dock_glass_rect(struct wm_glass_region *out, int max)
{
    if (max < 1) return 0;
    dock_panel_box(&out->r);
    out->radius = S(DOCK_GLASS_PT);
    return 1;
}

/* The dock's whole footprint, for a given hovered icon (-1 = none): the panel,
 * plus the launch bounce that lifts an icon S(14) out of the top, plus the
 * magnified hover tile, plus the tooltip above it.
 *
 * The tooltip is centred on the icon and as wide as the app's NAME, so it can
 * overhang the panel -- but its extent is EXACTLY KNOWABLE, and asking for it
 * is worth the four lines. The first version damaged the full screen width to
 * cover it, and because the Terminal's window reaches down into the dock, every
 * repaint of that window then grew to a full-width band: 82% of the screen for
 * a keystroke. Being vague about damage is not free; it is paid for by every
 * unrelated rectangle that happens to touch the vague one. */
static void dock_box_hov(int hov, struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    dock_panel_box(r);
    if (y0 - S(16) < r->y0) r->y0 = y0 - S(16);        /* bounce + 1.3x hover tile */
    if (hov >= 0 && hov < nreg) {
        int isz = S(DOCK_ISZ_PT), gap = S(DOCK_GAP_PT);
        int ccx = x0 + gap + hov * (isz + gap) + isz / 2;
        int tw = fb_text_width(reg[hov].name);
        struct drect t = { ccx - tw / 2 - S(10), y0 - S(30),
                           ccx + tw / 2 + tw % 2 + S(10), y0 };
        rect_or(r, &t);
    }
}
void dock_box(struct drect *r) { dock_box_hov(dock_hover_at(wm_pointer_x(), wm_pointer_y()), r); }

/* ---- DOCK AUTOHIDE (2026-10-26) ---------------------------------------------
 *
 * While the frontmost window is zoomed the dock slides below the bottom edge and
 * the zoom reaches the bottom of the screen (zoom_full_rect). A zoomed window
 * used to stop above the dock, leaving a band of wallpaper under a full-width
 * window with the dock floating in it -- "especially after full screen, very
 * awkward", from the owner's own desktop. Touching the bottom edge brings the
 * dock back OVER the window; it stays while the pointer is on it (or its
 * tooltip) and leaves when the pointer does. Any other state shows it. */
#define DOCK_SLIDE_MS 220
static int g_dock_hide_to;            /* the target: 0 shown, 256 hidden       */
static int g_dock_hide_from;          /* progress when the current slide began */
static uint64_t g_dock_hide_t0;       /* the slide's start; 0 = at rest        */

static int dock_hide_progress(void)
{
    if (!g_dock_hide_t0) return g_dock_hide_to;
    uint64_t e = anim_elapsed(g_dock_hide_t0);
    if (wm_reduce_motion || e >= DOCK_SLIDE_MS) return g_dock_hide_to;
    int k = ease_out((int)(e * 256 / DOCK_SLIDE_MS));
    return g_dock_hide_from + (g_dock_hide_to - g_dock_hide_from) * k / 256;
}

/* Far enough that the panel AND its shadow (DOCKSH_DY + DOCKSH_BLUR) clear the
 * bottom edge when hidden. */
static int dock_slide_px(void)
{
    int dh = S(DOCK_ISZ_PT) + 2 * S(DOCK_PAD_PT);
    return dock_hide_progress() * (dh + S(12) + DOCKSH_DY + DOCKSH_BLUR + S(2)) / 256;
}

/* The band the dock can occupy anywhere along its slide, tooltip included. */
static void dirty_dock_band(void)
{
    int dh = S(DOCK_ISZ_PT) + 2 * S(DOCK_PAD_PT);
    int top = wm_H - dh - S(12) - S(30);
    dirty_rect(0, top, wm_W, wm_H - top);
}

static void wm_dock_autohide_tick(void)
{
    int top = g_locked ? -1 : top_visible();
    int want = 0;
    if (top >= 0 && wm_wins[top].used && wm_wins[top].kind == WK_APP &&
        wm_wins[top].zoomed && !wm_wins[top].minimized && !ex_on) {
        int dw = 0, x0, dh = S(DOCK_ISZ_PT) + 2 * S(DOCK_PAD_PT);
        { int n = nreg < 1 ? 1 : nreg; dw = S(DOCK_GAP_PT) + n * (S(DOCK_ISZ_PT) + S(DOCK_GAP_PT)); }
        x0 = (wm_W - dw) / 2;
        int rest_y0 = wm_H - dh - S(12);
        int reveal = wm_pointer_y() >= wm_H - S(3);
        int on_it = g_dock_hide_to == 0 && wm_pointer_y() >= rest_y0 - S(30) &&
                    wm_pointer_x() >= x0 - DOCKSH_BLUR && wm_pointer_x() < x0 + dw + DOCKSH_BLUR;
        want = !(reveal || on_it);
    }
    int to = want ? 256 : 0;
    if (to != g_dock_hide_to) {
        g_dock_hide_from = dock_hide_progress();
        g_dock_hide_to = to;
        g_dock_hide_t0 = anim_stamp();
        kprintf("[wm] dock %s\n", want ? "hide" : "show");
    }
    if (g_dock_hide_t0) {
        dirty_dock_band();
        if (wm_reduce_motion || anim_elapsed(g_dock_hide_t0) >= DOCK_SLIDE_MS) g_dock_hide_t0 = 0;
    }
}

/* Damage the dock as it is about to be drawn. A hover CHANGE has to damage the
 * outgoing tooltip as well -- the icon the pointer just left still has one on
 * screen, and it is nowhere near the icon it moved to. */
static void dirty_dock_hov(int hov)
{ struct drect r; dock_box_hov(hov, &r); dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0); }
void dirty_dock(void) { dirty_dock_hov(dock_hover_at(wm_pointer_x(), wm_pointer_y())); }

/* Draw one dock tile of side `sz` centered at (cx,cy): glossy gradient + sheen +
 * the app's vector icon (or its letter). Used at base size and, for the hovered
 * icon, magnified -- the corner radius and icon scale track `sz`. */
void dock_tile(int i, int cx, int cy, int sz)
{
    int x = cx - sz / 2, y = cy - sz / 2, rad = sz * 12 / 50;
    /* Raster icons contain their own tile and alpha corners; painting the old
     * coloured square underneath would put a second, mismatched frame on them. */
    if (app_icon_draw(reg[i].file, x, y, sz)) return;
    fb_round_rect_vgrad(x, y, sz, sz, rad, fb_shade(reg[i].color, 38), fb_shade(reg[i].color, -26));
    fb_blend_round_rect(x, y, sz, sz / 2, rad, 255, 255, 255, 32);
    int ic = icon_for_app(reg[i].file, reg[i].ext);
    if (ic >= 0) { int isz = sz * 62 / 100; icon_draw(ic, cx - isz / 2, cy - isz / 2, isz, rgb(255, 255, 255)); }
    else { char ch[2] = { reg[i].icon, 0 }; fb_text(cx - S(FW) / 2, cy - S(FH) / 2, ch, rgb(255, 255, 255)); }
}

/* Upward pixel offset of dock icon `i`'s launch bounce (two decaying parabolic
 * hops over ~0.55s). Clears the timer when finished. Integer-only. */
static int dock_bounce_off(int i)
{
    uint64_t t0 = reg_bounce[i];
    if (!t0) return 0;
    uint64_t e = anim_elapsed(t0), DUR = 550, half = DUR / 2;
    if (wm_reduce_motion || !wm_desktop_prefs.dock_bounce || e >= DUR) { reg_bounce[i] = 0; return 0; }
    int hop = (e < half) ? 0 : 1, H = S(hop ? 7 : 14);
    int u = (int)(e - (uint64_t)hop * half), d = (int)half;   /* 0..d within the arc */
    return H * 4 * u * (d - u) / (d * d);                      /* parabola, peak mid-arc */
}

/* Which dock icon a device-pixel point hovers, or -1.
 *
 * Split out of draw_dock because the INPUT path has to ask it too. With the
 * pointer on a display plane, plain motion no longer repaints anything -- but
 * the dock magnifies the icon under the cursor, so the one kind of motion that
 * genuinely does change the picture is motion that crosses an icon boundary.
 * The compositor and the input path have to agree about where those boundaries
 * are, and the way to guarantee that is to have one function say so.
 *
 * Valid only after the first draw_dock() has published dock_x0/dock_y0/isz/gap;
 * before that they are the point-valued defaults and this returns -1 for the
 * whole screen, which is correct -- there is no dock on screen yet either. */
static int dock_hover_at(int x, int y)
{
    int dh = dock_isz + 2 * S(DOCK_PAD_PT);
    if (y < dock_y0 || y >= dock_y0 + dh) return -1;
    for (int i = 0; i < nreg; i++) {
        if (reg[i].hidden) continue;   /* no hover, no tooltip, no magnify -- see struct regent */
        int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap);
        if (x >= ix && x < ix + dock_isz) return i;
    }
    return -1;
}

void draw_dock(void)
{
    int dw, dh;
    dock_isz = S(DOCK_ISZ_PT); dock_gap = S(DOCK_GAP_PT);   /* device px, for the click path */
    dock_geom(&dock_x0, &dock_y0, &dw, &dh);
    if (dock_y0 >= wm_H) return;             /* autohidden: entirely below the screen */
    /* Called "soft drop shadow" and it was neither: a solid black rounded slab
     * at a flat alpha 50, offset down S(7), with no falloff anywhere -- a hard
     * edge all the way round, under a panel translucent enough to show it. */
    wm_cast_shadow(WMSH_DOCK, dock_x0, dock_y0, dw, dh, S(28), 256, 255);
    /* Liquid Glass: frost + rim refraction + specular highlight + body tint.
     * WM_MAT_DOCK's light tint is (200,202,216,92) and its dark (26,26,34,104).
     * Light: not white. A white tint is "light glass" to the material's adaptive
     * tint (glass_shading.h prepare_style), which raises opacity until the
     * result is at least luma 176 so dark text on it stays legible. The menu
     * bar needs that; the dock carries no text. Over the release wallpaper's
     * dark foreground (37,33,73) white/44 came out an opaque grey slab
     * (180,177,191) -- release tour, 2026-10-26. A neutral tint of luma ~203
     * is outside the adaptive range by the material's own rule (a tone between
     * 64 and 224 is left alone), so the dock is translucent over a dark
     * wallpaper and a faint grey lift over a light one. */
    wm_glass_panel(WM_MAT_DOCK, dock_x0, dock_y0, dw, dh, DOCK_GLASS_PT, 0, WM_GLASS_ALPHA_KIND);

    /* Live hover magnification: the icon under the cursor grows in place (kept
     * inside the panel + gap so it never overlaps a neighbour) and shows its name
     * as a tooltip. Re-evaluated every frame; the input path asks dock_hover_at()
     * the same question and requests a frame when the answer changes, which is
     * what still animates this as the cursor sweeps the dock now that plain
     * motion no longer repaints anything. */
    int ccy = dock_y0 + S(DOCK_PAD_PT) + dock_isz / 2, animating = 0;
    int hov = dock_hover_at(wm_pointer_x(), wm_pointer_y());
    for (int i = 0; i < nreg; i++) {
        if (i == hov) continue;                            /* hovered tile drawn last, on top */
        /* Hidden (AEX_CAT_TEST) tiles are still PAINTED, deliberately -- only
         * dock_hover_at() and the click test below treat them differently.
         * Blanking the tile would have been the more literal "not launchable",
         * but tests/qmp/qmp_desktop_look.py's dock_pitch()/rim checks measure
         * the row as eleven filled, evenly-pitched icons and would read a
         * blank slot as a structural asymmetry that has nothing to do with
         * glass. A tile nothing can hover, tooltip or click is exactly as
         * unlaunchable as a missing one, at zero risk to that gate. */
        int b = dock_bounce_off(i); if (b) animating = 1;  /* launch bounce lifts the icon */
        int ccx = dock_x0 + dock_gap + i * (dock_isz + dock_gap) + dock_isz / 2;
        dock_tile(i, ccx, ccy - b, dock_isz);
    }
    /* RUNNING-APP INDICATOR: a small dot under each app that has a live
     * process -- the cheapest recognisable macOS dock trait there is, and the
     * WM already has the answer for free (find_live_app is the exact check
     * wm_launch makes to decide "focus the existing window" vs. "start a new
     * one", keyed the same way: by the aex header NAME reg[] and apps[] both
     * carry, not by file path). Anchored to the panel's own bottom edge and
     * the BASE (non-magnified, non-bounced) icon column -- not `ccy - b` and
     * not the 1.3x hover size -- so the dot sits still while the icon above
     * it bounces or magnifies.
     *
     * DRAWN BEFORE THE MAGNIFIED HOVER TILE, and the order is the fix for a
     * measured overlap: the enlarged tile's bottom edge (ccy + 0.65*isz)
     * clears the dot row (dh - S(8)) by only S(8) - 0.15*isz -- under one
     * point at today's icon size, negative past isz = 53pt. An earlier
     * version drew the dots last and CLAIMED 2.5pt of clearance in this very
     * comment; the adversarial pass measured the dot punched into the icon.
     * Painting the icon over the dot instead makes the near-miss harmless at
     * every icon size, and matches what the eye expects: the icon in front,
     * the indicator behind it. */
    for (int i = 0; i < nreg; i++) {
        if (!wm_desktop_prefs.dock_dots || !find_live_app(reg[i].name)) continue;
        int ccx = dock_x0 + dock_gap + i * (dock_isz + dock_gap) + dock_isz / 2;
        fb_fill_circle(ccx, dock_y0 + dh - S(8), S(2), g_ui_dark ? rgb(235, 236, 240) : rgb(58, 58, 64));
    }
    if (hov >= 0) {
        int b = dock_bounce_off(hov); if (b) animating = 1;
        int ccx = dock_x0 + dock_gap + hov * (dock_isz + dock_gap) + dock_isz / 2;
        dock_tile(hov, ccx, ccy - b, dock_isz * (wm_desktop_prefs.dock_magnify ? 130 : 100) / 100);
        const char *nm = reg[hov].name;                    /* tooltip above the dock */
        int tw = fb_text_width(nm), tx = ccx - tw / 2;     /* already device px (scaled font) */
        fb_blend_round_rect(tx - S(9), dock_y0 - S(28), tw + S(18), S(23), S(7), 28, 28, 34, 225);
        fb_text(tx, dock_y0 - S(25), nm, rgb(244, 244, 248));
    }
    if (animating) dirty_dock();                           /* keep compositing while a bounce runs */
}

/* ---- the dock's input -------------------------------------------------------
 * A press on an icon launches its app. The hit box is the icon tile exactly:
 * iy used to be the bare literal `+ 10` -- correct only because it happened
 * to equal the old S(10) top padding at 1x scale, and silently wrong (a dead
 * icon-height strip at the top of the hit box, at scale > 100%) the day
 * either drifted from the other. S(DOCK_PAD_PT) is the same padding
 * draw_dock() actually placed the icon with, so a click and the pixel it
 * lands on agree at every backing scale, not just this one. Returns 1 when
 * an icon took the press. */
int wm_dock_click(int x, int y, int btn)
{
    if (btn != EV_BTN_LEFT) return 0;
    for (int i = 0; i < nreg; i++) {
        if (reg[i].hidden) continue;   /* AEX_CAT_TEST: painted, but no hit box and no launch */
        int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap), iy = dock_y0 + S(DOCK_PAD_PT);
        if (in_rect(x, y, ix, iy, dock_isz, dock_isz)) {
            wm_launch(reg[i].file, "");
            dirty_dock();            /* the launch bounce starts in the dock */
            return 1;
        }
    }
    return 0;
}

/* The pointer moved from (ox,oy) to (x,y): the one kind of motion that
 * changes the dock's picture is motion that crosses an icon boundary, and it
 * is asked for by name -- the icon being left and the icon being entered.
 * Returns 1 when the hover changed. */
int wm_dock_pointer(int x, int y, int ox, int oy)
{
    int old_hov = dock_hover_at(ox, oy);
    int hov = dock_hover_at(x, y);
    if (hov == old_hov) return 0;
    dirty_dock_hov(old_hov);
    dirty_dock_hov(hov);
    return 1;
}

/* ---------- the app registry ---------- */

/* WHERE THIS MACHINE'S APPS ARE, said out loud, exactly once.
 *
 * The dock's slot order is `reg[]`, built by scan_apps() below off the disk it
 * booted from, and until this line existed the machine never published it.
 * Ten places outside the kernel spelled it instead -- NAPPS, BROWSER_SLOT,
 * GALLERY_SLOT, SETTINGS_SLOT and a name->index dict in qmp_launch_click.py,
 * plus a scatter of bare integers -- which is CLAUDE.md rule 3 (one jar, two
 * doors) at ten doors, and it has already gone wrong the way that rule
 * predicts: NAPPS went stale the day a settings app was packed (10 -> 11).
 * The dock is CENTRED, so one more app moves EVERY icon half a slot; a stale
 * coordinate lands on the wallpaper, which does nothing at all and looks
 * exactly like the app failing to open.
 *
 * A constant maintained in step with a disk image by hand is not a fact, it is
 * a hope. This is the fact, printed by the ONE function that builds the
 * registry -- called from inside scan_apps(), not after it, so a second caller
 * cannot acquire a dock the outside world was never told about.
 *
 * THE FORMAT IS CHOSEN FOR THE READER, and the reader is Python in
 * tests/qmp/ (qmp_ui.py:parse_dock -- keep the two in step):
 *
 *   [wm] dock 9 apps: 0=clock.aex,shown 1=textedit.aex,shown ... 8=settings.aex,shown
 *
 * one line; one whitespace-separated token per slot; each token is
 * `slot=file,visibility` in that fixed order; no field can contain a space
 * (the file is a LogitFS root name, and the visibility is one of exactly two
 * words). `shown`/`hidden` is spelled rather than 0/1 because the line is read
 * by people on a serial log as well as by a parser, and because it answers a
 * question a driver otherwise diagnoses as a slow launch: an AEX_CAT_TEST tile
 * is painted but can never be hovered or clicked, so a click on it does
 * nothing, forever, and looks identical to an app that is merely slow.
 * (Nothing ships AEX_CAT_TEST on $(DISK) as of Task E, 2026-09-02 -- see
 * struct regent's comment below -- so `hidden` is expected to read `shown` on
 * every token of the product's own line; the field stays general for any
 * disk that packs one.) */
static void dock_publish(void)
{
#ifdef DOCK_NEGCTL_PUBLISH_STALE
    /* NEGATIVE CONTROL for test-dock-map (tests/dock.mk: test-dock-map-negctl,
     * driven by tests/qmp/qmp_dock_negctl.py). Print ONE APP FEWER than nreg
     * actually holds: the header number and the token count still agree with
     * EACH OTHER (n, not nreg), so the line parses as a perfectly ordinary,
     * self-consistent dock -- it is simply short by exactly the kind of lie a
     * hand-maintained NAPPS used to tell. scan_apps() is untouched, so the
     * dropped app is still in reg[], still drawn by draw_dock() at its real,
     * nreg-wide centred position, and still fully clickable -- only the
     * REPORT is wrong. A harness that clicks every slot the line names and
     * calls it done never notices; one that also checks whether a REAL tile
     * exists one slot past the line's own count catches it. That is exactly
     * what the count check has to do, and this is how it is proven to. */
    int n = nreg > 0 ? nreg - 1 : 0;
    kprintf("[wm] dock %d apps:", n);
    for (int i = 0; i < n; i++)
        kprintf(" %d=%s,%s", i, reg[i].file, reg[i].hidden ? "hidden" : "shown");
    kprintf("\n");
#else
    kprintf("[wm] dock %d apps:", nreg);
    for (int i = 0; i < nreg; i++)
        kprintf(" %d=%s,%s", i, reg[i].file, reg[i].hidden ? "hidden" : "shown");
    kprintf("\n");
#endif
}

void scan_apps(void)
{
    int n = vfs_count("/");
    for (int i = 0; i < n && nreg < MAXWIN; i++) {
        char nm[64];
        scopy(nm, vfs_ent_name("/", i), sizeof nm);
        if (!ends_aex(nm)) continue;
        /* THE 64 BYTES, AND ONLY THE 64 BYTES.
         *
         * This used to kmalloc the whole file and read it whole, because
         * logitfs's vfs_read is all-or-nothing (a buffer smaller than the file
         * is an ERROR, not a short read) and there was no partial read in the
         * VFS at all. So the Dock scan -- which runs at boot, before anything
         * else has allocated -- took a contiguous kmalloc the size of the
         * largest .aex in the root, to look at its first 64 bytes. That is what
         * put "never put a big binary in the root" in CLAUDE.md.
         *
         * vfs_pread() exists now (c/fs/vfs.c), so the workaround is unnecessary
         * and so is the advice: 64 bytes on the stack, one read, no allocation
         * on the boot path at all. `sz` is still taken because the fixed header
         * carries no length and a file shorter than one is not a program. */
        int sz = vfs_size(nm);
        if (sz < AEX_HDR_SIZE) continue;    /* aex_info reads the 64-byte header */
        struct aex_header hb;
        if (vfs_pread(nm, &hb, AEX_HDR_SIZE, 0) != AEX_HDR_SIZE) continue;
        char name[32], ext[8];
        if (aex_info(&hb, name, ext) == 0) {
            struct aex_header *h = &hb;
            scopy(reg[nreg].file, nm, sizeof reg[nreg].file);
            scopy(reg[nreg].name, name, sizeof reg[nreg].name);
            scopy(reg[nreg].ext, ext, sizeof reg[nreg].ext);
            reg[nreg].icon = h->icon ? (char)h->icon : reg[nreg].name[0];
            static const uint8_t pal[7][3] = {
                {80,140,255},{55,200,120},{255,92,92},{255,170,40},
                {170,110,255},{40,200,220},{255,120,170} };
            reg[nreg].color = (h->icon_r || h->icon_g || h->icon_b)
                ? rgb(h->icon_r, h->icon_g, h->icon_b)
                : rgb(pal[nreg % 7][0], pal[nreg % 7][1], pal[nreg % 7][2]);
            /* AEX_CAT_TEST apps keep their slot and their tile's ink but lose
             * the ability to be hovered or clicked -- see the comment on
             * `struct regent`. Nothing on the shipped disk is tagged this way
             * as of Task E (2026-09-02: Gallery no longer ships at all rather
             * than shipping hidden -- see the Makefile); the mechanism stays
             * general for whatever next asks for a harness-only tile.
             * h->category is read straight off the raw header already sitting
             * in `hb`; aex_info() does not surface it. */
            reg[nreg].hidden = (h->category == AEX_CAT_TEST);
            nreg++;
        }
    }
    dock_publish();
}

/* ---- the registry, asked by name ------------------------------------------- */

/* How many apps the dock holds (its slots, hidden ones included). */
int dock_app_count(void) { return nreg; }

/* The app registered for documents of extension `ext`, or NULL. Markdown uses
 * the text document handler, including task-produced reports, but an
 * explicitly registered Markdown handler is preferred when one exists. */
const char *dock_file_for_ext(const char *ext)
{
    for (int i = 0; i < nreg; i++)
        if (reg[i].ext[0] && streq(reg[i].ext, ext)) return reg[i].file;
    if (streq(ext, "md"))
        for (int i = 0; i < nreg; i++)
            if (streq(reg[i].ext, "txt")) return reg[i].file;
    return NULL;
}

/* A launch of `aex_file` has started: bounce its icon, if it has one. */
void dock_launch_bounce(const char *aex_file)
{
    for (int i = 0; i < nreg; i++)
        if (streq(reg[i].file, aex_file)) { reg_bounce[i] = anim_stamp(); break; }
}

/* ---- the dock's per-pass work ---------------------------------------------- */

/* The autohide slide (wm_dock_autohide_tick). Its damage asks for its frames. */
int wm_dock_tick(uint64_t now)
{
    (void)now;
    wm_dock_autohide_tick();
    return 0;
}
