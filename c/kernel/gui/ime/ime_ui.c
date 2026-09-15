/* c/kernel/gui/ime/ime_ui.c -- the pinyin composition state machine and its
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
#include "ime_learn.h"     /* the user-weight store: the hook, the training signal, the flush */
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
#define IME_EXPANDED_PATH "/ime/pinyin-qwen.dat"

static const struct ime_dict *g_dict;
static uint8_t *g_dat;          /* the resident file; owned here, never freed (see below) */

static void st_reset(void);     /* the one door on ime_reset() -- see below */

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
static int load_dictionary(const char *path)
{
    if (g_dict) return 1;                        /* idempotent */

    int sz = vfs_size(path);
    if (sz <= 0) {
        kprintf("[ime] %s: not found -- the input method is UNAVAILABLE;\n"
                "[ime] " IME_TOGGLE_NAME " will pass through and ASCII input is unchanged\n",
                path);
        return 0;
    }
    uint8_t *buf = kmalloc((unsigned)sz);
    if (!buf) { kprintf("[ime] %s: oom (%d bytes)\n", path, sz); return 0; }

    int off = 0;
    while (off < sz) {
        int want = sz - off;
        if (want > 65536) want = 65536;          /* bounded: a chunk, not the file */
        int got = vfs_pread(path, buf + off, want, off);
        if (got <= 0 || got > want) {
            kprintf("[ime] %s: pread at %d returned %d (want %d) -- refusing a partial dictionary\n",
                    path, off, got, want);
            kfree(buf);
            return 0;
        }
        off += got;
    }

    g_dict = ime_open(buf, (size_t)sz);
    if (!g_dict) {
        kprintf("[ime] %s: %d bytes read but ime_open REFUSED it (bad magic/version,"
                " or more keys than IME_MAX_KEYS)\n", path, sz);
        kfree(buf);
        return 0;
    }
    g_dat = buf;
    kprintf("[ime] %s: %d bytes, %u pinyin keys -- " IME_TOGGLE_NAME
            " toggles pinyin input\n",
            path, sz, (unsigned)g_dict->key_count);

    /* THE STORE, AND IT IS OPENED ONLY AFTER THE DICTIONARY IS. A store loaded
     * beside a dictionary that failed to load would hold weights nothing can
     * ever consult and would then rewrite /var/ime-learn.conf on a machine
     * whose input method is off -- a file that changes for no reason a user
     * could have caused. build_id is passed so the store records WHICH
     * dictionary it learned against (pinyin.h: the field exists for exactly
     * this) and reports a mismatch instead of enforcing one, because entries
     * are keyed on text and survive a regeneration.
     *
     * A failure here is not checked and not fatal by design: ime_learn_init()
     * degrades to an empty table, every ime_learn_weight() returns 0, and the
     * ranking is the dictionary's own frequency order -- which is exactly the
     * behaviour of the build before this store existed. */
    ime_learn_init(g_dict->build_id);
    st_reset();
    return 1;
}

/* Prefer the generated supplement, but a missing/corrupt optional file must
 * not disable Chinese input on an older image. Both loaders validate the
 * complete bytes before publishing g_dict. */
int ime_ui_init(void)
{
    if (g_dict) return 1;
    if (vfs_size(IME_EXPANDED_PATH) > 0 && load_dictionary(IME_EXPANDED_PATH)) return 1;
    return load_dictionary(IME_DICT_PATH);
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

/* ---- ONE DOOR ON ime_reset(), and it exists because of a two-door trap ----
 *
 * pinyin.h: ime_reset() "Clears raw/page/cand AND the user-weight hook -- a
 * state is inert until something installs one", and ime_set_user_weight() must
 * be called "after ime_reset(), before the first ime_feed()". There are five
 * ime_reset() sites in this file (open, park/restore, drop, toggle-off, window
 * teardown) and every one of them therefore has to re-install the hook.
 *
 * Four out of five would have been correct and the fifth would have been a bug
 * with no symptom a test could name: learning would keep working, and would
 * silently stop applying after -- say -- a focus switch, i.e. exactly the
 * "one jar, TWO doors" shape CLAUDE.md records losing a day to three times. So
 * there is one door. Nothing below calls ime_reset() directly.
 *
 * THE CEILING PASSED HERE IS THE STORE'S OWN, spelled from the store's own
 * constants rather than a literal: pinyin.h prunes with
 * `bound(base) = ((base * max_mul_q8) >> 8) + max_add`, and ime_learn_weight()
 * returns at most IME_LEARN_COUNT_MAX * IME_LEARN_STEP and never scales `base`,
 * so (256, that product) is exact -- not generous, not under-declared. An
 * under-declared bound makes the engine clamp away the words the user chose
 * most; an over-declared one costs ranking work on every keystroke.
 *
 * ---- AND THE HOOK IS NOT INSTALLED ON AN EMPTY STORE ----------------------
 *
 * MEASURED, with pinyin.h's own -DIME_STATS counters against the shipped
 * dictionary (candidates the ranker had to score, per keystroke):
 *
 *     buffer   no hook   hook installed   ratio
 *     "n"          266              717   2.7x
 *     "nh"         298              749   2.5x
 *     "nihao"      436              937   2.2x
 *     "s"          483            3,189   6.6x
 *
 * That is not the hook's own cost -- ime_learn_weight() is one FNV-1a and one
 * probe, and it returns on `if (!g_nent)` before either. It is the PRUNE
 * getting weaker: pinyin.h skips a candidate (and the rest of its key) once its
 * score cannot beat the slice minimum, and with a store declaring max_add =
 * 16,384 every candidate might still gain that much, so far fewer can be
 * skipped. The number is identical whether the store holds nothing or one
 * entry, because the engine cannot see inside it -- it prunes with the declared
 * bound, not with the table.
 *
 * So a machine that has never been taught anything must not declare one. This
 * is exact rather than a heuristic: the store goes non-empty only inside
 * ime_learn_note(), which is called from emit(). Correction 2026-09-09:
 * emit() now consumes only the selected prefix and installs the hook on the
 * remaining state immediately after learning, without dropping that suffix. There is no window in which the table has an entry
 * and the live composition is missing the hook.
 *
 * The cost of the check itself is one load and one branch, on a path that
 * already recomputes the whole candidate list. And it stays observable: the
 * toggle-off line prints the entry count, so "was the hook installed" is read
 * off a serial log that is printed anyway rather than inferred. */
static void st_reset(void)
{
	ime_reset(&g_st, g_dict);
	uint32_t learned = 0;
	ime_learn_stats(&learned, 0, 0);
	if (learned)
		ime_set_user_weight(&g_st, ime_learn_weight, 0,
		                    256u, IME_LEARN_STEP * IME_LEARN_COUNT_MAX);
}

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
int ime_ui_available(void) { return g_dict != 0; }

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

/* A vertical list keeps every numbered candidate visible. The old horizontal
 * row was clamped to screen width after measuring it, so selectable words
 * beyond the right edge were invisible. Layout, drawing and mouse hit testing
 * below share the same line-height and padding. */
static int page_count(void) { return (g_st.ncand + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE; }
static int page_items(void) {
    int n = g_st.ncand - g_st.page * IME_PAGE_SIZE;
    return n < 0 ? 0 : (n > IME_PAGE_SIZE ? IME_PAGE_SIZE : n);
}
static int decimal(char *out, int value) {
    char rev[12]; int n = 0;
    do { rev[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (int i = 0; i < n; i++) out[i] = rev[n - i - 1];
    return n;
}
static void bar_row(int item, char *out) {
    int n = 0;
    if (item < 0) {
        for (int i = 0; i < g_st.raw_len; i++) out[n++] = g_st.raw[i];
        if (page_count() > 1) {
            out[n++] = ' '; out[n++] = '[';
            n += decimal(out + n, g_st.page + 1); out[n++] = '/';
            n += decimal(out + n, page_count()); out[n++] = ']';
            const char *hint = " PgUp/PgDn"; while (*hint) out[n++] = *hint++;
        }
    } else if (!page_items()) {
        const char *hint = "Enter: keep spelling"; while (*hint) out[n++] = *hint++;
    } else {
        const struct ime_candidate *c = &g_st.cand[g_st.page * IME_PAGE_SIZE + item];
        out[n++] = (char)('1' + item); out[n++] = ' ';
        for (int i = 0; i < c->ncp; i++) n += cp_utf8(c->cp[i], out + n);
        if (c->raw_used < g_st.raw_len) { out[n++] = ' '; out[n++] = '+'; }
    }
    out[n] = 0;
}

static void bar_layout(void)
{
    if (!ime_ui_composing()) { g_bw = g_bh = 0; return; }
    int rows = page_items(); if (!rows) rows = 1;
    int width = 0;
    char row[R2MAX];
    for (int i = -1; i < rows; i++) {
        bar_row(i, row);
        int w = text_width_sz(row, fb_ui_px());
        if (w > width) width = w;
    }
    int pad = fb_pt(BAR_PAD), lh = text_line_height(fb_ui_px());
    int w = width + 2 * pad, h = 2 * pad + lh * (rows + 1) + fb_pt(BAR_GAP);
    int sw = (int)fb_width(), sh = (int)fb_height();
    if (w > sw - 2 * pad) w = sw - 2 * pad;
    int wi, ax, ay, aw, ah;
    if (wm_ime_anchor(&wi, &ax, &ay, &aw, &ah) && wi == g_owner) {
        g_bx = ax; g_by = ay + ah + fb_pt(6);
    } else { g_bx = (sw - w) / 2; g_by = sh * 3 / 4; }
    if (g_bx + w > sw - pad) g_bx = sw - pad - w;
    if (g_bx < pad) g_bx = pad;
    if (g_by + h > sh - pad) g_by = sh - pad - h;
    if (g_by < 0) g_by = 0;
    g_bw = w; g_bh = h;
}

static void bar_changed(void)
{
    bar_layout();
    /* Include the one-pixel/two-pixel shadow; otherwise closing or shortening
     * the popup leaves its shadow behind on a damage-only compositor. */
    if (g_pw > 0) wm_damage(g_px, g_py, g_pw + fb_pt(1), g_ph + fb_pt(2));
    if (g_bw > 0) wm_damage(g_bx, g_by, g_bw + fb_pt(1), g_bh + fb_pt(2));
    g_px = g_bx; g_py = g_by; g_pw = g_bw; g_ph = g_bh;
}

void ime_ui_compose(void)
{
    if (g_bw <= 0 || g_bh <= 0) return;
    int dark = wm_dark();
    int pad = fb_pt(BAR_PAD), lh = text_line_height(fb_ui_px()), rad = fb_pt(BAR_RAD);
    fb_blend_round_rect(g_bx + fb_pt(1), g_by + fb_pt(2), g_bw, g_bh, rad, 0, 0, 0, 60);
    if (dark) fb_blend_round_rect(g_bx, g_by, g_bw, g_bh, rad, 32, 32, 40, 244);
    else fb_blend_round_rect(g_bx, g_by, g_bw, g_bh, rad, 252, 252, 254, 244);
    fb_blend_round_rect(g_bx, g_by, g_bw, fb_pt(1), 0, 255, 255, 255, dark ? 40 : 190);
    uint32_t ink = dark ? fb_rgb(238,239,244) : fb_rgb(28,28,34);
    uint32_t accent = dark ? fb_rgb(150,200,255) : fb_rgb(40,100,200);
    char row[R2MAX]; bar_row(-1, row);
    text_draw_sz(g_bx + pad, g_by + pad, row, fb_ui_px(), accent);
    int rows = page_items(); if (!rows) rows = 1;
    for (int i = 0; i < rows; i++) {
        bar_row(i,row);
        text_draw_sz(g_bx + pad, g_by + pad + lh * (i + 1) + fb_pt(BAR_GAP), row, fb_ui_px(), ink);
    }
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
    st_reset();
    struct parked *p = &g_park[wi];
    for (int i = 0; i < p->raw_len; i++) ime_feed(&g_st, p->raw[i]);
    for (int i = 0; i < p->page; i++) ime_feed(&g_st, IME_KEY_PGDN);
    g_owner = wi;
}

/* Called before rendering and before dispatching a key/click, never inside
 * the renderer: focus can change without a keystroke, including to an English
 * window. Previously that left the old window's composition painted on top. */
void ime_ui_focus(int wi)
{
    if ((unsigned)wi >= IME_UI_MAXWIN) wi = -1;
    if (g_owner == wi) return;
    if (wi < 0) { park_current(); st_reset(); g_owner = -1; }
    else restore_to(wi);
    bar_changed();
}

static void drop(int wi)
{
    st_reset();
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
    if (g_owner == wi) { st_reset(); g_owner = -1; bar_changed(); }
    /* One of the two "the user may be about to walk away" moments. Still
     * asynchronous -- ime_learn_flush_soon() re-arms the one-shot to 1 ms and
     * returns, so closing a window never waits on the disk, and a no-op if
     * nothing has been learned since the last write. */
    ime_learn_flush_soon();
}

/* ============================ the key path =============================== */

/* Deliver a candidate's codepoints, refusing any that would be read as a
 * KEY_* code (see the IME_CP_MIN/MAX block at the top).
 *
 * ---- AND THIS IS WHERE THE MACHINE LEARNS -------------------------------
 *
 * Every commit is a (what was typed, what was chosen) pair and it is free: the
 * user has already told us. There are four commit paths in this file -- space,
 * a digit 1-9, Enter, and the empty-candidate fallback -- and ALL FOUR go
 * through here, which is the reason the training signal is one call in one
 * function rather than four calls that have to be kept in step.
 *
 * THREE THINGS ARE DELIBERATELY NOT LEARNED, and each is a refusal rather than
 * an omission:
 *
 *   1. A RAW-LETTER COMMIT (Enter, or space with no candidates). idx is
 *      IME_COMMIT_RAW, ime_commit_source() returns 0, and there is nothing to
 *      attribute: the user typed "xyzzy" and got "xyzzy" back. Learning it
 *      would fill a bounded table with ASCII nobody will ever look up.
 *   2. A TIER_SEG COMPOSITION. ime_commit_source() refuses it by contract --
 *      a composed candidate is several keys' top candidates concatenated, so
 *      there is no single entry a weight belongs to. Attributing it to the
 *      whole buffer would teach the machine a word the dictionary does not
 *      have, and the next lookup of that buffer would compose it again from
 *      scratch and find the weight attached to nothing.
 *   3. A COMMIT WHOSE CODEPOINTS WERE REFUSED ABOVE (k != n). The two doors
 *      have to say the same thing: promoting a candidate this file will not
 *      deliver would rank a character the user cannot type above one they can,
 *      and the symptom would be a candidate bar whose first entry does nothing.
 *
 * ORDER MATTERS: ime_commit_source() reads g_st. Previously drop() reset it;
 * now ime_accept() consumes the selected prefix and recomputes the suffix. The
 * lookup is therefore before the drop, and the pointers it hands back point
 * into the read-only dictionary rather than into g_st, so nothing here depends
 * on the composition still being open when ime_learn_note() copies them. */
static int emit(int idx, uint32_t *out, int max)
{
    uint32_t tmp[IME_UI_MAXCP];
    if (max > IME_UI_MAXCP) max = IME_UI_MAXCP;
    int n = ime_commit(&g_st, idx, tmp, max);
    if (n < 0) return 0; /* unavailable digit/capacity leaves the preedit intact */
    for (int i = 0; i < n; i++) {
        uint32_t cp = tmp[i];
        if (cp > 0x7F && (cp < IME_CP_MIN || cp > IME_CP_MAX)) {
            kprintf("[ime] refused candidate codepoint U+%x\n", (unsigned)cp);
            return 0; /* refuse the whole candidate, never deliver half a word */
        }
    }
    const char *key = 0; int keylen = 0;
    const uint8_t *ctext = 0; int clen = 0;
    int learn = ime_commit_source(&g_st, idx, &key, &keylen, &ctext, &clen);
    int k = ime_accept(&g_st, idx, out, max);
    if (k < 0) return 0;
    if (learn) {
        ime_learn_note(key, keylen, ctext, clen);
        ime_set_user_weight(&g_st, ime_learn_weight, 0,
                            256u, IME_LEARN_STEP * IME_LEARN_COUNT_MAX);
    }
    g_park[g_owner].raw_len = 0; g_park[g_owner].page = 0;
    bar_changed();
    return k;
}

int ime_ui_click(int wi, int x, int y, uint32_t *out, int max)
{
    ime_ui_focus(wi);
    if (g_bw <= 0 || x < g_bx || x >= g_bx + g_bw || y < g_by || y >= g_by + g_bh) return -1;
    int y0 = g_by + fb_pt(BAR_PAD) + text_line_height(fb_ui_px()) + fb_pt(BAR_GAP);
    if (y < y0) return 0;
    int i = (y - y0) / text_line_height(fb_ui_px());
    return i < page_items() ? emit(i, out, max) : 0;
}

static uint32_t punctuation(int c)
{
    switch (c) {
    case ',': return 0xFF0C; case '.': return 0x3002;
    case '!': return 0xFF01; case '?': return 0xFF1F;
    case ':': return 0xFF1A; case ';': return 0xFF1B;
    case '(': return 0xFF08; case ')': return 0xFF09;
    default: return 0;
    }
}

/* Punctuation confirms the composition first. The former default branch
 * dropped it, making nihao, insert only a comma. A corrected prefix may leave
 * a suffix open, so finish each suffix before delivering the punctuation. */
static int finish_punctuation(int c, uint32_t *out, int max)
{
    int n = 0;
    while (g_st.raw_len) {
        int k = emit(g_st.ncand ? 0 : IME_COMMIT_RAW, out + n, max - n - 1);
        if (!k) return n;
        n += k;
    }
    if (n < max) out[n++] = punctuation(c);
    return n;
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
int ime_key_slow(int wi, int c, int mods, int toggle, uint32_t *out, int max)
{
    ime_ui_focus(wi);
    if (toggle) {
        if (!g_dict) {
            /* REFUSED OUT LOUD, and the key is PASSED THROUGH rather than
             * swallowed: "ASCII input is untouched" has to mean the machine
             * behaves exactly as it did before this file existed, and a
             * swallowed chord is a behaviour change. */
            kprintf("[ime] " IME_TOGGLE_NAME " REFUSED: " IME_DICT_PATH
                    " is not loaded\n");
            return -1;
        }
        if (g_on[wi]) {
            if (g_owner == wi) { st_reset(); g_owner = -1; }
            g_park[wi].raw_len = 0; g_park[wi].page = 0;
            g_on[wi] = 0;
            /* The other walk-away moment. Turning the IME off is the closest
             * thing this machine has to "I am done typing Chinese", and the
             * debounce window (IME_LEARN_QUIET_MS) is exactly what a power cut
             * would cost -- so it is spent here rather than waited out. */
            ime_learn_flush_soon();
            uint32_t le = 0, lc = 0;
            ime_learn_stats(&le, &lc, 0);
            kprintf("[ime] window %d: pinyin OFF (learned: %u entries, %u commits)\n",
                    wi, (unsigned)le, (unsigned)lc);
        } else {
            g_on[wi] = 1;
            kprintf("[ime] window %d: pinyin ON\n", wi);
        }
        bar_changed();
        /* THE ONLY THING THE USER CAN SEE. bar_changed() damages the candidate
         * bar, which does not exist yet -- a toggle opens no composition, so
         * bar_layout() returns 0x0 and nothing on screen moves. Measured
         * 2026-08-28 by injecting the chord over QMP and screendumping either
         * side: 175 changed pixels of 2,304,000, and all of them the clock. So
         * the machine answered a deliberate keystroke with nothing, while the
         * HOST's own switcher answers Ctrl+Space with an animation -- which is
         * how the owner came to be certain Ctrl+Space was the binding. The
         * menu-bar indicator is the reply; this is what asks for it. */
        wm_damage_menubar();
        return 0;
    }

    ime_ui_focus(wi);
    int composing = (g_st.raw_len > 0);

    /* A system-modifier combination is the app's, always. The previous policy
     * cancelled the preedit on Ctrl+S. Correction 2026-09-09: save/copy now
     * preserve it visibly; Escape is the explicit cancellation command. Ctrl+letter has already been folded to a control code by the
     * keyboard driver, so the mods bit is the only way to tell Ctrl+S from a
     * literal 0x13. */
    if (mods & (EV_MOD_CTRL | EV_MOD_SUPER | EV_MOD_ALT)) {
        return -1; /* keep the preedit visible across save/copy shortcuts */
    }

    if (!composing) {
        /* Only a LOWERCASE letter opens a composition. Shift+letter therefore
         * types a capital straight through, which is the escape hatch for a
         * name or an acronym without toggling the IME off and on again. */
        if (c >= 'a' && c <= 'z') { ime_feed(&g_st, c); bar_changed(); return 0; }
        if (punctuation(c) && max > 0) { out[0] = punctuation(c); return 1; }
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
    case '-': case KEY_PGUP: case KEY_LEFT:
        ime_feed(&g_st, IME_KEY_PGUP); bar_changed(); return 0;
    case '=': case KEY_PGDN: case KEY_RIGHT:
        ime_feed(&g_st, IME_KEY_PGDN); bar_changed(); return 0;
    default: break;
    }

    if (c >= 'a' && c <= 'z') { ime_feed(&g_st, c); bar_changed(); return 0; }
    if (c >= '1' && c <= '9') return emit(c - '1', out, max);

    if (punctuation(c)) return finish_punctuation(c, out, max);
    /* Uppercase/other printable input is an English escape: return the raw
     * spelling and that character together. Navigation preserves the preedit
     * instead of silently destroying it. Escape remains explicit cancellation. */
    if (c >= 32 && c < 127 && max > g_st.raw_len) {
        int n = emit(IME_COMMIT_RAW, out, max - 1);
        out[n++] = (uint32_t)c;
        return n;
    }
    return -1;
}

/* ---- THE NOT-COMPOSING PATH, and it is the whole of what this feature costs
 * a machine that is typing ASCII: an unsigned bounds check, one byte load from
 * a dedicated array (g_on[] is separate from g_park[] precisely so this is a
 * scaled byte load and not a 72-byte struct stride), and the IME_TOGGLE_NAME
 * compare. Nothing above it, and nothing after it but a return.
 *
 * THE TOGGLE IS TESTED BEFORE EVERYTHING, including the composition. So the
 * chord turns the IME off mid-composition and drops what was typed, rather than
 * committing it -- the same rule the unknown-key path takes, and for the reason
 * argued there: the letters were on screen for the user to see disappear, and a
 * candidate they never chose is worse in their document than three lost keys. */
int ime_ui_key(int wi, int c, int mods, uint32_t *out, int max)
{
    if ((unsigned)wi >= (unsigned)IME_UI_MAXWIN) return -1;
    int toggle = (c == ' ' && (mods & (EV_MOD_SHIFT | EV_MOD_CTRL | EV_MOD_ALT | EV_MOD_SUPER)) == IME_TOGGLE_MOD);
    if (!g_on[wi] && !toggle) return -1;
    return ime_key_slow(wi, c, mods, toggle, out, max);
}
