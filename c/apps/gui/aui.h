#ifndef AUI_H
#define AUI_H
#include "logit.h"
/* The 「澄」 tokens (design_language.md 3, appendix A): colours, materials and
 * the elevation ladder, one freestanding source for aui, the WM and apps. Named
 * relative to this header so every build that compiles aui (ring 3 and each
 * host harness) finds them without another -I. */
#include "../../include/ui/logit_tokens.h"
#include "../../include/ui/logit_elevation.h"

/* ============================================================================
 * aui -- the Logit immediate-mode UI toolkit, layered on the gui_* syscalls.
 *
 * IMMEDIATE MODE: a widget is drawn and handled in the same call, and the app
 * re-runs its whole frame whenever something happens. There is no retained
 * widget tree, no callbacks and no invalidation bookkeeping -- the frame
 * function IS the UI. That idiom is unchanged from the first version of this
 * file; everything below is more vocabulary inside it, not a new paradigm.
 *
 *     void app_main(void) {
 *         gui_create("Demo", 320, 240);
 *         frame();                            // initial paint
 *         struct logit_event e;
 *         for (;;) {
 *             if (!poll_event(&e)) { wait_idle(0); continue; }
 *             if (e.type == EV_CLOSE) app_exit(0);
 *             aui_feed(&e); frame(); aui_feed_done();
 *         }
 *     }
 *     // frame(): aui_begin(AUI_BG); ...widgets...; aui_end();
 *
 * IT IS wait_idle(0) AND NOT sys_yield(), AND THAT LINE IS THE WHOLE POINT.
 * This example said sys_yield() for two years and every app in c/apps/gui/
 * copied it, which is how the machine came to spend 98% of its kernel entries
 * -- 3.3 MILLION syscalls in one 8.8-second boot -- on apps taking the BKL to
 * be told nothing had happened. wait_idle(ms) parks on this window's event
 * queue instead (SYS_WAIT_EVENT; 0 = no timeout, wake only on an event) and
 * does NOT consume the event, so the poll_event() drain above is unchanged.
 * Pass a timeout ONLY for something with a real deadline -- an animation
 * frame, a socket that must be stepped. A fixed small timeout is a spin with
 * extra steps.
 *
 * HOVER COSTS A REPAINT. The window manager delivers EV_MOUSE_MOVE at pointer
 * rate, and an app that drops those events gets a toolkit with no hover states
 * -- which is most of what separates a 2010s control from a 1995 one. An app
 * that wants hover feeds motion events like any other; one that does not simply
 * keeps dropping them and every widget still works, just without the highlight.
 * aui_want_repaint() answers "did the last event actually change anything I
 * would draw", so an app can feed motion without repainting 100 times a second.
 *
 * WHY THE SHAPES ARE SMOOTH. The kernel's drawing surface has exactly one
 * anti-aliased primitive -- text (SYS_GUI_TEXT_RUN) -- plus vector icons. Its
 * rectangle calls are hard-edged: SYS_GUI_RRECT's corner test in
 * fb_round_rect() is a boolean `dx*dx + dy*dy <= r*r`, so a rounded corner is a
 * staircase, and that staircase is the single loudest thing that dates the UI.
 * What the kernel DOES give us is SYS_GUI_BLIT, which blends a straight-RGBA
 * bitmap into the window with per-pixel alpha. So every smooth thing here is a
 * coverage mask rasterized in ring 3 and handed to that one blending call.
 * Nothing in this toolkit needs a kernel change; see the cost notes on
 * aui_round() for why it is not expensive.
 *
 * THE RASTERIZER IS NOT IN HERE ANY MORE. It is `c/lib/gfx` -- Open Logit, the
 * 2D engine -- and aui is one of its clients. An app that needs a shape the
 * toolkit does not have should include "gfx.h", build a path and fill it,
 * rather than starting a fourth rasterizer; the engine does paths (with
 * quadratics and cubics), both fill rules, an affine transform, four paints
 * (solid / linear gradient / radial gradient / image), src-over compositing and
 * a rect clip. Stroke and path clipping are its phase 2 and do not exist yet.
 * ========================================================================== */

/* ---------------------------------------------------------------- tokens --
 * Semantic colour tokens resolved at runtime from the active theme, so an app
 * can flip light/dark (or restyle the accent) without touching widget code.
 * Fields are only ever APPENDED to this struct: an app that names one by hand
 * keeps compiling. */
struct aui_theme {
    unsigned bg;          /* window background          */
    unsigned surface;     /* raised surface (cards, fields) */
    unsigned face;        /* control face (button)      */
    unsigned text;        /* primary text               */
    unsigned muted;       /* secondary text             */
    unsigned border;      /* hairline / control edge    */
    unsigned hi;          /* top-edge highlight         */
    unsigned accent;      /* brand / selection          */
    unsigned accent_text; /* text on an accent fill     */
    unsigned success;     /* green   */
    unsigned warning;     /* amber   */
    unsigned error;       /* red     */
    unsigned focus;       /* focus ring */
    /* --- appended --- */
    unsigned surface_2;   /* a surface raised ABOVE `surface` (popovers, sheets) */
    unsigned face_hover;  /* control face under the pointer  */
    unsigned face_active; /* control face while pressed      */
    unsigned track;       /* slider / scrollbar / progress trough */
    unsigned thumb;       /* the thing that slides in a track     */
    unsigned disabled;    /* a control that cannot be used   */
    unsigned disabled_tx; /* ...and its label                */
    unsigned scrim;       /* the dimming behind a modal      */
    unsigned shadow;      /* drop-shadow colour (alpha comes from elevation) */
    unsigned selection;   /* text-selection fill             */
    /* --- appended (ui-aui-core): the 「澄」 roles of design 3.1, named as in
     * appendix A, from logit_tokens for the current mode. The accent roles are
     * logit_tokens_derive() of the CURRENT `accent` above, so aui_set_accent()
     * moves them too. The roles appendix A shares with a legacy field (accent,
     * success, warning, error) are not repeated: the legacy fields keep their
     * pre-「澄」 values until ui-aui-theme maps them, and the token values are
     * aui_tok()->accent etc. Fields marked ARGB carry alpha in the high byte;
     * the rest are opaque 0xRRGGBB. */
    unsigned bg_window, bg_content, bg_card, bg_sidebar, bg_inspector,
             bg_toolbar, bg_raised, bg_sunken, bg_stage;
    unsigned fill_control, fill_hover /*ARGB*/, fill_pressed /*ARGB*/,
             fill_zebra, fill_inactive_sel;
    unsigned label_primary, label_secondary, label_tertiary,
             label_quaternary, label_on_accent;
    unsigned sep_hairline /*ARGB*/, sep_opaque, sep_strong /*ARGB*/;
    unsigned accent_hover, accent_pressed, accent_ink,
             accent_tint /*ARGB*/, accent_focus_ring /*ARGB*/, accent_sel_text /*ARGB*/;
    unsigned success_ink, warning_ink, error_ink, destructive_fill;
    unsigned shadow_ink;
};

extern struct aui_theme aui_t;        /* the active theme (follows the system) */
void     aui_ensure(void);            /* fill aui_t on first use (idempotent) */
void     aui_set_dark(int on);        /* swap the whole palette light<->dark */
/* -1 follows the desktop (default); 0/1 pins this process to light/dark.
 * Unlike set_dark, an override survives the next frame and EV_THEME. */
void     aui_theme_override(int mode);
int      aui_is_dark(void);
unsigned aui_hsl(int h, int s, int l);   /* h:0..359 s,l:0..100 -> packed rgb */
void     aui_set_accent(unsigned color); /* recolor the accent + focus tokens   */
unsigned aui_mix(unsigned a, unsigned b, int t);  /* lerp, t = 0..255 -> b      */
unsigned aui_shade(unsigned c, int delta);        /* lighten/darken per channel */
/* The full token set for the current mode, accent roles derived from the
 * current accent (logit_tokens_derive). Recomputed by aui_set_dark,
 * aui_theme_override and aui_set_accent; never NULL. */
const struct logit_tokens *aui_tok(void);

/* Each token lazy-inits the theme (comma operator) before reading, so even the
 * very first `aui_begin(AUI_BG)` -- whose argument is evaluated at the call site
 * -- sees a populated palette rather than zeroed (black) fields. */
#define AUI_BG          (aui_ensure(), aui_t.bg)
#define AUI_SURFACE     (aui_ensure(), aui_t.surface)
#define AUI_SURFACE_2   (aui_ensure(), aui_t.surface_2)
#define AUI_FACE        (aui_ensure(), aui_t.face)
#define AUI_FACE_HOVER  (aui_ensure(), aui_t.face_hover)
#define AUI_FACE_ACTIVE (aui_ensure(), aui_t.face_active)
#define AUI_TEXT        (aui_ensure(), aui_t.text)
#define AUI_MUTED       (aui_ensure(), aui_t.muted)
#define AUI_BORDER      (aui_ensure(), aui_t.border)
#define AUI_HI          (aui_ensure(), aui_t.hi)
#define AUI_ACCENT      (aui_ensure(), aui_t.accent)
#define AUI_ACCENT_TEXT (aui_ensure(), aui_t.accent_text)
#define AUI_SUCCESS     (aui_ensure(), aui_t.success)
#define AUI_WARNING     (aui_ensure(), aui_t.warning)
#define AUI_ERROR       (aui_ensure(), aui_t.error)
#define AUI_FOCUS       (aui_ensure(), aui_t.focus)
#define AUI_TRACK       (aui_ensure(), aui_t.track)
#define AUI_THUMB       (aui_ensure(), aui_t.thumb)
#define AUI_DISABLED    (aui_ensure(), aui_t.disabled)
#define AUI_DISABLED_TX (aui_ensure(), aui_t.disabled_tx)
#define AUI_SCRIM       (aui_ensure(), aui_t.scrim)
#define AUI_SHADOW      (aui_ensure(), aui_t.shadow)
#define AUI_SELECTION   (aui_ensure(), aui_t.selection)

/* ---------- spacing scale (4px base, 8px rhythm) ---------- */
#define AUI_SP(n)   ((n) * 4)        /* AUI_SP(1)=4 AUI_SP(2)=8 AUI_SP(4)=16 ... */
#define AUI_GAP     AUI_SP(2)        /* default gap between stacked items (8)    */
#define AUI_PAD     AUI_SP(4)        /* default content inset from a window edge */

/* ---------- type scale (px) ----------
 * Design 3.6 (the closed size set): body 14 / 400, label (callout) 13,
 * caption (footnote) 12, title (title2) 17 / 600, heading (title1) 22 / 600.
 * The weights travel with the size: draw a title or a heading with
 * aui_text_w(x, y, AUI_FS_TITLE, AUI_W_TITLE, colour, s) -- aui_heading()
 * does exactly that. */
#define AUI_FS_CAPTION  12
#define AUI_FS_LABEL    13
#define AUI_FS_BODY     14
#define AUI_FS_TITLE    17
#define AUI_FS_HEADING  22
#define AUI_W_TITLE     600
#define AUI_W_HEADING   600

/* ---------- corner radii ----------
 * A radius token, not a number at the call site: the whole UI rounds by the
 * same family of curves or it looks assembled from parts. AUI_R_PILL is
 * clamped to half the shorter side, so it is "fully round" at any size.
 * Design 3.4: xs 4 (checkbox, kbd chip, small badges), sm 6 (selection pills,
 * tooltips, segmented thumb), md 8 (fields, popup items), lg 12 (cards,
 * menus, sidebars), xl 14 (popovers), 2xl 18 (alerts, sheets, notifications),
 * panel 22 (Spotlight, About, Dock), window 20 (every window). */
#define AUI_R_NONE   0
#define AUI_R_XS     4
#define AUI_R_SM     6
#define AUI_R_MD     8
#define AUI_R_LG     12
#define AUI_R_XL     14
#define AUI_R_2XL    18
#define AUI_R_PANEL  22
#define AUI_R_WINDOW 20
#define AUI_R_PILL   9999

/* ---------- control metrics (design 3.5) ---------- */
#define AUI_H_SM     24              /* small: toolbar segmented, chips, tags  */
#define AUI_H_CTL    28              /* regular, the default control height    */
#define AUI_H_LG     32              /* large: sheet and alert buttons         */
#define AUI_H_XL     36              /* x-large: Greeter field, panel composer */
#define AUI_H_HERO   44              /* hero: Assistant composer, new-tab search */

/* ---------- elevation ----------
 * Elevation is a shadow recipe, not a colour: a level of the design's ladder
 * (3.3, logit_elevation.h), two analytic shadows in the shadow ink. Passing
 * the level to aui_shadow() / aui_card() keeps every raised thing in the system
 * lit from the same direction with the same falloff. aui_shadow and aui_card
 * take 1/2/3 as E1/E2/E3; aui_elevation takes 1..4 as E1/E2/E3/E6. */
#define AUI_ELEV_0   0               /* flush with the background: no shadow */
#define AUI_ELEV_1   1               /* E1: a resting card                   */
#define AUI_ELEV_2   2               /* E2: a lifted card, a tooltip         */
#define AUI_ELEV_3   3               /* E3: a menu, popover, dropdown        */

/* ---------- widget state bits (aui_state / the *_ex calls) ---------- */
#define AUI_HOVER     0x01
#define AUI_ACTIVE    0x02           /* the pointer is down on it            */
#define AUI_FOCUSED   0x04
#define AUI_OFF       0x08           /* disabled                             */
#define AUI_ON        0x10           /* checked / selected                   */

/* ---------- button variants ---------- */
enum aui_variant {
    AUI_V_PRIMARY,     /* accent fill, the one action of the screen */
    AUI_V_SECONDARY,   /* face fill  */
    AUI_V_GHOST,       /* no fill until hovered */
    AUI_V_DANGER,      /* destructive */
    AUI_V_GLASS,       /* the liquid-glass pill (the pre-existing look) */
};

/* --------------------------------------------------------------- geometry --
 * A rect type exists so layout can be composed (split, inset, aligned) instead
 * of every widget taking four loose ints. The four-int entry points all remain,
 * because six apps already call them. */
struct aui_rect { int x, y, w, h; };

static inline struct aui_rect aui_r(int x, int y, int w, int h)
{ struct aui_rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }

struct aui_rect aui_inset(struct aui_rect r, int dx, int dy);
struct aui_rect aui_cut_top(struct aui_rect *r, int h);    /* take h off the top, shrink *r */
struct aui_rect aui_cut_bottom(struct aui_rect *r, int h);
struct aui_rect aui_cut_left(struct aui_rect *r, int w);
struct aui_rect aui_cut_right(struct aui_rect *r, int w);
int             aui_hit(struct aui_rect r, int x, int y);

/* ------------------------------------------------------------ scale / DPI --
 * Everything an app hands aui is in POINTS, exactly like every gui_* call: the
 * compositor multiplies by the display's backing scale. aui needs the scale
 * only because its rasterizer produces DEVICE pixels -- a coverage mask
 * generated at 1x and stretched to a 2x window would be a blurry mask of a
 * sharp shape, which is worse than no anti-aliasing at all. So masks are
 * rasterized at aui_dev() size and the blit's source and destination then
 * agree 1:1 and the kernel's nearest-neighbour rescale is the identity.
 *
 * An app should not normally call these. Multiplying layout by the scale draws
 * everything twice too big -- the kernel already applied it. */
int aui_scale(void);          /* backing scale in percent (100 = 1x) */
int aui_dev(int points);      /* points -> device pixels (floors, as fb_pt does) */

/* -------------------------------------------------------------- the frame --*/
void aui_feed(const struct logit_event *e);   /* stash the event for the coming frame */
void aui_feed_done(void);                    /* clear it after the frame */
/* 1 if the event just fed can change what is drawn (a click, a key, a hover
 * that entered or left a widget). An app that feeds EV_MOUSE_MOVE can gate its
 * repaint on this and pay for hover only when hover actually moved. */
int  aui_want_repaint(void);

void aui_begin(unsigned bg);                 /* reset widget ids + clear the window */
void aui_end(void);                          /* present: gui_flush() or gui_flush_rect(), whichever this frame's own draw-call diff proved honest -- see aui.c section 5a-flush */
/* Same as aui_end(), plus ONE caller-supplied damage rect (window-local
 * points, same origin as gui_rect's) UNIONED into that frame's own diff --
 * for a caller who has already computed narrower damage than aui's generic
 * per-primitive tracking can see on its own, because it paints outside any
 * aui_* wrapper (textedit.c's raw gui_text_run() calls for its paragraph
 * text are the reason this exists). w<=0||h<=0 is "no extra hint": behaves
 * exactly like aui_end(). Never narrows what aui_end() alone would have
 * flushed, only ever widens it. */
void aui_end_rect(int x, int y, int w, int h);
/* The window size aui lays out against. Set it once after gui_create() and
 * aui_end() can size scrollbars, centre dialogs and place tooltips; without it
 * those fall back to a 640x480 assumption. */
void aui_set_size(int w, int h);
int  aui_width(void);
int  aui_height(void);

/* ---------------------------------------------------------------- drawing --
 * The primitive layer. Everything above it is built out of these, and an app
 * that needs a shape the toolkit does not have should reach for these rather
 * than gui_rect.
 *
 * COST. aui_round() is a 9-slice: the interior and the four edge bands go out
 * as opaque gui_rect calls (the kernel's fast path) and only the four corner
 * tiles -- r x r device pixels each -- are rasterized and blitted. So a rounded
 * rect costs O(r^2), not O(w*h), and the corner coverage masks are cached by
 * radius, so the second widget with the same radius rasterizes nothing at all.
 * A 200x40 pt card with r=8 pushes 4 blits of 8x8 and 5 rects: nine syscalls,
 * ~256 blended pixels. */
#include "aui_paint.h"

#include "aui_text.h"


/* ------------------------------------------------------------------- aims --
 * Test geometry over serial (design P13, ui_aim.h). An app names itself once
 * with ui_aim_set_app("monitor") and aims hand-drawn controls with ui_aim();
 * controls that an aui WIDGET draws get their aims from the widget, and only
 * from it (one emitter per name):
 *
 *     aui_aim_prefix("tab");          aui_tabs(...);      // tab-0, tab-1, ...
 *     aui_aim_names(names, 3);        aui_segmented(...); // names[i] per segment
 *
 * The prefix or name list arms the NEXT aims-aware widget call of the frame:
 * aui_tabs, aui_segmented, aui_list and aui_table (one aim per VISIBLE row),
 * aui_select (the control itself: the prefix, or names[0]). The open popup of
 * a select or menu always publishes `popup-<i>` per row, and aui_dialog_buttons
 * always publishes `dialog-<i>`, `dialog-confirm` (item 0, Enter) and, with
 * more than one button, `dialog-cancel` (the last, Escape) -- as soon as the
 * app has called ui_aim_set_app, with or without a prefix.
 *
 * An aim is published after layout, at the item's centre in window-local
 * points, only for items inside the visible clip, and never while the widget
 * is moving (a tab underline or segmented thumb in flight, a list whose scroll
 * is still easing, a popup or sheet still fading in): the next settled frame
 * publishes it. aui_begin() disarms a prefix no widget consumed. */
#include "ui_aim.h"
void aui_aim_prefix(const char *prefix);
void aui_aim_names(const char *const *names, int n);

/* ---------------------------------------------------------------- widgets --
 * Every widget returns its interaction (1 on activate, or the new value), and
 * every one takes its state by pointer -- the app owns the state, the toolkit
 * owns the pixels. The four-int (x,y,w,h) signatures of the three original
 * widgets are preserved exactly. */

#include "aui_button.h"
#include "aui_controls.h"
#include "aui_fields.h"
#include "aui_lists.h"
#include "aui_overlay.h"
#include "aui_nav.h"
#include "aui_form.h"
#include "aui_data.h"

/* Rounded clips and opacity groups (GUI ABI v2 CLIP / GROUP; both fall back
 * without them). Every push needs its pop in the same frame; aui_end() closes
 * what was left open. `r` is in the current drawing coordinates.
 *
 * aui_clip_push_round(): later drawing is clipped to r with every corner
 * `radius` points, intersected with the clip in force. Returns 1 when the
 * kernel is doing the rounding, 0 when only the square clip is (a kernel
 * without CLIP_PUSH, or a full level stack). aui_clip_pop() undoes the last
 * push from either this or the scroll/dialog containers.
 *
 * aui_fade_begin(): everything drawn until aui_fade_end() is composited over
 * what was there at `alpha` (0..255) as one layer -- overlapping shapes, text
 * and shadows fade together, not each by itself. On OL_LIMIT or OL_UNSUPPORTED
 * no group is made and the fade is the old colour mixing, done by aui's own
 * widgets; app code between begin and end is then drawn at full strength.
 * Include any shadow in `r`: the group only changes pixels inside it. */
int  aui_clip_push_round(struct aui_rect r, int radius);
void aui_clip_pop(void);
void aui_fade_begin(struct aui_rect r, int alpha);
void aui_fade_end(void);

/* ------------------------------------------------------------------ focus --
 * Tab / Shift+Tab walk the focusable widgets in the order they were drawn;
 * Enter activates the focused control; arrows drive sliders, lists, tabs and
 * radio groups. The toolkit consumes those keys itself, so an app gets keyboard
 * navigation without writing any.
 *
 * Because focus order IS call order, it is stable frame to frame and needs no
 * registration step -- but it also means a widget drawn conditionally shifts
 * the ids after it. aui_focus_id()/aui_set_focus() exist for an app that wants
 * to pin focus across such a change. */
int  aui_focus_id(void);
void aui_set_focus(int id);
void aui_focus_next(int dir);          /* +1 / -1 */
int  aui_focus_visible(void);          /* 1 once the keyboard has been used */

/* ----------------------------------------------------------------- layout --
 * Stacks nest (8 deep). aui_next() answers the top-left of the next item and
 * advances the cursor; the item's cross-axis extent can be stretched to the
 * stack's width with AUI_FILL.
 *
 *     aui_vstack(AUI_PAD, AUI_PAD, AUI_GAP);
 *       aui_row(&r, AUI_FILL, AUI_H_CTL);  aui_button_ex(r.x, r.y, r.w, r.h, ...);
 *       aui_row(&r, AUI_FILL, AUI_H_CTL);  aui_textfield(r.x, r.y, r.w, ...);
 *     aui_stack_end();
 */
#define AUI_FILL (-1)
void aui_vstack(int x, int y, int gap);          /* also: opens a stack */
void aui_hstack(int x, int y, int gap);
void aui_vstack_w(int x, int y, int w, int gap); /* with a fixed cross-axis width */
void aui_hstack_h(int x, int y, int h, int gap);
void aui_stack_end(void);
void aui_next(int w, int h, int *x, int *y);     /* the original 2-out form */
void aui_row(struct aui_rect *out, int w, int h);/* the rect form, honours AUI_FILL */
void aui_spacer(int n);                          /* advance the cursor by n points */

/* ------------------------------------------------------------------ time --
 * The frame's monotonic clock, sampled once in aui_begin() so everything drawn
 * in one frame agrees about what "now" is. Anything that moves should be a
 * function of THIS rather than of a frame counter, so it runs at the same speed
 * whatever the repaint rate turns out to be.
 *
 * (This block used to say the switch knob was driven off it. It was not --
 * aui_toggle teleported the knob in one frame. The header documented an
 * animation that did not exist for as long as the toggle has shipped, which is
 * the exact shape of stale claim this tree keeps paying for. It is true now.) */
unsigned aui_ms(void);

#include "aui_motion.h"

/* DAMAGE, and it is a cost rather than a gift. An animated frame cannot
 * under-report its extent -- the failure mode that leaves smears nothing
 * repaints -- because there is nowhere to report an extent: SYS_GUI_FLUSH
 * carries no rectangle, so the compositor damages the app's whole canvas, and
 * aui_begin() clears the whole canvas to match. Correct damage is therefore
 * structural here, not a discipline. The flip side is the floor named above:
 * every animated frame costs the whole window. It is an ABI limit, not a
 * compositor one, and closing it is a change to SYS_GUI_FLUSH.
 *
 * The one geometric hazard the toolkit CANNOT see, and it belongs to the gate:
 * the compositor grows any damage rectangle that touches a glass panel until it
 * contains the WHOLE panel, because blur reads a neighbourhood and cannot be
 * clipped. A window whose bottom edge reaches the dock therefore adds ~77,000
 * glass pixels at ~207 ns each -- about +16 ms PER FRAME -- to every animated
 * frame. Same widget, same code, 1.8x the cost, decided entirely by where the
 * user left the window. tests/qmp/qmp_repaint.py's `anim` class runs each
 * interaction twice, clear of and overlapping the dock, and publishes both. */

#endif /* AUI_H */
