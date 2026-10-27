/* aui_fields.inc -- text field, scrollbar and scroll containers.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

/* ---- text field ----
 * Caret and selection live here rather than in the app because only the FOCUSED
 * field has any, so a single set of globals is the whole state. They are reset
 * when focus moves, which is also what makes clicking from one field to another
 * do the obvious thing. */
static int tf_owner, tf_caret, tf_anchor;
/* When the caret was last MOVED. A caret that only blinks off a wall clock
 * vanishes for half a second in an app that repaints on input and nothing else
 * -- which is most of them, and which is why it must be solid right after a
 * keystroke and only start blinking once the field has been left alone. */
static unsigned tf_touch;

/* ---- UTF-8, at the one place a raw EV_KEY codepoint becomes bytes in this
 * buffer ----
 *
 * logit_abi.h: EV_KEY's `a` is "a character, or a KEY_* code ... all > 0xFF
 * so they never collide with a character" -- i.e. above ASCII, `a` is either
 * one of the eight enumerated navigation codes or a Unicode code point (the
 * pinyin IME commits CJK this way). `(char)k` truncates a code point to its
 * low byte, which for U+4F60 (你) used to store a lone 0x60 -- a stray
 * backtick where a character should have been, and every byte after it still
 * decoded as if the buffer were well-formed UTF-8, which it no longer was.
 * tf_is_nav_key is enumerated rather than range-tested against KEY_UP/
 * KEY_RIGHT so a future non-contiguous KEY_* addition cannot silently start
 * being typed into text fields. */
static int tf_is_nav_key(int a)
{
    return a == KEY_UP || a == KEY_DOWN || a == KEY_PGUP || a == KEY_PGDN ||
           a == KEY_HOME || a == KEY_END || a == KEY_LEFT || a == KEY_RIGHT;
}

static int tf_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A UTF-8 CONTINUATION byte (10xxxxxx). The caret and every insert/delete
 * boundary must never land on one -- gui_text_run and SYS_TEXT_MEASURE are
 * handed a byte range and decode it as UTF-8 from its start, so a boundary
 * one byte into a multi-byte sequence hands them a lead byte's tail as if it
 * were a new character and desyncs every character after it. */
static int tf_is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int tf_step_left(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && tf_is_cont(s[i])) i--;
    return i;
}

static int tf_step_right(const char *s, int n, int i)
{
    if (i >= n) return n;
    i++;
    while (i < n && tf_is_cont(s[i])) i++;
    return i;
}

static int tf_index_at(const char *buf, int n, int px_off)
{
    int lo = 0, hi = n;
    while (lo < hi) {                       /* binary search: each probe is a syscall */
        int mid = (lo + hi + 1) / 2;
        if (twn(buf, mid) <= px_off) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && tf_is_cont(buf[lo])) lo--;   /* never split a UTF-8 char */
    return lo;
}

static void tf_erase_sel(char *buf, int *n)
{
    int a = imin(tf_caret, tf_anchor), b = imax(tf_caret, tf_anchor);
    if (a == b) return;
    for (int i = a; i + (b - a) <= *n; i++) buf[i] = buf[i + (b - a)];
    *n -= (b - a);
    buf[*n] = 0;
    tf_caret = tf_anchor = a;
}

/* One key against the field's buffer and caret -- pulled out of
 * aui_textfield_ex's body so tests/unit/aui_textfield_test.c can drive it
 * directly (`#include "aui.c"`, same technique as aui_mask_test.c) without
 * exercising a single drawing syscall: this function never calls gui_text_run,
 * aui_round or anything else that reaches the kernel. Returns 1 on Enter
 * (aui_textfield_ex's documented "returns 1 on Enter" contract), 0 if some
 * other key was consumed, -1 if the key was not this widget's to take (the
 * caller must not set in.key_used).
 *
 * -DAUI_BYTE_BACKSPACE is the negative control: it reverts backspace/delete to
 * removing one BYTE instead of one character, which is the original bug --
 * mid-buffer text after a deleted lead byte is left holding orphaned
 * continuation bytes, decodable as neither the old character nor a new one. */
static int tf_apply_key(char *buf, int cap, int *np, int k, int shift)
{
    int n = *np, ret = 0;
    if (k == '\n') ret = 1;
    else if (k == 1) { tf_anchor = 0; tf_caret = n; }             /* Ctrl+A */
    else if (k == '\b' || k == 127) {
        if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
        else if (k == '\b' && tf_caret > 0) {
#ifdef AUI_BYTE_BACKSPACE
            int a = tf_caret - 1;
#else
            int a = tf_step_left(buf, tf_caret);
#endif
            int d = tf_caret - a;
            for (int i = a; i + d <= n; i++) buf[i] = buf[i + d];
            n -= d; tf_caret = a; tf_anchor = tf_caret;
        } else if (k == 127 && tf_caret < n) {
#ifdef AUI_BYTE_BACKSPACE
            int b = tf_caret + 1;
#else
            int b = tf_step_right(buf, n, tf_caret);
#endif
            int d = b - tf_caret;
            for (int i = tf_caret; i + d <= n; i++) buf[i] = buf[i + d];
            n -= d;
        }
        buf[n] = 0;
    }
    else if (k == KEY_LEFT)  { if (tf_caret > 0) tf_caret = tf_step_left(buf, tf_caret); if (!shift) tf_anchor = tf_caret; }
    else if (k == KEY_RIGHT) { if (tf_caret < n) tf_caret = tf_step_right(buf, n, tf_caret); if (!shift) tf_anchor = tf_caret; }
    else if (k == KEY_HOME)  { tf_caret = 0; if (!shift) tf_anchor = 0; }
    else if (k == KEY_END)   { tf_caret = n; if (!shift) tf_anchor = n; }
    else if (k >= 32 && k < 127) {
        if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
        if (n < cap - 1) {
            for (int i = n; i > tf_caret; i--) buf[i] = buf[i - 1];
            buf[tf_caret++] = (char)k; n++; buf[n] = 0; tf_anchor = tf_caret;
        }
    }
    else if (k > 0x7F && !logit_key_is_command((unsigned)k)) {
        /* A code point above ASCII -- UTF-8 encode it and splice in all of its
         * bytes as one atomic unit, same shape as the ASCII branch above. */
        char enc[4]; int el = tf_utf8_encode((unsigned)k, enc);
        if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
        if (n + el < cap) {
            for (int i = n + el - 1; i >= tf_caret + el; i--) buf[i] = buf[i - el];
            for (int i = 0; i < el; i++) buf[tf_caret + i] = enc[i];
            n += el; tf_caret += el; buf[n] = 0; tf_anchor = tf_caret;
        }
    }
    else { *np = n; return -1; }                                  /* not ours */
    *np = n;
    return ret;
}

int aui_textfield_ex(int x, int y, int w, char *buf, int cap, const char *placeholder, int enabled)
{
    int id = ++id_ctr, h = AUI_H_CTL, ret = 0;
    int wx = X_(x), wy = Y_(y);
    /* Focusable, but the keyboard is the FIELD's -- see wpoll_kb. */
    struct wres r = wpoll_kb(id, wx, wy, w, h, enabled, 1, 0);
    int foc = (focus_id == id) && enabled;
    int n = slen(buf);
    int pad = AUI_SP(2);

    if (foc && tf_owner != id) { tf_owner = id; tf_caret = tf_anchor = n; tf_touch = frame_ms; }
    if (foc) { tf_caret = iclamp(tf_caret, 0, n); tf_anchor = iclamp(tf_anchor, 0, n); }

    if (foc && (r.clicked || (in.active == id && in.down && in.ev == EV_MOUSE_MOVE))) {
        int idx = tf_index_at(buf, n, in.mx - wx - pad);
        tf_caret = idx;
        if (r.clicked && !(in.mods & EV_MOD_SHIFT)) tf_anchor = idx;
    }

    if (foc && (r.clicked || in.ev == EV_KEY)) tf_touch = frame_ms;
    if (foc && in.ev == EV_KEY && !in.key_used) {
        int shift = in.mods & EV_MOD_SHIFT;
        in.key_used = 1;
        int kr = tf_apply_key(buf, cap, &n, in.a, shift);
        if (kr < 0) in.key_used = 0;                                  /* not ours */
        else ret = kr;
    }

    unsigned bg = (r.st & AUI_OFF) ? AUI_DISABLED : AUI_SURFACE;
    aui_round(x, y, w, h, AUI_R_MD, bg);
    focus_ring(foc, x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, foc ? AUI_FOCUS
                                            : ((r.st & AUI_HOVER) ? AUI_MUTED : AUI_BORDER));

    struct aui_rect ir = aui_r(x + 1, y + 1, w - 2, h - 2);
    clip_push(aui_r(X_(ir.x), Y_(ir.y), ir.w, ir.h));
    int ty = y + (h - PX) / 2 - 1;
    if (!n && placeholder) txt(x + pad, ty, AUI_MUTED, placeholder);
    if (foc && tf_caret != tf_anchor) {
        int a = imin(tf_caret, tf_anchor), b = imax(tf_caret, tf_anchor);
        int ax = twn(buf, a), bx = twn(buf, b);
        aui_fill(x + pad + ax, y + 4, bx - ax, h - 8, AUI_SELECTION);
    }
    gui_text_run(X_(x) + pad, Y_(ty), PX, 0, (r.st & AUI_OFF) ? AUI_DISABLED_TX : AUI_TEXT, buf, n);
    if (foc && (frame_ms - tf_touch < 600 || ((frame_ms / 500) & 1) == 0))
        aui_fill(x + pad + twn(buf, tf_caret), y + 5, 2, h - 10, AUI_TEXT);
    clip_pop();
    return ret;
}

int aui_textfield(int x, int y, int w, char *buf, int cap)
{ return aui_textfield_ex(x, y, w, buf, cap, 0, 1); }

/* ---- scrolling ---- */

static int scrollbar_thumb(int h, int content, int view)
{ return imin(h, imax(24, (int)((long long)h * view / content))); }

static int scrollbar_input(int id, int x, int y, int h, int *off,
                           int content, int view, int *state)
{
    struct wres r = wpoll(id, X_(x) - 3, Y_(y), 16, h, 1, 0);
    *state = r.st;
    int direct = r.clicked || (in.active == id && in.down);
    if (direct) {
        int thumb = scrollbar_thumb(h, content, view), span = h - thumb;
        long long ty = (long long)in.my - Y_(y) - thumb / 2;
        *off = aui_scroll_clamp(span > 0 ? ty * (content - view) / span : 0,
                                content - view);
    }
    return direct;
}

static void scrollbar_draw(int x, int y, int h, int off, int content, int view, int state)
{
    int thumb = scrollbar_thumb(h, content, view), span = h - thumb;
    int ty = (int)((long long)aui_scroll_clamp(off, content - view) * span / (content - view));
    aui_round(x, y, 10, h, 5, AUI_TRACK);
    aui_round(x, y + ty, 10, thumb, 5,
              (state & (AUI_HOVER | AUI_ACTIVE)) ? AUI_MUTED : aui_mix(AUI_TRACK, AUI_TEXT, 90));
}

int aui_scrollbar(int x, int y, int h, int *off, int content, int view)
{
    int id = ++id_ctr, state = 0, old = *off;
    if (content <= view || h <= 0) { *off = 0; return old != 0; }
    *off = aui_scroll_clamp(*off, content - view);
    scrollbar_input(id, x, y, h, off, content, view, &state);
    scrollbar_draw(x, y, h, *off, content, view, state);
    return old != *off;
}

static struct {
    int x, y, w, h, sx, sy, state, show_bar;
    int *off, content;
    int moving;               /* the offset is still easing: rows publish no aims (ui-aui-core) */
} scr[8];
static int scrn, scr_skip;

static struct scroll_slot *scroll_slot_for(int *off)
{
    struct scroll_slot *free_slot = 0, *oldest = &scroll_slots[0];
    for (int i = 0; i < SCROLL_SLOTS; i++) {
        struct scroll_slot *s = &scroll_slots[i];
        if (s->owner == off) return s;
        if (!s->owner && !free_slot) free_slot = s;
        if ((unsigned)(anim_gen - s->gen) > (unsigned)(anim_gen - oldest->gen)) oldest = s;
    }
    struct scroll_slot *s = free_slot ? free_slot : oldest;
    s->owner = off; s->gen = anim_gen - 2; /* new/evicted owners latch */
    return s;
}

void aui_scroll_begin(int x, int y, int w, int h, int *off, int content_h)
{ aui_scroll_begin_round(x, y, w, h, off, content_h, 0); }

void aui_scroll_begin_round(int x, int y, int w, int h, int *off, int content_h, int radius)
{
    /* A rejected nested begin still has a matching end. Without this balance
     * the ninth container used to pop the eighth one's clip and origin. */
    if (scr_skip || scrn >= 8 || w <= 0 || h <= 0 || !off) { scr_skip++; return; }
    int wx = X_(x), wy = Y_(y), maxo = content_h > h ? content_h - h : 0;
    /* Tiny containers still clip and scroll; only the thumb is omitted when
     * its gutter/track cannot fit. Skipping begin would leave child drawing
     * unbounded during a small resize. */
    int show_bar = maxo > 0 && w >= 14 && h >= 5;
    int state = 0, direct = 0;
    struct scroll_slot *s = scroll_slot_for(off);
    int fresh = s->gen + 1 != anim_gen;
    /* Poll before painting the content, draw the thumb after it. The old
     * end-only poll moved the thumb but left the content one frame behind;
     * on mouse-up there might never be another frame to repair that mismatch.
     * A container reserves the gutter so its children cannot take the drag.
     */
    if (show_bar)
        direct = scrollbar_input(++id_ctr, x + w - 12, y + 2, h - 4,
                                  off, content_h, h, &state);
    long long delta = 0;
    if (in.ev == EV_WHEEL && !scroll_wheel_used && maxo > 0 &&
        (!scroll_wheel_owner || scroll_wheel_owner == off) && input_ok(wx, wy, w, h)) {
        delta = (long long)in.wheel * 48;
        scroll_wheel_used = 1;
    }
    *off = aui_scroll_sample(&s->motion, *off, maxo, delta, frame_ms, AUI_T_BASE,
                             fresh, direct, anim_reduced);
    if (s->motion.running) anim_live++;
    s->gen = anim_gen; s->order = ++scroll_order;
    scr[scrn].x = x; scr[scrn].y = y; scr[scrn].w = w; scr[scrn].h = h;
    scr[scrn].sx = ox_; scr[scrn].sy = oy_; scr[scrn].off = off;
    scr[scrn].content = content_h; scr[scrn].state = state;
    scr[scrn].show_bar = show_bar;
    scr[scrn].moving = s->motion.running;
    scrn++;
    /* Wheel hit testing includes the gutter; content clipping excludes it. */
    s->viewport = clip_intersect(aui_r(wx, wy, w, h));
    {
        /* The container's own corners; the gutter side stays square when a bar
         * is there (its thumb is drawn outside the clip). */
        int rr = radius > 0 ? imin(radius, imin(w, h) / 2) : 0;
        int rad[4] = { rr, show_bar ? 0 : rr, show_bar ? 0 : rr, rr };
        clip_push_r4(aui_r(wx, wy, show_bar ? w - 12 : w, h), rad);
    }
    ox_ = wx; oy_ = wy - *off;
}

void aui_scroll_end(void)
{
    if (scr_skip) { scr_skip--; return; }
    if (!scrn) return;
    scrn--;
    ox_ = scr[scrn].sx; oy_ = scr[scrn].sy;
    clip_pop();
    if (scr[scrn].show_bar)
        scrollbar_draw(scr[scrn].x + scr[scrn].w - 12, scr[scrn].y + 2, scr[scrn].h - 4,
                        *scr[scrn].off, scr[scrn].content, scr[scrn].h, scr[scrn].state);
}
