#ifndef AUI_H
#define AUI_H
#include "logit.h"

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
};

extern struct aui_theme aui_t;        /* the active theme (follows the system) */
void     aui_ensure(void);            /* fill aui_t on first use (idempotent) */
void     aui_set_dark(int on);        /* swap the whole palette light<->dark */
int      aui_is_dark(void);
unsigned aui_hsl(int h, int s, int l);   /* h:0..359 s,l:0..100 -> packed rgb */
void     aui_set_accent(unsigned color); /* recolor the accent + focus tokens   */
unsigned aui_mix(unsigned a, unsigned b, int t);  /* lerp, t = 0..255 -> b      */
unsigned aui_shade(unsigned c, int delta);        /* lighten/darken per channel */

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

/* ---------- type scale (px cap heights) ---------- */
#define AUI_FS_CAPTION  12
#define AUI_FS_LABEL    13
#define AUI_FS_BODY     15
#define AUI_FS_TITLE    20
#define AUI_FS_HEADING  26

/* ---------- corner radii ----------
 * A radius token, not a number at the call site: the whole UI rounds by the
 * same family of curves or it looks assembled from parts. AUI_R_PILL is
 * clamped to half the shorter side, so it is "fully round" at any size. */
#define AUI_R_NONE   0
#define AUI_R_SM     4
#define AUI_R_MD     8
#define AUI_R_LG     12
#define AUI_R_XL     18
#define AUI_R_PILL   9999

/* ---------- control metrics ---------- */
#define AUI_H_SM     22
#define AUI_H_CTL    28              /* the default control height */
#define AUI_H_LG     36

/* ---------- elevation ----------
 * Elevation is a shadow recipe (y offset, blur, alpha), not a colour. Passing
 * the level to aui_shadow() / aui_card() keeps every raised thing in the system
 * lit from the same direction with the same falloff. */
#define AUI_ELEV_0   0               /* flush with the background: no shadow */
#define AUI_ELEV_1   1               /* a resting card                       */
#define AUI_ELEV_2   2               /* a popover / dropdown                 */
#define AUI_ELEV_3   3               /* a modal sheet                        */

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
void aui_fill(int x, int y, int w, int h, unsigned color);
/* Uniform ALPHA fill. There is no alpha rectangle syscall; this is one 1x1 RGBA
 * pixel blitted over the whole rect, so the kernel's blit loop does the blend
 * and it costs exactly what an opaque fill costs. */
void aui_fill_a(int x, int y, int w, int h, unsigned color, int alpha);
void aui_round(int x, int y, int w, int h, int radius, unsigned color);
void aui_round_a(int x, int y, int w, int h, int radius, unsigned color, int alpha);
void aui_stroke(int x, int y, int w, int h, int radius, int thick, unsigned color);
void aui_circle(int cx, int cy, int radius, unsigned color);
void aui_ring(int cx, int cy, int radius, int thick, unsigned color);
/* Vertical / horizontal gradients. One blit of a 1-pixel-wide (or 1-tall)
 * source strip: the kernel's nearest-neighbour rescale replicates it along the
 * constant axis exactly, so a gradient is the same cost as a flat fill. */
void aui_vgrad(int x, int y, int w, int h, unsigned top, unsigned bottom);
void aui_hgrad(int x, int y, int w, int h, unsigned left, unsigned right);
void aui_vgrad_round(int x, int y, int w, int h, int radius, unsigned top, unsigned bottom);
/* Drop shadow UNDER the rect (x,y,w,h): 8 slices around it, nothing inside, so
 * the cost is O(perimeter * blur) and a big card's shadow is no dearer than a
 * small one's. `elev` is AUI_ELEV_*; aui_shadow_ex takes the recipe directly. */
void aui_shadow(int x, int y, int w, int h, int radius, int elev);
void aui_shadow_ex(int x, int y, int w, int h, int radius, int dy, int blur, int alpha);
void aui_hairline(int x, int y, int w);           /* 1px horizontal rule */
void aui_vhairline(int x, int y, int h);
/* Liquid-glass a region over the content drawn behind it (theme-tinted). For
 * app chrome (sidebars/toolbars/cards) -- draw the content first, then glass. */
void aui_glass(int x, int y, int w, int h, int radius);

/* text: aui_label is body size; aui_heading is title size; aui_text_sz is any. */
void aui_label(int x, int y, const char *s, unsigned color);
void aui_heading(int x, int y, const char *s, unsigned color);
void aui_text_sz(int x, int y, const char *s, unsigned color, int px);
/* A SLICE of a buffer -- `len` bytes from `s`, no NUL needed. This is what a
 * scrolling text view draws: one buffer, a table of (offset, length) lines, and
 * only the visible ones painted. Use it instead of reaching for gui_text_run,
 * which takes WINDOW coordinates and therefore ignores aui_scroll_begin's
 * translation -- see the note at its definition. */
void aui_text_n(int x, int y, const char *s, int len, unsigned color, int px);
int  aui_text_w(const char *s, int px);      /* measured width at size px        */
/* Centred / right-aligned inside a rect, vertically centred on the cap height. */
void aui_text_in(struct aui_rect r, const char *s, unsigned color, int px, int align);
#define AUI_ALIGN_LEFT   0
#define AUI_ALIGN_CENTER 1
#define AUI_ALIGN_RIGHT  2
/* Draw `s` fitted to `maxw`, appending an ellipsis when it does not fit. */
void aui_text_ellipsis(int x, int y, int maxw, const char *s, unsigned color, int px);

void aui_panel(int x, int y, int w, int h, unsigned color);
/* A raised surface with a radius, a hairline and an elevation shadow. */
void aui_card(int x, int y, int w, int h, int elev);

/* ---------------------------------------------------------------- widgets --
 * Every widget returns its interaction (1 on activate, or the new value), and
 * every one takes its state by pointer -- the app owns the state, the toolkit
 * owns the pixels. The four-int (x,y,w,h) signatures of the three original
 * widgets are preserved exactly. */

/* Buttons. aui_button keeps the original signature AND the original look
 * (a glass pill), so no existing app changes appearance. */
int aui_button(int x, int y, int w, int h, const char *label);
int aui_button_ex(int x, int y, int w, int h, const char *label,
                  enum aui_variant v, int enabled);
int aui_icon_button(int x, int y, int size, int icon, int enabled);
/* A tooltip that appears next to the widget just drawn, if it is hovered. */
void aui_tooltip(const char *s);

int aui_checkbox(int x, int y, const char *label, int *state);
int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled);
/* Radio: `value` is this button's value; sets *group to it when chosen. */
int aui_radio(int x, int y, const char *label, int *group, int value);
/* An iOS-style switch. Returns 1 when it changed. */
int aui_toggle(int x, int y, int *state, int enabled);

/* Slider: value in [lo,hi]. Drags, clicks-to-position, and takes arrow keys
 * (and Home/End) when focused. Returns 1 when the value changed. */
int aui_slider(int x, int y, int w, int *value, int lo, int hi);
/* A determinate bar; pct 0..100. pct < 0 = indeterminate (animated stripe). */
void aui_progress(int x, int y, int w, int pct);
void aui_spinner(int cx, int cy, int radius);          /* animated busy ring */

/* Text field. Full editing: caret, click-to-place, shift-select, drag-select,
 * Home/End/arrows, backspace/delete, and Ctrl+A. Returns 1 on Enter. */
int aui_textfield(int x, int y, int w, char *buf, int cap);
int aui_textfield_ex(int x, int y, int w, char *buf, int cap,
                     const char *placeholder, int enabled);

/* Scrollbar. `off` is the scroll offset in content units; returns 1 when the
 * offset changed (drag, click on the trough, or wheel over `track_rect`). */
int aui_scrollbar(int x, int y, int h, int *off, int content, int view);

/* A clipped, scrollable container. Between begin and end, draw in CONTENT
 * coordinates -- aui translates and clips for you.
 *
 *     static int sc;
 *     aui_scroll_begin(x, y, w, h, &sc, content_h);
 *     for (i...) aui_label(0, i*20, item[i], AUI_TEXT);
 *     aui_scroll_end();
 */
void aui_scroll_begin(int x, int y, int w, int h, int *off, int content_h);
void aui_scroll_end(void);

/* Lists and tables. `items` is an array of `n` NUL-terminated strings; `cols`
 * an array of `ncol` header names with `widths` in points. Returns the row
 * index that was activated (clicked or Enter'd), or -1. Arrow keys move the
 * selection when the list has focus. */
int aui_list(int x, int y, int w, int h, const char *const *items, int n,
             int *sel, int *scroll);
int aui_table(int x, int y, int w, int h, const char *const *cols,
              const int *widths, int ncol,
              const char *const *cells, int nrow, int *sel, int *scroll);

/* Tabs / segmented control: `*sel` is the chosen index. Returns 1 on change. */
int aui_tabs(int x, int y, int w, const char *const *items, int n, int *sel);
int aui_segmented(int x, int y, int w, int h, const char *const *items, int n, int *sel);

/* Dropdown. Draws closed; when open it defers its popup to aui_end() so the
 * list paints over everything else in the frame. Returns 1 on a new choice. */
int aui_select(int x, int y, int w, const char *const *items, int n, int *sel);

/* A menu bar: `titles` across the top, each opening `items[i]` (a NULL-
 * terminated array of NUL-terminated strings; "-" is a separator). Writes the
 * chosen menu index into *mi and the item index into *ii, and returns 1. */
int aui_menubar(int x, int y, int w, const char *const *titles,
                const char *const *const *items, int n, int *mi, int *ii);

/* Modal dialog. Returns 1 while it is open; draw its content between the two
 * calls, in dialog-local coordinates. The scrim swallows clicks behind it. */
int  aui_dialog_begin(const char *title, int w, int h);
void aui_dialog_end(void);
/* The standard footer: buttons right-aligned. Returns the index pressed, or -1.
 * Enter picks item 0 (the default), Escape picks the last one (the cancel). */
int  aui_dialog_buttons(const char *const *labels, int n);

/* A coloured status pill. */
void aui_badge(int x, int y, const char *s, unsigned color);
void aui_separator(int x, int y, int w);

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

/* ==========================================================================
 * MOTION -- the toolkit's animation vocabulary.
 *
 * WHY THIS EXISTS AT ALL, AND WHY IT IS SMALL. Before it, the entire animated
 * surface of this toolkit was four things: aui_mix (a colour lerp), the
 * spinner, the indeterminate progress stripe, and -- claimed but not real --
 * the switch knob. Everything else changed state in one frame. The brief was
 * "far too few animations" AND minimal, and those pull the same way only if the
 * vocabulary is a SMALL CLOSED SET applied everywhere, rather than fifteen
 * bespoke curves. Three curves, three durations, one function.
 *
 * THE VOCABULARY IS NOT INVENTED HERE. The window manager has animated in ring
 * 0 for some time -- Expose, the dock fly-down, the window open pop -- on a
 * quadratic ease-out over 0..256 and an 18-tick (180 ms) duration. The toolkit
 * did not know. Inventing a second curve and a second duration in ring 3 would
 * have put one jar behind two doors on the two numbers a person can literally
 * see at the same time: a segmented pill sliding at one speed while the window
 * behind it minimises at another. So the curve is gfx_ease_out() in c/lib/gfx
 * (the one library BOTH rings link -- wm.c calls it now too) and AUI_T_BASE is
 * the kernel's EX_DUR_TICKS restated in milliseconds.
 *
 * THE BUDGET IS THE DESIGN. Measured on this machine, 2026-08-28, at the
 * shipped 1920x1200: a composite costs ~30 ns per composited pixel, and
 * SYS_GUI_FLUSH carries no rectangle -- so the floor on ANY animated frame is
 * the app's WHOLE CANVAS, not the widget. A 640x480 pt window at 150% is
 * 691,200 device px = ~21 ms per frame. That single number decides the whole
 * table below: 90 ms buys 4 delivered frames and 180 ms buys 8, and four steps
 * of a colour fade is banding the eye forgives while four steps of a 33 px
 * travel is a strobe it does not. Hence the rule, which is enforceable by
 * reading a diff: AUI_T_FAST may only appear next to a colour or an alpha.
 * ========================================================================== */

/* ---- curves ----
 * Three, and no more. A public ease-IN is deliberately absent: "starts slow" is
 * only ever right for something LEAVING, and everything that leaves here leaves
 * on alpha in 90 ms, where in, out and linear are indistinguishable. */
#define AUI_EASE_LINEAR  0   /* LOOPS ONLY. Easing a loop puts a pulse in it
                              * that reads as stutter, not as design.        */
#define AUI_EASE_OUT     1   /* the default; ~90% of everything              */
#define AUI_EASE_INOUT   2   /* rest at BOTH ends. Earned by exactly one
                              * category: an indicator that travels across
                              * open space and whose midpoint the eye tracks
                              * (the segmented pill, the tab underline).     */

/* ---- durations, in milliseconds ----
 * One number and two ratios, so there is one thing to change. 180 is not taste:
 * it is the kernel's EX_DUR_TICKS/MINFLY_TICKS (18 ticks at the 100 Hz PIT),
 * already shipped, already known survivable under TCG, already what a window
 * does when it minimises. */
#define AUI_T_FAST   90      /* 0.5x -- colour and alpha, IN PLACE. NEVER geometry. */
#define AUI_T_BASE  180      /* 1.0x -- something MOVES or RESIZES in place         */
#define AUI_T_SLOW  270      /* 1.5x -- a whole NEW surface arrives over content    */

/* ---- cadence ---- these are wake intervals, not durations.
 * A transition runs eight frames and should get every one it can; a loop runs
 * forever and must be cheap. THE SPINNER'S PRICE, stated here so nobody puts
 * one in a full-screen window: at 20 Hz in a 640x480 pt window it is
 * 20 x 691,200 x 30 ns = 415 ms of BKL-held compositor per second of spinning,
 * i.e. 41% of the machine, continuously, for a busy indicator. */
#define AUI_ANIM_TICK_MIN   16   /* the fastest wake a transition may ask for */
#define AUI_ANIM_TICK_LOOP  50   /* the cadence for an ENDLESS animation      */
/* Measured cost of one composited device pixel, 2026-08-28, 1920x1200 4-core
 * TCG. aui_anim_wait() uses it to derive a cadence from the window's own size,
 * so a large window stops asking for frames the compositor cannot make while
 * holding the BKL. A wrong value here makes an animation coarse; it can never
 * make it spin, because the floor is AUI_ANIM_TICK_MIN and the deadline is
 * computed AFTER the frame lands. */
#define AUI_NS_PER_PX       30

/* ---- animation keys ----
 * `key` distinguishes several animations on ONE widget. Named rather than
 * numbered at the call site so a widget that grows a second animation cannot
 * silently collide with its first. */
#define AUI_AK_FACE   0      /* the control face: hover / press cross-fade */
#define AUI_AK_VALUE  1      /* the thing that moves: knob, pill, underline */
#define AUI_AK_FOCUS  2      /* the focus ring's alpha                      */
#define AUI_AK_EXTRA  3

/* ---- the one widget-side call ----
 *
 *     int t = aui_anim(AUI_AK_FACE, hovered ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
 *     fill = aui_mix(base, hover_colour, t);
 *
 * Returns where the value IS this frame, on 0..255 -- aui_mix's domain, which
 * was the entire consistency requirement. (The curves are computed on 0..256;
 * this is the SINGLE place that seam is closed, so no widget ever sees a 256.)
 * Must be called between aui_begin() and aui_end(), from inside the widget that
 * owns the animation, AFTER that widget has taken its id.
 *
 * If `target` differs from what the slot was aiming at, the slot RE-AIMS FROM
 * ITS CURRENT VALUE rather than from its original start -- so a pointer that
 * leaves mid-fade reverses out of where the pixels actually are and never
 * snaps. That is the property that makes an interruptible animation look
 * deliberate instead of broken.
 *
 * IDENTITY, which is the hard problem in an immediate-mode toolkit and is
 * solved here the same way focus already solves it: the slot is keyed by
 * (widget id, key), and the widget id is CALL ORDER -- see the focus note above,
 * which has staked the same bet since the toolkit grew a Tab key. Two extra
 * rules make it safe rather than merely conventional:
 *
 *   - A slot whose id was not queried in the IMMEDIATELY PRECEDING frame is
 *     treated as fresh: it latches to `target` and does not animate. So a
 *     widget appearing for the first time appears finished (a dialog does not
 *     open with thirty hover fades running), and a stale slot can never resume
 *     mid-flight from a value that belonged to something else. Note this is a
 *     FRAME counter, not a clock: an app that sits idle for ten seconds and
 *     then repaints still counts as consecutive, which is exactly right,
 *     because an immediate-mode frame is a pure function of state that the
 *     passage of time did not alter.
 *   - If the caller's layout changes so that a widget takes an id that belonged
 *     to a different widget last frame, that slot re-aims from the previous
 *     occupant's value. The visible consequence is one bounded cross-fade of
 *     the wrong quantity, self-correcting within `ms`; it cannot stick, and it
 *     cannot flicker, because re-aiming is continuous by construction. An app
 *     that reorders deliberately calls aui_anim_reset() -- the same escape
 *     hatch, for the same reason, as aui_set_focus().
 *
 * The table is AUI_ANIM_MAX slots, evicted least-recently-touched. The app
 * declares nothing and immediate mode is untouched. */
#define AUI_ANIM_MAX 32
int  aui_anim(int key, int target, int ms, int curve);
/* Register an ENDLESS animation for this frame (the spinner, the indeterminate
 * bar). It keeps no value -- it only tells the wake contract below that a frame
 * is wanted every AUI_ANIM_TICK_LOOP ms. Returns aui_ms() so a caller can phase
 * off it in one expression. Before this existed, both loop animations computed
 * a phase from the clock and NOTHING WOKE THE APP, so in any app sleeping on
 * wait_idle(0) they were still pictures. */
unsigned aui_anim_loop(void);
/* Forget every slot. For an app that has just restructured its frame on
 * purpose (switched tabs, opened a different page) and would rather have the
 * new widgets appear settled than inherit the old ones' values. */
void aui_anim_reset(void);

/* ---- the app-side wake contract ----
 * THE APP LOOP CHANGES BY ONE LINE, AND THE LOAD-BEARING PART IS THE ZERO:
 *
 *     for (;;) {
 *         int drew = 0;
 *         while (poll_event(&e)) {
 *             if (e.type == EV_CLOSE) app_exit(0);
 *             aui_feed(&e);
 *             if (aui_want_repaint()) { frame(); drew = 1; }
 *             aui_feed_done();
 *         }
 *         if (!drew && aui_anim_due()) frame();
 *         wait_idle(aui_anim_wait());
 *     }
 *
 * When nothing is animating aui_anim_wait() returns 0, and wait_idle(0) is
 * "sleep until an event" -- byte for byte the behaviour that deleting
 * sys_yield() bought (see the top of this file: 3.3 MILLION syscalls in one
 * 8.8-second boot). The regression cannot come back through an app forgetting a
 * case, because the SAFE VALUE IS THE DEFAULT RETURN. Both calls answer from a
 * static and read no clock when nothing is animating, so an idle desktop makes
 * exactly zero extra syscalls -- which is the number the gate checks.
 *
 * NO BACKLOG, EVER. The next deadline is `now + tick` computed in aui_end(),
 * AFTER the frame has landed -- never `last + tick`. A frame that took 27 ms
 * against a 16 ms tick therefore yields a 43 ms period, not a queue of
 * already-expired deadlines. That one choice is the whole difference between
 * this contract and a spin.
 *
 * HOW AN ANIMATION ENDS, which is the half a threshold cannot do: at
 * elapsed >= ms a slot LATCHES its target and is marked arrived. It returns the
 * target and registers no further deadline, and an arrived slot is invisible to
 * both calls below. So the machine draws exactly one frame after arrival -- the
 * frame that puts the final pixel down -- and the next wait is a real sleep.
 * Termination is a proof, not a guess. */
int  aui_anim_due(void);    /* 1 if a deadline has passed: draw a frame     */
int  aui_anim_wait(void);   /* ms to hand wait_idle(); 0 when nothing moves */
/* Two introspection calls, and BOTH ARE CURRENTLY UNCALLED -- said out loud
 * because this tree keeps a list of things built with no real consumer and a
 * silent addition to it is worse than a stale claim. The gate in
 * tests/repaint.mk deliberately does not use them: it counts composites from
 * the COMPOSITOR's own counters, which is a measurement the toolkit cannot
 * fake. They are here for an app that wants to say "busy" honestly, and for an
 * on-device probe. If they still have no caller when the vocabulary has been
 * applied, delete them.
 *
 * aui_anim_active(): slots still in flight. Zero on an idle desktop.
 * aui_anim_frames(): frames drawn because a deadline came due rather than
 *   because of an event. In the loop above these are the same thing; an app
 *   that ignores aui_anim_due() will see this over-count by the frames it
 *   declined to draw. */
int  aui_anim_active(void);
unsigned aui_anim_frames(void);

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
