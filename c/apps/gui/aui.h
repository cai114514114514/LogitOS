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
 *             if (!poll_event(&e)) { sys_yield(); continue; }
 *             if (e.type == EV_CLOSE) app_exit(0);
 *             aui_feed(&e); frame(); aui_feed_done();
 *         }
 *     }
 *     // frame(): aui_begin(AUI_BG); ...widgets...; aui_end();
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
void aui_end(void);                          /* present */
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
 * Animations (the switch knob, the spinner, the indeterminate bar) are driven
 * off this rather than a frame counter, so they run at the same speed whatever
 * the repaint rate is. */
unsigned aui_ms(void);

#endif /* AUI_H */
