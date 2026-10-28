/* aui_fields.inc -- text field, search field, overlay scrollbar and scroll
 * containers.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. The key
 * handling is moved verbatim from aui.c (ui-aui-core); the fields, the caret,
 * the search capsule and the overlay scrollbar are ui-aui-fields (design 「澄」
 * 4.4 and 4.12). */

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
/* The focused field's horizontal scroll, in points: the text slides left so the
 * caret stays inside the box (ui-aui-fields). Like the caret it belongs to the
 * one focused field and is reset when focus moves. */
static int tf_sx;

/* ---- field metrics (design 4.4) ---- */
#define TF_PX        14      /* body */
#define TF_PAD       10      /* text field padding x */
#define TF_SEARCH_X  30      /* search field: text starts here (SEARCH glyph at 10) */
#define TF_CARET_H   18      /* points; 2 DEVICE pixels wide */
#define TF_BLINK_MS  530     /* on for 530, off for 530 */
#define TF_FADE_MS   90      /* the on/off cross-fade */
#define TF_IDLE_MS   8000    /* solid and silent after this long without input */

/* The caret's opacity, 0..255, `since` ms after the last time it was moved.
 * Pure, so the host test can sweep it. Solid for the first 530 ms (a caret
 * that vanishes right after a keystroke reads as lost), then 530 off / 530 on
 * with a 90 ms linear fade at every switch, and solid again for good once the
 * field has been left alone for TF_IDLE_MS -- the frame at which the loop stops
 * asking for wakes. Reduce Motion keeps it solid: a caret that blinks only on
 * the frames some other event happens to draw would be missing half the time. */
static int tf_caret_alpha(unsigned since, int reduced)
{
    if (reduced) return 255;
    if (since >= TF_IDLE_MS) return 255;
    unsigned k = since / TF_BLINK_MS, q = since % TF_BLINK_MS;
    int on = !(k & 1);
    if (k == 0) return 255;
    if (q < TF_FADE_MS) {
        int t = (int)(q * 255u / TF_FADE_MS);
        return on ? t : 255 - t;
    }
    return on ? 255 : 0;
}

/* Caret width: 2 DEVICE pixels, in whole points (the smallest that covers 2). */
static int tf_caret_w(void)
{
    int sc = aui_scale();
    return sc > 0 ? imax(1, (200 + sc - 1) / sc) : 2;
}

static int tfw(const char *s, int n) { return n <= 0 ? 0 : text_measure_px(s, n, TF_PX, 0); }

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
        if (tfw(buf, mid) <= px_off) lo = mid; else hi = mid - 1;
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


/* ---- field drawing helpers (design 4.4) ---- */

/* The rim of a field: exactly 1 DEVICE pixel (`width_q8` is 24.8 points), the
 * outer edge on the rect's. Without PATH a whole-point ring in the colour
 * already blended over the window, which is what the old tile path can do. */
static void fld_rim(struct aui_rect r, int radius, unsigned argb)
{
    int a = (int)(argb >> 24);
    if (a == 0) return;
    if (path_ok()) {
        int sc = aui_scale();
        aui_stroke_a(r, radius, sc > 0 ? 25600 / sc : 256, argb);
        return;
    }
    aui_stroke(r.x, r.y, r.w, r.h, radius, 1, aui_mix(aui_t.bg_window, argb & 0xFFFFFFu, a));
}

/* The focus ring of a text or search field: accent.focusRing, 3 pt wide, its
 * inner edge 1 pt outside the control and concentric with it, faded over 90 ms.
 * Shown on ANY focus (a text field takes the keyboard however it got it), which
 * is why this is not focus_ring(), whose AUI_FOCUS rings follow the keyboard
 * only. Queried every frame the field draws, focused or not: the fade needs the
 * slot to be continuous. */
static void fld_ring(int focused, struct aui_rect r, int radius)
{
    int t = aui_anim(AUI_AK_FOCUS, focused ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    if (t <= 0) return;
    unsigned c = aui_t.accent_focus_ring;
    unsigned a = (c >> 24) * (unsigned)t / 255u;
    if (!path_ok()) {      /* no alpha ring without PATH: the blend, as an opaque tile ring */
        aui_stroke(r.x - 4, r.y - 4, r.w + 8, r.h + 8, radius + 4, 3,
                   aui_mix(aui_t.bg_window, c & 0xFFFFFFu, (int)a));
        return;
    }
    aui_stroke_a(aui_inset(r, -4, -4), radius + 4, 3 * 256, (a << 24) | (c & 0xFFFFFFu));
}

/* Keep the caret inside the visible text width `vw` by sliding the text. */
static void tf_follow(const char *buf, int n, int vw)
{
    int total = tfw(buf, n), cx = tfw(buf, tf_caret);
    if (total <= vw) { tf_sx = 0; return; }
    if (cx - tf_sx > vw - 2) tf_sx = cx - vw + 2;
    if (cx < tf_sx) tf_sx = cx;
    tf_sx = iclamp(tf_sx, 0, total - vw + 2);
}

#define AUI_TFX_LARGE_H 36

/* The one field behind aui_textfield*, aui_searchfield*: `search` selects the
 * capsule (glyph, clear button, Esc) and the rest is shared -- the keys, the
 * caret, the selection and the scroll are the same code. */
static int fld_core(int x, int y, int w, char *buf, int cap, const char *placeholder,
                    int enabled, const char *error, int flags, int search)
{
    int id = ++id_ctr, ret = 0;
    /* The clear button's id is taken UNCONDITIONALLY: a widget id that came and
     * went with the text would shift every id after the field (focus order and
     * animation slots are call order). */
    int cid = search ? ++id_ctr : 0;
    int large = !search && (flags & AUI_TF_LARGE);
    int h = large ? AUI_TFX_LARGE_H : AUI_H_CTL;
    int radius = search ? h / 2 : (large ? 10 : AUI_R_MD);
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll_kb(id, wx, wy, w, h, enabled, 1, 0);
    int foc = (focus_id == id) && enabled;
    int n = slen(buf);
    int pad = search ? TF_SEARCH_X : TF_PAD;
    int right = search ? (n > 0 ? 30 : 10) : TF_PAD;     /* the clear button's room */

    if (foc && tf_owner != id) { tf_owner = id; tf_caret = tf_anchor = n; tf_touch = frame_ms; tf_sx = 0; }
    if (foc) { tf_caret = iclamp(tf_caret, 0, n); tf_anchor = iclamp(tf_anchor, 0, n); }

    if (foc && (r.clicked || (in.active == id && in.down && in.ev == EV_MOUSE_MOVE))) {
        int idx = tf_index_at(buf, n, in.mx - wx - pad + tf_sx);
        tf_caret = idx;
        if (r.clicked && !(in.mods & EV_MOD_SHIFT)) tf_anchor = idx;
    }

    if (foc && (r.clicked || in.ev == EV_KEY)) tf_touch = frame_ms;
    if (foc && in.ev == EV_KEY && !in.key_used) {
        int shift = in.mods & EV_MOD_SHIFT;
        in.key_used = 1;
        if (search && in.a == 27) {
            /* Esc: clear, and only when there is nothing left to clear, blur. */
            if (n > 0) { buf[0] = 0; n = 0; tf_caret = tf_anchor = 0; tf_sx = 0; }
            else { focus_id = 0; foc = 0; }
        } else {
            int kr = tf_apply_key(buf, cap, &n, in.a, shift);
            if (kr < 0) in.key_used = 0;                              /* not ours */
            else ret = kr;
        }
    }

    /* The clear button: a 16 pt disc 8 from the right edge, a 24 x 24 hit zone.
     * Polled AFTER the field so that a press on it leaves the field focused. */
    int clear_x = x + w - 24, clear_y = y + (h - 16) / 2, clear_hot = 0;
    if (search && enabled) {
        struct wres cr = wpoll_kb(cid, X_(x + w - 28), wy + (h - 24) / 2, 28, 24, n > 0, 0, 0);
        clear_hot = (cr.st & AUI_HOVER) != 0;
        if (cr.clicked) {
            buf[0] = 0; n = 0; tf_caret = tf_anchor = 0; tf_sx = 0; tf_touch = frame_ms;
            focus_id = id; foc = 1; tf_owner = id;
            clear_hot = 0;
        }
    }
    int has_clear = search && enabled && n > 0;

    struct aui_rect box = aui_r(x, y, w, h);
    int dark = aui_is_dark();

    /* Disabled: no box at all, the value (or the placeholder) as plain text. */
    if (!enabled && !search) {
        struct aui_rect tr = aui_r(x + pad, y, w - 2 * pad, h);
        if (n > 0) aui_text_vc(tr, buf, TF_PX, AUI_W_REGULAR, aui_t.label_secondary, AUI_ALIGN_LEFT);
        else if (placeholder) aui_text_vc(tr, placeholder, TF_PX, AUI_W_REGULAR, aui_t.label_tertiary, AUI_ALIGN_LEFT);
        return 0;
    }

    /* The box: white over the window in light, white @ 13 in dark. */
    if (search) aui_round(x, y, w, h, radius, aui_t.fill_control);
    else if (dark) aui_round_a(x, y, w, h, radius, 0xFFFFFF, 13);
    else aui_round(x, y, w, h, radius, aui_t.bg_content);

    fld_ring(foc, box, radius);
    if (!search) {
        unsigned rim = error ? (0xFF000000u | aui_t.error)
                             : foc ? (0xFF000000u | aui_t.accent) : aui_t.sep_strong;
        fld_rim(box, radius, aui_anim_color(AUI_AK_VALUE, rim, rim));
    }

    int left = pad, vw = w - pad - right;
    if (foc) tf_follow(buf, n, vw);
    int sx = foc ? tf_sx : 0;
    int cy = y + (h - TF_CARET_H) / 2;

    if (search) {
        gui_icon(GICON_SEARCH, X_(x) + 10, Y_(y) + (h - 14) / 2, 14, aui_t.label_tertiary);
    }

    clip_push(aui_r(wx + left - 4, wy + 1, vw + 8, h - 2));
    struct aui_rect tr = aui_r(x + left - sx, y, vw + sx, h);
    if (!n && placeholder)
        aui_text_vc(aui_r(x + left, y, vw, h), placeholder, TF_PX, AUI_W_REGULAR, aui_t.label_tertiary, AUI_ALIGN_LEFT);
    if (foc && tf_caret != tf_anchor) {
        int a = imin(tf_caret, tf_anchor), b = imax(tf_caret, tf_anchor);
        int ax = tfw(buf, a), bx = tfw(buf, b);
        unsigned sel = aui_t.accent_sel_text;
        aui_fill_a(x + left - sx + ax, cy, bx - ax, TF_CARET_H, sel & 0xFFFFFFu, (int)(sel >> 24));
    }
    if (n > 0) aui_text_vc(tr, buf, TF_PX, AUI_W_REGULAR, aui_t.label_primary, AUI_ALIGN_LEFT);
    if (foc) {
        /* The caret wakes only its own rect: nothing else in the frame changes
         * with the blink, so the draw-call diff reports exactly this call. The
         * loop is requested only while the blink runs; after TF_IDLE_MS the
         * caret is solid and the field asks for nothing. */
        unsigned since = frame_ms - tf_touch;
        int al = tf_caret_alpha(since, anim_reduced);
        if (!anim_reduced && since < TF_IDLE_MS) anim_loop_want = 1;
        if (al > 0)
            aui_fill_a(x + left - sx + tfw(buf, tf_caret), cy, tf_caret_w(), TF_CARET_H,
                       aui_t.accent, al);
    }
    clip_pop();

    if (has_clear) {
        unsigned c = aui_t.label_tertiary;
        aui_round_a(clear_x, clear_y, 16, 16, 8, c & 0xFFFFFFu, clear_hot ? 176 : 128);
        gui_icon(GICON_CLOSE, X_(clear_x) + 4, Y_(clear_y) + 4, 8, dark ? aui_t.bg_content : 0xFFFFFF);
    }

    if (error && *error && !search)
        aui_text_w(x, y + h + 4, 12, AUI_W_REGULAR, aui_t.error_ink, error);
    return ret;
}

int aui_textfield_exf(int x, int y, int w, char *buf, int cap, const char *placeholder,
                      int enabled, const char *error, int flags)
{ return fld_core(x, y, w, buf, cap, placeholder, enabled, error, flags, 0); }

int aui_textfield_exe(int x, int y, int w, char *buf, int cap, const char *placeholder,
                      int enabled, const char *error)
{ return fld_core(x, y, w, buf, cap, placeholder, enabled, error, 0, 0); }

int (aui_textfield_ex)(int x, int y, int w, char *buf, int cap, const char *placeholder, int enabled)
{ return fld_core(x, y, w, buf, cap, placeholder, enabled, 0, 0, 0); }

int aui_textfield_ph(int x, int y, int w, char *buf, int cap, const char *placeholder)
{ return fld_core(x, y, w, buf, cap, placeholder, 1, 0, 0, 0); }

int aui_textfield(int x, int y, int w, char *buf, int cap)
{ return fld_core(x, y, w, buf, cap, 0, 1, 0, 0, 0); }

int aui_searchfield_ph(int x, int y, int w, char *buf, int cap, int flags, const char *placeholder)
{ return fld_core(x, y, w, buf, cap, placeholder, 1, 0, flags, 1); }

int aui_searchfield(int x, int y, int w, char *buf, int cap, int flags)
{ return fld_core(x, y, w, buf, cap, "搜索", 1, 0, flags, 1); }

/* ---- scrolling: the overlay scrollbar (design 4.12) ----
 *
 * Hidden at rest. It shows while the offset moves (wheel, drag, easing) and
 * while the pointer is in the 12 pt edge zone, then fades over 200 ms once it
 * has been idle for 800 ms. The thumb is 6 pt wide (8 in the zone), a capsule
 * 2 pt in from the edge, #000 @ 90 (light) / #FFF @ 102 (dark), at least 32 pt
 * long; the track shows only while the pointer is in the zone. There is NO
 * gutter: content runs the full width and the 12 pt zone is only a hit zone.
 *
 * Per-bar state is a touch time keyed by the offset's address, like the scroll
 * motion slots. The fade asks for the 20 Hz loop only while it is running --
 * from the last touch to its end -- and nothing once the bar is hidden. */
#define SB_ZONE     12
#define SB_THUMB    6
#define SB_THUMB_HV 8
#define SB_INSET    2
#define SB_MIN      32
#define SB_REST_MS  800
#define SB_FADE_MS  200
#define SB_SLOTS    16

static struct sb_slot {
    int *owner;
    unsigned touch, gen;
    int has, last_off;
} sb_tab[SB_SLOTS];

static struct sb_slot *sb_for(int *off)
{
    struct sb_slot *free_slot = 0, *oldest = &sb_tab[0];
    for (int i = 0; i < SB_SLOTS; i++) {
        struct sb_slot *s = &sb_tab[i];
        if (s->owner == off) return s;
        if (!s->owner && !free_slot) free_slot = s;
        if ((unsigned)(anim_gen - s->gen) > (unsigned)(anim_gen - oldest->gen)) oldest = s;
    }
    struct sb_slot *s = free_slot ? free_slot : oldest;
    s->owner = off; s->has = 0; s->touch = 0; s->last_off = *off;
    return s;
}

/* The bar's opacity 0..255 from the time since its last touch. Pure. */
static int sb_alpha(unsigned dt, int has, int reduced)
{
    if (!has) return 0;
    if (dt < SB_REST_MS) return 255;
    if (reduced || dt >= SB_REST_MS + SB_FADE_MS) return 0;
    return (int)((SB_REST_MS + SB_FADE_MS - dt) * 255u / SB_FADE_MS);
}

static int scrollbar_thumb(int h, int content, int view)
{ return imin(h, imax(SB_MIN, (int)((long long)h * view / content))); }

/* The zone is [zx, zx+12) by [ty - ext, ty + th + ext). A press in it that finds
 * the bar visible (or continues a drag) moves the thumb; on a hidden bar the
 * press belongs to whatever is under it. Taking the press also takes the EVENT,
 * so the rows beneath, which are polled after, do not select. */
static int scrollbar_input(int id, int zx, int ty, int th, int ext, int *off,
                           int content, int view, int *state, int grab)
{
    struct wres r = wpoll(id, X_(zx), Y_(ty - ext), SB_ZONE, th + 2 * ext, 1, 0);
    *state = r.st;
    if (r.clicked && !grab) { in.active = 0; r.clicked = 0; }
    int direct = r.clicked || (in.active == id && in.down);
    if (direct) {
        int thumb = scrollbar_thumb(th, content, view), span = th - thumb;
        long long t = (long long)in.my - Y_(ty) - thumb / 2;
        *off = aui_scroll_clamp(span > 0 ? t * (content - view) / span : 0,
                                content - view);
        if (r.clicked && in.ev == EV_MOUSE) in.ev = EV_NONE;
    }
    return direct;
}

/* Touch bookkeeping for one frame; returns the opacity to draw at. */
static int sb_update(struct sb_slot *s, int *off, int state, int direct, int wheel)
{
    int hov = (state & (AUI_HOVER | AUI_ACTIVE)) != 0;
    if (*off != s->last_off || direct || wheel || hov) { s->touch = frame_ms; s->has = 1; }
    s->last_off = *off;
    s->gen = anim_gen;
    unsigned dt = frame_ms - s->touch;
    int a = hov ? 255 : sb_alpha(dt, s->has, anim_reduced);
    /* The hide is a state change, so it is scheduled even under Reduce Motion
     * (aui_anim_loop() would refuse there): the bar must not stay on screen. */
    if (!hov && s->has && dt < SB_REST_MS + SB_FADE_MS) anim_loop_want = 1;
    return a;
}

static void scrollbar_draw(int zx, int ty, int th, int off, int content, int view,
                           int state, int fade)
{
    if (fade <= 0) return;
    int hov = (state & (AUI_HOVER | AUI_ACTIVE)) != 0;
    int thumb = scrollbar_thumb(th, content, view), span = th - thumb;
    int tpos = (int)((long long)aui_scroll_clamp(off, content - view) * span / (content - view));
    int tw = hov ? SB_THUMB_HV : SB_THUMB;
    int base = aui_is_dark() ? 102 : 90;
    if (hov)
        aui_round_a(zx, ty, SB_ZONE, th, SB_ZONE / 2, aui_t.fill_control, 128 * fade / 255);
    aui_round_a(zx + SB_ZONE - SB_INSET - tw, ty + tpos, tw, thumb, tw / 2,
                aui_is_dark() ? 0xFFFFFF : 0x000000, base * fade / 255);
}

/* A free-standing bar: `x` is the left of its 12 pt zone, `y`/`h` its track. */
int aui_scrollbar(int x, int y, int h, int *off, int content, int view)
{
    int id = ++id_ctr, state = 0, old = *off;
    if (content <= view || h <= 0) { *off = 0; return old != 0; }
    *off = aui_scroll_clamp(*off, content - view);
    struct sb_slot *s = sb_for(off);
    int grab = sb_alpha(frame_ms - s->touch, s->has, anim_reduced) > 0;
    int direct = scrollbar_input(id, x, y, h, 0, off, content, view, &state, grab);
    int fade = sb_update(s, off, state, direct, 0);
    scrollbar_draw(x, y, h, *off, content, view, state, fade);
    return old != *off;
}

static struct {
    int x, y, w, h, sx, sy, state, show_bar;
    int *off, content, fade;
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
     * its zone cannot fit. Skipping begin would leave child drawing
     * unbounded during a small resize. */
    int show_bar = maxo > 0 && w >= 14 && h >= 5;
    int state = 0, direct = 0;
    struct scroll_slot *s = scroll_slot_for(off);
    int fresh = s->gen + 1 != anim_gen;
    struct sb_slot *sb = show_bar ? sb_for(off) : 0;
    /* Poll before painting the content, draw the thumb after it. The old
     * end-only poll moved the thumb but left the content one frame behind;
     * on mouse-up there might never be another frame to repair that mismatch.
     * The zone is polled first, and a press on a VISIBLE bar takes the event,
     * so the children cannot take the drag. */
    if (show_bar) {
        int grab = sb_alpha(frame_ms - sb->touch, sb->has, anim_reduced) > 0;
        direct = scrollbar_input(++id_ctr, x + w - SB_ZONE, y + SB_INSET, h - 2 * SB_INSET,
                                 SB_INSET, off, content_h, h, &state, grab);
    }
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
    scr[scrn].fade = show_bar ? sb_update(sb, off, state, direct, delta != 0) : 0;
    scr[scrn].moving = s->motion.running;
    scrn++;
    /* Wheel hit testing and content clipping are the same rect: no gutter. */
    s->viewport = clip_intersect(aui_r(wx, wy, w, h));
    {
        /* The container's own corners, all four, whether or not a bar is there:
         * the thumb floats 2 pt inside the clip. */
        int rr = radius > 0 ? imin(radius, imin(w, h) / 2) : 0;
        int rad[4] = { rr, rr, rr, rr };
#ifdef UI_FIELDS_NEGCTL
        /* The control: the old 12 pt gutter, content narrower than its box. */
        clip_push_r4(aui_r(wx, wy, show_bar ? w - 12 : w, h), rad);
#else
        clip_push_r4(aui_r(wx, wy, w, h), rad);
#endif
    }
    ox_ = wx; oy_ = wy - *off;
}

/* Added by ui-aui-core: a query of the slot table above, nothing more. A
 * container opened THIS frame (gen == anim_gen) wins over a slot that held the
 * same order last frame and has not been reopened yet. */
int aui_scroll_offset(int id)
{
    const struct scroll_slot *last = 0;
    if (id <= 0) return 0;
    for (int i = 0; i < SCROLL_SLOTS; i++) {
        const struct scroll_slot *s = &scroll_slots[i];
        if (!s->owner || s->order != (unsigned)id) continue;
        if (s->gen == anim_gen) return s->motion.current;
        if (s->gen + 1 == anim_gen) last = s;
    }
    return last ? last->motion.current : 0;
}

void aui_scroll_end(void)
{
    if (scr_skip) { scr_skip--; return; }
    if (!scrn) return;
    scrn--;
    ox_ = scr[scrn].sx; oy_ = scr[scrn].sy;
    clip_pop();
    if (scr[scrn].show_bar)
        scrollbar_draw(scr[scrn].x + scr[scrn].w - SB_ZONE, scr[scrn].y + SB_INSET,
                       scr[scrn].h - 2 * SB_INSET, *scr[scrn].off, scr[scrn].content,
                       scr[scrn].h, scr[scrn].state, scr[scrn].fade);
}
