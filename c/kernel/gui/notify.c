/* Notifications: the third option between a modal and silence.
 *
 * Before this the machine had exactly two ways to say anything -- open a window
 * in the user's face, or say nothing -- so a download finishing, a fetch
 * failing and a process being killed were all in the second category. What is
 * missing is not a convenience; it is the only channel a system has for telling
 * somebody about something they did not just ask about.
 *
 * The presentation contract is in include/abi/logit_abi.h, so that an app
 * author can predict it without reading this file. What is argued here is the
 * drawing, and specifically the two things the compositor holds this code to.
 */

#include <stddef.h>
#include <stdint.h>

#include "logit_abi.h"
#include "notify.h"
#include "fb.h"
#include "text.h"
#include "ktime.h"
#include "usercopy.h"
#include "kprintf.h"
#include "wm.h"

#define S(v) fb_pt(v)          /* points -> device pixels, exactly as wm.c */

/* ---- geometry, in points --------------------------------------------------
 *
 * Top-right, under the menu bar: the macOS position, and it is the right one
 * here for a reason that is not taste. The dock owns the bottom, windows are
 * cascaded from the top-LEFT (see `cascade` in wm.c), and the menu bar's own
 * right end holds the clock and the theme switch -- the top-right strip BELOW
 * it is the one region of this desktop that nothing else claims. A notification
 * that lands where a window is likely to be is a notification that covers the
 * thing it is telling you about. */
#define CARD_W      340
#define CARD_H      74
#define CARD_GAP    10
#define CARD_MARGIN 14         /* from the right edge */
#define CARD_TOP    (24 + 12)  /* MENUBAR_H + a gap; wm.c owns MENUBAR_H = 24 */
#define CARD_RADIUS 16
#define TILE        38         /* the level-coloured square on the left */
/* The drop shadow's offset. It is in this list, and named, because the first
 * version of notify_damage() reported the CARD's rectangle and the shadow falls
 * outside it -- 2,338 pixels of shadow, down the right edge and along the
 * bottom, that nothing ever repainted. tests/qmp/qmp_notify.py's round-trip
 * check found it on the first run. What a thing DRAWS and what a thing REPORTS
 * are two numbers, and this is what it costs to derive them separately. */
#define SHADOW_DX   2
#define SHADOW_DY   3

/* The ring. Bigger than NOTIFY_VISIBLE on purpose: past three on screen the
 * rest QUEUE (see the ABI comment) and appear as slots free, so the ring has to
 * hold both populations. A post into a full ring is dropped and says so by
 * returning 0 -- it does not evict a notification the user may be reading, and
 * it does not block the caller. */
#define NOTIFY_RING 12

/* The entrance animation, in ms; 0 = none, which is the default and the shipped
 * behaviour. See the long comment above notify_compose() for the measurement
 * that chose it and for why the losing variant is kept rather than deleted. */
#ifdef NOTIFY_ANIMATE
#define NOTIFY_ANIM_MS 240
#else
#define NOTIFY_ANIM_MS 0
#endif

struct note {
    int      used;
    int      id;
    int      level;
    int      shown;                    /* 0 = queued, 1 = on screen */
    uint64_t t_show, t_end;            /* ms; valid once shown */
    char     title[NOTIFY_TITLE_MAX];
    char     body[NOTIFY_BODY_MAX];
};

/* `slot[]` is the SCREEN, in order: slot[0] is the top card. `ring[]` is
 * everything alive. Keeping the two separate is what makes "several at once"
 * describable -- the queue is a property of the ring, the stacking is a
 * property of the slots, and promoting one to the other is one loop. */
static struct note ring[NOTIFY_RING];
static int         slot[NOTIFY_VISIBLE];    /* ring indices, -1 = free */
static int         slots_init;
static int         next_id = 1;
static unsigned    posted, dropped;         /* lifetime counters, for tests */

/* The overlay's state, on the serial log, on every change.
 *
 * This is how "several at once" is TESTED rather than described: a burst of
 * seven prints seven lines whose showing= never exceeds NOTIFY_VISIBLE and
 * whose queued= rises and then drains, and a boot harness can assert on that
 * without a screenshot. It is also why there is no "how many are showing"
 * syscall -- an app that can ask is an app that will eventually wait, and a
 * notification you can wait on is a dialog. */
static int queued_count(void);
static void notify_log(const char *what, int id)
{
    int showing = 0;
    for (int s = 0; s < NOTIFY_VISIBLE; s++) if (slot[s] >= 0) showing++;
    kprintf("[wm] notify %s id=%d showing=%d queued=%d posted=%u dropped=%u\n",
            what, id, showing, queued_count(), posted, dropped);
}

static void slots_setup(void)
{
    if (slots_init) return;
    for (int i = 0; i < NOTIFY_VISIBLE; i++) slot[i] = -1;
    slots_init = 1;
}

/* ---- where a card is, in device pixels ------------------------------------ */

static void card_box(int i, int *x, int *y, int *w, int *h)
{
    int sw = (int)fb_width();
    *w = S(CARD_W);
    *h = S(CARD_H);
    *x = sw - *w - S(CARD_MARGIN);
    *y = S(CARD_TOP) + i * (*h + S(CARD_GAP));
}

/* THE NEGATIVE CONTROL for the damage claim. Built with -DNOTIFY_DAMAGE_LIE
 * (make test-notify-negctl) the overlay reports only the LEFT HALF of the
 * column it draws into -- the same mistake the compositor line's own negative
 * control makes, made here on purpose. Every pixel of the right half then stays
 * on screen after the notification is gone, and tests/qmp/qmp_notify.py's
 * round-trip check MUST fail. That check has been watched failing; without
 * that, "the damage rectangle is honest" would be a claim about an assertion
 * nobody has ever seen fire.
 *
 * 320,112 stale pixels is what the compositor line measured from its version of
 * this lie. Ours is smaller because the overlay is smaller -- which is exactly
 * why it needs its own control rather than inheriting theirs. */
#ifdef NOTIFY_DAMAGE_LIE
#define NOTIFY_LIE 1
#else
#define NOTIFY_LIE 0
#endif

/* Damage the WHOLE column, not the individual card that changed.
 *
 * This is a deliberate over-report and it is the safe direction: the
 * compositor's invariant is broken by damage that is too SMALL (stale pixels
 * that nothing ever repaints), never by damage that is too large (wasted work,
 * identical pixels). And a card expiring is not a one-card event -- the cards
 * below it move UP one slot, so the honest per-card rectangle would be "every
 * slot from this one down" in almost every case anyway.
 *
 * The column is NOTIFY_VISIBLE cards tall PLUS THE SHADOW: 342 x 265 points.
 * tests/qmp/qmp_notify.py prints the device-pixel figure and its share of the
 * screen for the mode it ran at, rather than this comment quoting one, because
 * the answer depends on the display and a number in a comment does not. */
static void notify_damage(void)
{
    int x, y, w, h;
    card_box(0, &x, &y, &w, &h);
    int bottom_y, bx, bw, bh;
    card_box(NOTIFY_VISIBLE - 1, &bx, &bottom_y, &bw, &bh);
    /* The shadow is drawn OFFSET from the card, so the footprint is the union
     * of the two -- not the card. Getting this wrong is invisible until
     * something photographs the screen afterwards. */
    w += S(SHADOW_DX) + 1;
    int total_h = bottom_y + bh + S(SHADOW_DY) + 1 - y;
#if NOTIFY_LIE
    w /= 2;
#endif
    wm_damage(x, y, w, total_h);
}

/* ---- posting -------------------------------------------------------------- */

static void copy_str(char *d, int max, const char *s)
{
    int i = 0;
    if (s) for (; i < max - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* Promote queued notifications into free slots, newest LAST so the queue is
 * first-in-first-out: a burst of ten arrives in the order it was sent, which is
 * the only order that can be read as a narrative. */
static void promote(void)
{
    uint64_t now = time_mono_ms();
    for (int s = 0; s < NOTIFY_VISIBLE; s++) {
        if (slot[s] >= 0) continue;
        int best = -1;
        for (int i = 0; i < NOTIFY_RING; i++)
            if (ring[i].used && !ring[i].shown && (best < 0 || ring[i].id < ring[best].id))
                best = i;
        if (best < 0) return;
        ring[best].shown = 1;
        ring[best].t_show = now;
        ring[best].t_end = now + NOTIFY_MS_DEFAULT;
        slot[s] = best;
    }
}

int notify_post(const char *title, const char *body, int level)
{
    slots_setup();
    int i = 0;
    for (; i < NOTIFY_RING; i++) if (!ring[i].used) break;
    if (i == NOTIFY_RING) { dropped++; return 0; }
    ring[i].used = 1;
    ring[i].shown = 0;
    ring[i].id = next_id++;
    ring[i].level = (level < NOTIFY_INFO || level > NOTIFY_ERROR) ? NOTIFY_INFO : level;
    copy_str(ring[i].title, NOTIFY_TITLE_MAX, title);
    copy_str(ring[i].body, NOTIFY_BODY_MAX, body);
    posted++;
    promote();
    notify_damage();
    notify_log("post", ring[i].id);
    return ring[i].id;
}

static int queued_count(void)
{
    int n = 0;
    for (int i = 0; i < NOTIFY_RING; i++) if (ring[i].used && !ring[i].shown) n++;
    return n;
}

long notify_syscall(long num, long a, long b, long c)
{
    if (num != SYS_NOTIFY) return -1;
    char title[NOTIFY_TITLE_MAX], body[NOTIFY_BODY_MAX];
    title[0] = body[0] = 0;
    if (a && user_copy_string(title, (int)sizeof title, (const char *)a) < 0) return -1;
    if (b && user_copy_string(body, (int)sizeof body, (const char *)b) < 0) return -1;
    return notify_post(title, body, (int)c);
}

int notify_showing(void)
{
    slots_setup();
    int n = 0;
    for (int s = 0; s < NOTIFY_VISIBLE; s++) if (slot[s] >= 0) n++;
    return n;
}

/* ---- expiry --------------------------------------------------------------- */

static void close_slot(int s)
{
    if (slot[s] < 0) return;
    int id = ring[slot[s]].id;
    ring[slot[s]].used = 0;
    slot[s] = -1;
    /* Close the gap: everything below moves up one, which is what makes the
     * stack read top-down as "oldest first" no matter which one went away. */
    for (int k = s; k + 1 < NOTIFY_VISIBLE; k++) { slot[k] = slot[k + 1]; slot[k + 1] = -1; }
    notify_log("close", id);
}

void notify_tick(void)
{
    slots_setup();
    uint64_t now = time_mono_ms();
    int changed = 0;
    for (int s = 0; s < NOTIFY_VISIBLE; s++)
        if (slot[s] >= 0 && now >= ring[slot[s]].t_end) { close_slot(s); changed = 1; s--; }
    if (changed) {
        promote();
        notify_damage();
    }
#if NOTIFY_ANIM_MS
    /* The animated variant's cost, made explicit: while any card is inside its
     * entrance window the overlay asks for a frame on EVERY pass of the WM
     * loop -- 100 Hz -- because the picture is different on every one of them.
     * This loop IS the measurement. */
    for (int s = 0; s < NOTIFY_VISIBLE; s++)
        if (slot[s] >= 0 && now - ring[slot[s]].t_show < NOTIFY_ANIM_MS) { notify_damage(); break; }
#endif
}

int notify_click(int x, int y)
{
    slots_setup();
    for (int s = 0; s < NOTIFY_VISIBLE; s++) {
        if (slot[s] < 0) continue;
        int cx, cy, cw, ch;
        card_box(s, &cx, &cy, &cw, &ch);
        if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
            close_slot(s);
            promote();
            notify_damage();
            return 1;
        }
    }
    return 0;
}

/* ---- drawing --------------------------------------------------------------
 *
 * WHY A NOTIFICATION APPEARS RATHER THAN SLIDING IN.
 *
 * A slide-and-fade entrance is the obvious thing to build, and the instruction
 * this work was given was to measure what it costs BEFORE animating it. So both
 * were built: -DNOTIFY_ANIMATE gives the 240 ms slide + fade, the default gives
 * an appearance. The flag is kept rather than deleted, because it is the
 * apparatus the number came from and anyone who wants to re-measure on other
 * hardware should not have to write it again.
 *
 * The measurement, on the machine: `make test-notify-cost` raises three
 * notifications on an otherwise idle 1920x1200 desktop and reads the
 * compositor's OWN counters (`[wm] perf ... composites= cpx= fpx=`) before and
 * after -- AGAINST A CONTROL that runs the same three shell commands with
 * `true` instead of `notify`, because forking and exec'ing a process is itself
 * worth a full-screen frame here and the first version of the measurement was
 * about to credit three of those to the notification.
 *
 * MEASURED (2026-08-08, 1920x1200 @ scale 150, TCG, three notifications raised
 * and left to expire; figures are load minus control):
 *
 *                      composites   px recomposited   full-screen frames
 *   appear/disappear        +5           945,760              0
 *   240 ms slide + fade    +26         5,176,224              0
 *
 * 5.2x the frames and 5.5x the pixels, for two thirds of a second of
 * decoration. The reason is structural rather than a property of this host: an
 * appearance costs ONE frame per state change -- three cards appearing, three
 * expiring -- while an entrance costs one frame per 10 ms tick for its whole
 * duration, and every one of those frames redraws the same column (340x265
 * points: 3.9% of a 1920x1200 screen at scale 100, 8.0% at the scale 150 this
 * was measured at).
 *
 * The compositor line's own summary of where the cost still is -- "what still
 * costs a full frame is motion that changes the picture" -- names this exact
 * thing. The honest answer to being named is not to do it.
 *
 * AND WHY IT IS NOT GLASS, which is the other half of the same argument. Every
 * primitive below is CLIP-EXACT: each output pixel is a function of that pixel
 * and the card, and of nothing around it. fb_liquid_glass is not -- it samples
 * up to ~24 px outside each pixel it writes, which is why wm.c's dmg_expand
 * exists and has to grow any damage rectangle until it contains a whole glass
 * panel. Frosting these cards would have meant either a seam whenever a damage
 * rectangle cut one, or a fourth entry in somebody else's dmg_expand plus a
 * rectangle that grows to swallow the menu bar every time a notification
 * appears. An alpha-blended card composites correctly inside ANY rectangle,
 * which is what lets the WM hook be one unconditional call with no geometry in
 * it. Real cost, real reason, stated rather than discovered later. */

/* The level colour as SEPARATE CHANNELS, never as a packed pixel. fb_rgb packs
 * into the framebuffer's native field order, which is not necessarily
 * red-green-blue -- so a caller that packs a colour and then shifts the
 * components back out of it (as the faded path below needs to) reads the wrong
 * channels on a BGR display, and gets it right on the one it was tested on. */
static void level_rgb(int level, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (level) {
    /* NOT (255,95,86). That is draw_frame's close-button red, and
     * tests/qmp/qmp_repaint.py finds the focused titlebar by taking the
     * bounding box of every pixel of exactly that colour -- an error
     * notification on screen would silently stretch that box across the
     * display and aim an unrelated driver's clicks at nothing. Chrome that
     * shares an exact colour with other chrome is a trap for whoever writes
     * the next pixel test, not a coincidence. */
    case NOTIFY_ERROR: *r = 255; *g =  76; *b =  92; break;
    case NOTIFY_WARN:  *r = 255; *g = 172; *b =  56; break;
    default:           *r =  64; *g = 148; *b = 255; break;
    }
}

/* Draw `s` clipped to `maxw` device pixels, ellipsised. Measuring in the same
 * font and size it will be drawn in is the only way this is right for CJK,
 * where one character is two ASCII widths and a byte count says nothing. */
static void draw_ellipsised(int x, int y, const char *s, int px, int maxw, uint32_t ink)
{
    if (text_width_sz(s, px) <= maxw) { text_draw_sz(x, y, s, px, ink); return; }
    char buf[NOTIFY_BODY_MAX + 4];
    int n = 0;
    while (s[n] && n < (int)sizeof buf - 4) n++;
    /* Shrink on CHARACTER boundaries -- stepping back over continuation bytes,
     * the same rule the clipboard uses -- so an ellipsised CJK body never ends
     * in half a codepoint. */
    while (n > 0) {
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        for (int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = 0;
        if (text_width_sz(buf, px) <= maxw) { text_draw_sz(x, y, buf, px, ink); return; }
        n--;
    }
}

void notify_compose(void)
{
    slots_setup();
    int dark = wm_dark();
#if NOTIFY_ANIM_MS
    uint64_t now = time_mono_ms();
#endif
    for (int s = 0; s < NOTIFY_VISIBLE; s++) {
        if (slot[s] < 0) continue;
        const struct note *n = &ring[slot[s]];
        int x, y, w, h;
        card_box(s, &x, &y, &w, &h);
        int op = 255;                        /* card opacity, 0..255 */
#if NOTIFY_ANIM_MS
        /* Slide in from the right edge + fade, easeOutCubic. Integer only: the
         * kernel builds with SSE but the compositor has no reason to want it. */
        uint64_t e = now - n->t_show;
        if (e < NOTIFY_ANIM_MS) {
            int t = (int)(e * 256 / NOTIFY_ANIM_MS), inv = 256 - t;
            int eased = 256 - inv * inv * inv / (256 * 256);
            x += (w + S(CARD_MARGIN)) * (256 - eased) / 256;
            op = 255 * eased / 256;
        }
#endif

        /* Shadow, then body. Both alpha-blended over whatever the frame has
         * already laid down in this rectangle, which inside a damage rect is a
         * composite that was re-laid from the wallpaper up this very frame --
         * so the card is idempotent from frame to frame instead of darkening
         * itself a little more every time. */
        fb_blend_round_rect(x + S(SHADOW_DX), y + S(SHADOW_DY), w, h, S(CARD_RADIUS),
                            0, 0, 0, (uint8_t)((dark ? 90 : 45) * op / 255));
        if (dark) fb_blend_round_rect(x, y, w, h, S(CARD_RADIUS), 32, 32, 40, (uint8_t)(242 * op / 255));
        else      fb_blend_round_rect(x, y, w, h, S(CARD_RADIUS), 252, 252, 254, (uint8_t)(242 * op / 255));
        /* hairline rim, so the card reads as a card on a light wallpaper too */
        fb_blend_round_rect(x, y, w, S(1), 0, 255, 255, 255, (uint8_t)((dark ? 40 : 190) * op / 255));

        /* The level tile. Saturated on purpose: it is the one thing in the card
         * whose exact RGB a screenshot test can look for, which is how
         * tests/qmp/qmp_notify.py finds a notification without knowing anything
         * about the font. Opaque in the shipped build (op == 255), so the check
         * is an exact-colour one and not a threshold. */
        int tx = x + S(14), ty = y + (h - S(TILE)) / 2;
        uint8_t lr, lg, lb;
        level_rgb(n->level, &lr, &lg, &lb);
        if (op >= 255) fb_round_rect(tx, ty, S(TILE), S(TILE), S(10), fb_rgb(lr, lg, lb));
        else fb_blend_round_rect(tx, ty, S(TILE), S(TILE), S(10), lr, lg, lb, (uint8_t)op);

        int textx = tx + S(TILE) + S(12);
        int maxw  = x + w - S(14) - textx;
        uint32_t ink  = dark ? fb_rgb(238, 239, 244) : fb_rgb(28, 28, 34);
        uint32_t ink2 = dark ? fb_rgb(176, 178, 190) : fb_rgb(96, 98, 110);
        draw_ellipsised(textx, y + S(14), n->title, fb_ui_px(), maxw, ink);
        draw_ellipsised(textx, y + S(38), n->body,  fb_ui_px() * 7 / 8, maxw, ink2);
    }
}
