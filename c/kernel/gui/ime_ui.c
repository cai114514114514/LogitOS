/* c/kernel/gui/ime_ui.c -- the pinyin composition state machine and its
 * candidate bar. See ime_ui.h for why this lives in the window manager and for
 * the four hooks it asks of wm.c.
 *
 * The split with c/lib/ime/pinyin.c is the same one c/lib/gfx has with the
 * things that draw: the engine is freestanding, allocator-free and knows
 * nothing about windows, framebuffers or the ABI; this file is the policy --
 * which key means commit, where the bar goes, what happens when focus moves.
 */

#include <stdint.h>
#include <stddef.h>

#include "logit_abi.h"     /* EV_MOD_*, and the KEY_* range this file must not collide with */
#include "ime_ui.h"
#include "pinyin.h"        /* c/lib/ime -- already in the kernel's C_SRC, no Makefile change */
#include "fb.h"
#include "text.h"
#include "vfs.h"
#include "kheap.h"
#include "kprintf.h"
#include "wm.h"

/* ============================ the codepoint contract =======================
 *
 * A committed character is delivered as an ordinary EV_KEY with a = the
 * Unicode codepoint. include/abi/logit_abi.h promises every KEY_* code is
 * "> 0xFF so they never collide with a character", and the eight that exist
 * are 0x101..0x108 -- which as codepoints are U+0101..U+0108, LATIN SMALL
 * LETTER A WITH MACRON through LATIN CAPITAL LETTER C WITH CIRCUMFLEX.
 *
 * That is a REAL Unicode range, not a hole, and its first member is the macron
 * vowel of the very language this file exists to type. It does not bite today
 * and the check is asserted rather than assumed: every codepoint the shipped
 * dictionary can commit was measured against fsroot/ime/pinyin.dat and lies in
 * U+4E00..U+9F9F (4,818 distinct, zero outside), so nothing this file delivers
 * can be mistaken for an arrow key.
 *
 * IME_CP_MIN/MAX is that measurement written down as a runtime refusal. A
 * dictionary regenerated with punctuation, Latin letters or tone marks in it
 * would deliver a codepoint an app reads as KEY_UP, silently, and the app
 * would scroll instead of typing. Refusing here costs one compare per
 * committed character and turns that into a serial line. */
#define IME_CP_MIN 0x4E00u
#define IME_CP_MAX 0x9FFFu

/* ============================ the dictionary ============================== */

#define IME_DICT_PATH "/ime/pinyin.dat"

static const struct ime_dict *g_dict;
static uint8_t *g_dat;          /* the resident file; owned here, never freed (see below) */

/* Read the dictionary with vfs_pread in chunks rather than vfs_read whole.
 *
 * Not a micro-optimisation: logitfs's ->read is ALL OR NOTHING (c/fs/vfs.h
 * says so above ->pread -- "a request that does not cover the whole file is
 * REFUSED"), so a whole-file read gives no way to fail early on a truncated
 * file and no bound on the transfer. pread gives read(2)'s shape, which is
 * what a half-megabyte artefact wants: the buffer is still one allocation
 * (ime_open indexes it in place and never copies), but each transfer is
 * bounded and a short return is a first-class answer instead of a bare -1.
 *
 * The buffer is deliberately never freed on success: struct ime_dict points
 * INTO it (base + a table of byte offsets), exactly as text.c's load_font
 * leaves the TTF resident because struct ttf_font points into it. */
int ime_ui_init(void)
{
    if (g_dict) return 1;                        /* idempotent */

    int sz = vfs_size(IME_DICT_PATH);
    if (sz <= 0) {
        kprintf("[ime] %s: not found -- the input method is UNAVAILABLE;\n"
                "[ime] Ctrl+Space will pass through and ASCII input is unchanged\n",
                IME_DICT_PATH);
        return 0;
    }
    uint8_t *buf = kmalloc((unsigned)sz);
    if (!buf) { kprintf("[ime] %s: oom (%d bytes)\n", IME_DICT_PATH, sz); return 0; }

    int off = 0;
    while (off < sz) {
        int want = sz - off;
        if (want > 65536) want = 65536;          /* bounded: a chunk, not the file */
        int got = vfs_pread(IME_DICT_PATH, buf + off, want, off);
        if (got <= 0) {
            kprintf("[ime] %s: pread at %d returned %d (want %d) -- refusing a partial dictionary\n",
                    IME_DICT_PATH, off, got, want);
            kfree(buf);
            return 0;
        }
        off += got;
    }

    g_dict = ime_open(buf, (size_t)sz);
    if (!g_dict) {
        kprintf("[ime] %s: %d bytes read but ime_open REFUSED it (bad magic/version,"
                " or more keys than IME_MAX_KEYS)\n", IME_DICT_PATH, sz);
        kfree(buf);
        return 0;
    }
    g_dat = buf;
    kprintf("[ime] %s: %d bytes, %u pinyin keys -- Ctrl+Space toggles pinyin input\n",
            IME_DICT_PATH, sz, (unsigned)g_dict->key_count);
    return 1;
}

/* ============================ per-window state ============================
 *
 * ONE live composition (g_st, ~8 KiB) plus a small parked record per window,
 * rather than a struct ime_state per window.
 *
 * The engine's own header is what makes this exact rather than a compromise:
 * "ime_candidates/ime_commit are pure functions of [raw]", recomputed fresh on
 * every feed. So raw[] IS the composition and cand[] is a cache of it --
 * parking means keeping raw[] and page, and restoring means replaying raw[]
 * through ime_feed, which by that same property gives byte-identical
 * candidates to never having switched away.
 *
 * MEASURED cost of the rejected alternative (a full struct ime_state per
 * window): sizeof(struct ime_state) is 8,152 bytes, times MAXWIN 16 = 130,432
 * bytes of kernel .bss, to hold a value derivable from 72. The replay costs
 * one ime_feed per typed letter and happens only when focus moves while a
 * composition is open -- bounded by IME_MAX_RAW = 64 feeds. */
static uint8_t       g_on[IME_UI_MAXWIN];        /* HOT: read on every keystroke, see ime_ui_key */
struct parked { char raw[IME_MAX_RAW]; int raw_len, page; };
static struct parked g_park[IME_UI_MAXWIN];      /* cold */
static struct ime_state g_st;
static int g_owner = -1;                         /* window g_st belongs to, or -1 */

/* The bar's rectangle, in device pixels, latched when the composition opens.
 *
 * ANCHORED ONCE, NOT TRACKED. A bar that followed a window being dragged would
 * have to report damage from inside render_region (too late -- the frame is
 * already being painted) or make every window move damage a rectangle it does
 * not own. Latching keeps damage honest with no coupling: the bar is near the
 * window the composition belongs to, and it stays where it appeared.
 *
 * The caret would be the right anchor and the window manager does not know
 * where it is. The rejected alternative is a syscall for an app to report its
 * caret -- a change under c/apps, off-limits to this line, and one that would
 * make the feature depend on every app being edited. */
static int g_bx, g_by, g_bw, g_bh;               /* current; g_bw == 0 = not showing */
static int g_px, g_py, g_pw, g_ph;               /* what was on screen last, for damage */

int ime_ui_composing(void) { return g_owner >= 0 && g_st.raw_len > 0; }
int ime_ui_enabled(int wi) { return (wi >= 0 && wi < IME_UI_MAXWIN) && g_on[wi]; }

/* ============================ UTF-8 out ================================== */

/* One codepoint to UTF-8, for fb_text. The three-byte branch covers
 * U+0800..U+FFFF, which by IME_CP_MIN/MAX above is every character this file
 * can draw from the dictionary; the ASCII branch is for the raw preview. */
static int cp_utf8(uint32_t cp, char *o)
{
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18));         o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

/* ============================ the bar ==================================== */

#define BAR_PAD   10      /* points */
#define BAR_GAP    4
#define BAR_RAD    9
#define R2MAX    768

/* Build the two rows and return their pixel widths. The text is built here
 * rather than in ime_ui_compose() so the geometry the damage rectangle comes
 * from is the SAME text that is later drawn -- measuring one string and
 * drawing another is how a bar ends up clipped. */
static void bar_rows(char *r1, int r1max, char *r2, int r2max, int *w1, int *w2)
{
    int n = 0;
    for (int i = 0; i < g_st.raw_len && n < r1max - 1; i++) r1[n++] = g_st.raw[i];
    r1[n] = 0;

    n = 0;
    struct ime_candidate pg[IME_PAGE_SIZE];
    int np = ime_candidates(&g_st, pg, IME_PAGE_SIZE);
    if (np < 0) np = 0;
    for (int i = 0; i < np; i++) {
        if (n > r2max - 80) break;
        r2[n++] = (char)('1' + i);
        r2[n++] = ' ';
        for (int k = 0; k < pg[i].ncp; k++) n += cp_utf8(pg[i].cp[k], r2 + n);
        r2[n++] = ' '; r2[n++] = ' ';
    }
    if (np == 0) {
        /* NOT an empty row. A bar that shrinks to the composition line when a
         * syllable has no candidates reads as "the IME broke"; saying that
         * Enter still commits the letters is the one thing the user needs. */
        const char *m = "(no match -- Enter for the letters)";
        while (*m && n < r2max - 1) r2[n++] = *m++;
    } else {
        int pages = (g_st.ncand + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
        if (pages > 1 && n < r2max - 10) {
            r2[n++] = ' '; r2[n++] = ' ';
            r2[n++] = (char)('0' + (g_st.page + 1) % 10);
            r2[n++] = '/';
            if (pages >= 10) r2[n++] = (char)('0' + pages / 10);
            r2[n++] = (char)('0' + pages % 10);
        }
    }
    r2[n] = 0;

    *w1 = text_width_sz(r1, fb_ui_px());
    *w2 = text_width_sz(r2, fb_ui_px());
}

/* Recompute g_b* from the current composition. Called after every state
 * change, never from inside the renderer. */
static void bar_layout(void)
{
    if (!ime_ui_composing()) { g_bw = g_bh = 0; return; }

    char r1[IME_MAX_RAW + 2], r2[R2MAX];
    int w1, w2;
    bar_rows(r1, (int)sizeof r1, r2, (int)sizeof r2, &w1, &w2);

    int pad = fb_pt(BAR_PAD), lh = text_line_height(fb_ui_px());
    int w = (w1 > w2 ? w1 : w2) + 2 * pad;
    int h = 2 * pad + lh * 2 + fb_pt(BAR_GAP);

    int sw = (int)fb_width(), sh = (int)fb_height();
    if (w > sw - 2 * pad) w = sw - 2 * pad;

    /* Anchored just under the focused window's title bar, at its left edge --
     * the closest a caller with no caret can get to "where the text is going". */
    int wi, ax, ay, aw, ah;
    if (wm_ime_anchor(&wi, &ax, &ay, &aw, &ah) && wi == g_owner) {
        g_bx = ax; g_by = ay + ah + fb_pt(6);
        (void)aw;
    } else {
        g_bx = (sw - w) / 2; g_by = sh * 3 / 4;
    }
    if (g_bx + w > sw - pad) g_bx = sw - pad - w;
    if (g_bx < pad) g_bx = pad;
    if (g_by + h > sh - pad) g_by = sh - pad - h;
    if (g_by < 0) g_by = 0;
    g_bw = w; g_bh = h;
}

/* Damage what WAS there and what IS there now. Both, always: a bar that got
 * narrower would otherwise leave its right end standing on the wallpaper --
 * wm.c's dirty_rect has no periodic full repaint behind it to cover for an
 * under-report (see wm.h). */
static void bar_changed(void)
{
    bar_layout();
    if (g_pw > 0) wm_damage(g_px, g_py, g_pw, g_ph);
    if (g_bw > 0) wm_damage(g_bx, g_by, g_bw, g_bh);
    g_px = g_bx; g_py = g_by; g_pw = g_bw; g_ph = g_bh;
}

void ime_ui_compose(void)
{
    if (g_bw <= 0 || g_bh <= 0) return;          /* the not-composing cost of this hook */

    char r1[IME_MAX_RAW + 2], r2[R2MAX];
    int w1, w2;
    bar_rows(r1, (int)sizeof r1, r2, (int)sizeof r2, &w1, &w2);
    (void)w1; (void)w2;

    int dark = wm_dark();
    int pad = fb_pt(BAR_PAD), lh = text_line_height(fb_ui_px()), rad = fb_pt(BAR_RAD);

    /* NOT GLASS, for notify.c's reason: fb_blur_rect samples outside the rect,
     * so a glass panel needs an entry in wm.c's dmg_expand and becomes a second
     * thing a damage rectangle may not cut in half. A blend is clip-exact and
     * asks the compositor for nothing. */
    fb_blend_round_rect(g_bx + fb_pt(1), g_by + fb_pt(2), g_bw, g_bh, rad, 0, 0, 0, 60);
    if (dark) fb_blend_round_rect(g_bx, g_by, g_bw, g_bh, rad, 32, 32, 40, 244);
    else      fb_blend_round_rect(g_bx, g_by, g_bw, g_bh, rad, 252, 252, 254, 244);
    /* The one-row top highlight the notification cards use, same idiom. */
    fb_blend_round_rect(g_bx, g_by, g_bw, fb_pt(1), 0, 255, 255, 255, dark ? 40 : 190);

    uint32_t ink  = dark ? fb_rgb(238, 239, 244) : fb_rgb(28, 28, 34);
    uint32_t ink2 = dark ? fb_rgb(150, 200, 255) : fb_rgb(40, 100, 200);

    /* The composition line in the ACCENT colour and the candidates in the ink
     * colour, not the other way round: the accent marks what is still
     * provisional. */
    text_draw_sz(g_bx + pad, g_by + pad, r1, fb_ui_px(), ink2);
    text_draw_sz(g_bx + pad, g_by + pad + lh + fb_pt(BAR_GAP), r2, fb_ui_px(), ink);
}

/* ============================ focus / teardown =========================== */

static void park_current(void)
{
    if (g_owner < 0) return;
    struct parked *p = &g_park[g_owner];
    for (int i = 0; i < g_st.raw_len; i++) p->raw[i] = g_st.raw[i];
    p->raw_len = g_st.raw_len;
    p->page = g_st.page;
}

/* Replay -- see the per-window note above for why this is exact and not an
 * approximation of a saved state. */
static void restore_to(int wi)
{
    park_current();
    ime_reset(&g_st, g_dict);
    struct parked *p = &g_park[wi];
    for (int i = 0; i < p->raw_len; i++) ime_feed(&g_st, p->raw[i]);
    for (int i = 0; i < p->page; i++) ime_feed(&g_st, IME_KEY_PGDN);
    g_owner = wi;
}

static void drop(int wi)
{
    ime_reset(&g_st, g_dict);
    g_park[wi].raw_len = 0;
    g_park[wi].page = 0;
    bar_changed();
}

void ime_ui_win_gone(int wi)
{
    if (wi < 0 || wi >= IME_UI_MAXWIN) return;
    g_on[wi] = 0;
    g_park[wi].raw_len = 0;
    g_park[wi].page = 0;
    if (g_owner == wi) { ime_reset(&g_st, g_dict); g_owner = -1; bar_changed(); }
}

/* ============================ the key path =============================== */

/* Deliver a candidate's codepoints, refusing any that would be read as a
 * KEY_* code (see the IME_CP_MIN/MAX block at the top). */
static int emit(int idx, uint32_t *out, int max)
{
    uint32_t tmp[IME_UI_MAXCP];
    if (max > IME_UI_MAXCP) max = IME_UI_MAXCP;
    int n = ime_commit(&g_st, idx, tmp, max);
    if (n < 0) return 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = tmp[i];
        if (cp > 0x7F && (cp < IME_CP_MIN || cp > IME_CP_MAX)) {
            kprintf("[ime] REFUSED U+%x: outside U+4E00..U+9FFF, so an app would read it\n"
                    "[ime] as a KEY_* code (logit_abi.h: 0x101..0x108). The dictionary is wrong.\n",
                    (unsigned)cp);
            continue;
        }
        out[k++] = cp;
    }
    drop(g_owner);
    return k;
}

/* SPLIT IN TWO, and the split is the measurement rather than a style choice.
 *
 * With the whole state machine in one function, the not-composing path still
 * paid the prologue the state machine needs: clang spilled five callee-saved
 * registers and opened a stack frame BEFORE the first bounds check, so an
 * ASCII keystroke on a machine with the IME off cost 27 instructions to be
 * told the key was not ours -- 9 of which existed only to preserve registers
 * for code that was about to be skipped. Moving the body behind a noinline
 * call leaves the entry point with no frame at all and a tail call for the
 * one case that needs one. Both numbers are in this line's report; the after
 * is 9 instructions, and this comment is here so the next person who
 * "simplifies" the two back together knows what it costs. */
static __attribute__((noinline))
int ime_key_slow(int wi, int c, int mods, int ctrl_space, uint32_t *out, int max)
{
    if (ctrl_space) {
        if (!g_dict) {
            /* REFUSED OUT LOUD, and the key is PASSED THROUGH rather than
             * swallowed: "ASCII input is untouched" has to mean the machine
             * behaves exactly as it did before this file existed, and a
             * swallowed chord is a behaviour change. */
            kprintf("[ime] Ctrl+Space REFUSED: " IME_DICT_PATH " is not loaded\n");
            return -1;
        }
        if (g_on[wi]) {
            if (g_owner == wi) { ime_reset(&g_st, g_dict); g_owner = -1; }
            g_park[wi].raw_len = 0; g_park[wi].page = 0;
            g_on[wi] = 0;
            kprintf("[ime] window %d: pinyin OFF\n", wi);
        } else {
            g_on[wi] = 1;
            kprintf("[ime] window %d: pinyin ON\n", wi);
        }
        bar_changed();
        return 0;
    }

    if (g_owner != wi) restore_to(wi);
    int composing = (g_st.raw_len > 0);

    /* A system-modifier combination is the app's, always. While composing it
     * additionally CANCELS: Ctrl+S is about to save, and saving a document
     * with a half-typed romanisation still pending is worse than losing three
     * letters. Ctrl+letter has already been folded to a control code by the
     * keyboard driver, so the mods bit is the only way to tell Ctrl+S from a
     * literal 0x13. */
    if (mods & (EV_MOD_CTRL | EV_MOD_SUPER | EV_MOD_ALT)) {
        if (composing) drop(wi);
        return -1;
    }

    if (!composing) {
        /* Only a LOWERCASE letter opens a composition. Shift+letter therefore
         * types a capital straight through, which is the escape hatch for a
         * name or an acronym without toggling the IME off and on again. */
        if (c >= 'a' && c <= 'z') { ime_feed(&g_st, c); bar_changed(); return 0; }
        return -1;
    }

    switch (c) {
    case ' ':                                    /* commit the first candidate */
        if (g_st.ncand > 0) return emit(0, out, max);
        return emit(IME_COMMIT_RAW, out, max);
    case '\n': case '\r':                        /* commit the letters verbatim */
        return emit(IME_COMMIT_RAW, out, max);
    case 27:                                     /* cancel */
        drop(wi);
        return 0;
    case '\b':
        ime_feed(&g_st, '\b');
        bar_changed();
        return 0;
    case '\'':
        ime_feed(&g_st, '\'');
        bar_changed();
        return 0;
    case KEY_PGUP: case KEY_LEFT:
        ime_feed(&g_st, IME_KEY_PGUP); bar_changed(); return 0;
    case KEY_PGDN: case KEY_RIGHT:
        ime_feed(&g_st, IME_KEY_PGDN); bar_changed(); return 0;
    default: break;
    }

    if (c >= 'a' && c <= 'z') { ime_feed(&g_st, c); bar_changed(); return 0; }
    if (c >= '1' && c <= '9') return emit(c - '1', out, max);

    /* Anything else -- Tab, an arrow the bar does not use, a punctuation mark.
     * The composition is DROPPED and the key goes to the app. Dropped rather
     * than committed: committing a candidate the user never looked at puts a
     * character they did not choose into their document, and the letters were
     * on screen for them to see disappear. */
    drop(wi);
    return -1;
}

/* ---- THE NOT-COMPOSING PATH, and it is the whole of what this feature costs
 * a machine that is typing ASCII: an unsigned bounds check, one byte load from
 * a dedicated array (g_on[] is separate from g_park[] precisely so this is a
 * scaled byte load and not a 72-byte struct stride), and the Ctrl+Space
 * compare. Nothing above it, and nothing after it but a return. */
int ime_ui_key(int wi, int c, int mods, uint32_t *out, int max)
{
    if ((unsigned)wi >= (unsigned)IME_UI_MAXWIN) return -1;
    int ctrl_space = (c == ' ' && (mods & EV_MOD_CTRL));
    if (!g_on[wi] && !ctrl_space) return -1;
    return ime_key_slow(wi, c, mods, ctrl_space, out, max);
}
