/* The system clipboard.
 *
 * The design arguments -- who owns the bytes, what happens when the copying
 * process dies, why the cap is a refusal and not a truncation, and why the
 * flavour mechanism exists before its second flavour does -- are written above
 * SYS_CLIP_SET in include/abi/logit_abi.h, because they are properties of the
 * INTERFACE and an app author has to be able to read them without reading this.
 * What is argued here is only what is specific to the implementation.
 *
 * THE ONE RULE THIS FILE EXISTS TO KEEP:
 *
 *     Whatever is in CLIP_F_TEXT is well-formed UTF-8, and every prefix this
 *     file ever hands out ends on a character boundary.
 *
 * Both halves are needed and neither is sufficient. Validating on the way in
 * without truncating safely on the way out means a short paste buffer still
 * produces a broken character. Truncating safely without validating means the
 * truncation walks a byte sequence whose structure it has merely assumed --
 * and a "continuation byte" scan over arbitrary bytes can walk backwards past
 * the start of the buffer, which is a kernel bug rather than a mojibake bug.
 * Together they are an invariant a consumer can rely on, which is the only
 * reason a paste into a fixed-size text field can be correct at all.
 *
 * This system renders CJK. A clipboard that mangles multi-byte text is worse
 * than no clipboard, because no clipboard fails visibly and this one would not.
 */

#include <stddef.h>
#include <stdint.h>

#include "logit_abi.h"
#include "clipboard.h"
#include "kheap.h"
#include "usercopy.h"
#include "notify.h"

/* ---- the store ------------------------------------------------------------
 *
 * One kmalloc'd buffer per flavour, owned by this file and by nothing else.
 * `p == NULL` is "this flavour is not present" -- there is no separate presence
 * flag to fall out of step with the pointer.
 *
 * A zero-length payload is a legitimate thing to put on a clipboard (select
 * nothing, copy), so presence is the pointer and NOT `len > 0`; kmalloc(0)
 * would blur that, hence the +1 on every allocation below. The spare byte is
 * also what makes the store safe for a consumer that wants a C string. */
static struct {
    char *p;
    int   len;
} g_flav[CLIP_NFLAVOUR];

static unsigned g_serial;      /* bumps on every successful set */
static int      g_owner_pid;   /* informational only -- see CLIP_Q_OWNER */

/* ---- UTF-8 ---------------------------------------------------------------
 *
 * Strict, and strict in the four ways that matter, because "does it look like
 * UTF-8" is what lets bad bytes in:
 *
 *   - no overlong forms (0xC0/0xC1 lead bytes, E0 80 .., F0 80 ..) -- an
 *     overlong encoding of '/' or NUL is the classic way a validator and a
 *     decoder are made to disagree about the same bytes;
 *   - no surrogates (U+D800..U+DFFF, i.e. ED A0 .. ED BF ..) -- they are not
 *     characters, and a text stack that accepts them here has to handle them
 *     everywhere downstream;
 *   - nothing above U+10FFFF (lead bytes F5..FF, and F4 90 and up);
 *   - no truncated sequence at the end of the buffer.
 *
 * The awkward-looking table of per-lead-byte second-byte ranges is exactly
 * where the first three of those live. A version that only checked "lead byte,
 * then N continuation bytes" would accept all three classes.
 */
static int utf8_valid(const unsigned char *s, int n)
{
    int i = 0;
    while (i < n) {
        unsigned c = s[i];
        int need;
        unsigned lo, hi;                    /* legal range of the SECOND byte */
        if (c < 0x80)                  { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { need = 1; lo = 0x80; hi = 0xBF; }
        else if (c == 0xE0)              { need = 2; lo = 0xA0; hi = 0xBF; }
        else if (c >= 0xE1 && c <= 0xEC) { need = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xED)              { need = 2; lo = 0x80; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { need = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF0)              { need = 3; lo = 0x90; hi = 0xBF; }
        else if (c >= 0xF1 && c <= 0xF3) { need = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF4)              { need = 3; lo = 0x80; hi = 0x8F; }
        else return 0;                  /* 0x80..0xC1 and 0xF5..0xFF: never a lead */
        /* The sequence occupies s[i .. i+need], so it needs i+need <= n-1. */
        if (i + need >= n) return 0;                  /* truncated tail */
        if (s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (int k = 2; k <= need; k++)
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}

/* THE NEGATIVE CONTROL for the boundary rule. Built with
 * -DCLIP_NAIVE_TRUNC (make test-clip-negctl), clip_get truncates at exactly the
 * byte count asked for, which is the obvious implementation and the wrong one:
 * it splits whatever character straddles the cut. `clip trunc` then reports a
 * malformed prefix and the boot test MUST fail. An assertion nobody has watched
 * fail is not a known-failing assertion, so this exists to be watched. */
#ifdef CLIP_NAIVE_TRUNC
#define CLIP_TRUNC_IS_NAIVE 1
#else
#define CLIP_TRUNC_IS_NAIVE 0
#endif

/* The largest k <= cut such that p[0..k) is a whole number of characters.
 *
 * Only correct because the buffer was validated at set time: it walks back over
 * continuation bytes, and the guarantee that it meets a lead byte within three
 * steps (rather than running off the front of the buffer) IS the validation.
 * The k > 0 guard is belt and braces, not the reason it terminates. */
static int utf8_cut(const unsigned char *p, int len, int cut)
{
    if (cut >= len) return len;
    if (cut <= 0) return 0;
    if (CLIP_TRUNC_IS_NAIVE) return cut;
    int k = cut;
    while (k > 0 && (p[k] & 0xC0) == 0x80) k--;
    return k;
}

/* ---- the store's one mutator ---------------------------------------------- */

static void flav_drop(int f)
{
    if (g_flav[f].p) kfree(g_flav[f].p);
    g_flav[f].p = NULL;
    g_flav[f].len = 0;
}

/* Install `n` bytes (already in kernel memory, already validated) as `f`.
 * Takes ownership of `buf`. */
static void flav_install(int f, char *buf, int n, int add, int pid)
{
    if (!add)
        for (int i = 0; i < CLIP_NFLAVOUR; i++) flav_drop(i);
    else
        flav_drop(f);
    g_flav[f].p = buf;
    g_flav[f].len = n;
    g_serial++;
    g_owner_pid = pid;
}

/* The single set path. `from_user` says whether `src` needs user_copy_from;
 * everything after the copy is identical, which is the point -- there is one
 * cap check and one validator, so the keyboard shortcut and the syscall cannot
 * come to different conclusions about the same bytes. */
static long clip_set_common(int flavour, int flags, const char *src, int len,
                            int from_user, int pid)
{
    if (flavour < 0 || flavour >= CLIP_NFLAVOUR) return CLIP_E_ARG;
    if (len < 0) return CLIP_E_ARG;
    if (len > CLIP_MAX_BYTES) {
        /* The kernel-side notification path, on a real event rather than a
         * manufactured one: a refusal the user would otherwise experience as
         * "copy did nothing". Nothing about this is a dialog -- the copy has
         * already failed and the app has already been told; this is the part
         * the PERSON needs. */
        notify_post("Clipboard", "Copy refused: too large for the clipboard",
                    NOTIFY_WARN);
        return CLIP_E_TOOBIG;
    }
    if (len && !src) return CLIP_E_ARG;
    if (from_user && len && !user_range_ok(src, (uint64_t)len, 0)) return CLIP_E_ARG;

    char *buf = kmalloc((size_t)len + 1);
    if (!buf) return CLIP_E_NOMEM;
    if (len) {
        if (from_user) {
            if (user_copy_from(buf, src, (uint64_t)len) < 0) { kfree(buf); return CLIP_E_ARG; }
        } else {
            for (int i = 0; i < len; i++) buf[i] = src[i];
        }
    }
    buf[len] = 0;

    /* Validate AFTER the copy, never before it. A check on the user's buffer
     * followed by a copy of the user's buffer is a time-of-check/time-of-use
     * window: another thread of the same process can rewrite those bytes in
     * between, and the invariant this whole file rests on would then be a
     * statement about bytes that are no longer there. */
    if (flavour == CLIP_F_TEXT && !utf8_valid((const unsigned char *)buf, len)) {
        kfree(buf);
        return CLIP_E_UTF8;
    }

    flav_install(flavour, buf, len, (flags & CLIP_SET_ADD) != 0, pid);
    return len;
}

static long clip_get_common(int flavour, char *dst, int max, int to_user)
{
    if (flavour < 0 || flavour >= CLIP_NFLAVOUR) return CLIP_E_ARG;
    if (max < 0) return CLIP_E_ARG;
    if (!g_flav[flavour].p) return CLIP_E_EMPTY;
    if (max == 0) return 0;
    if (!dst) return CLIP_E_ARG;
    if (to_user && !user_range_ok(dst, (uint64_t)max, 1)) return CLIP_E_ARG;

    const unsigned char *p = (const unsigned char *)g_flav[flavour].p;
    int len = g_flav[flavour].len;
    int n = max < len ? max : len;
    /* Only the text flavour has characters to split. The others are byte
     * strings by definition and a prefix of one is a prefix. */
    if (flavour == CLIP_F_TEXT) n = utf8_cut(p, len, n);
    if (n <= 0) return 0;
    if (to_user) {
        if (user_copy_to(dst, p, (uint64_t)n) < 0) return CLIP_E_ARG;
    } else {
        for (int i = 0; i < n; i++) dst[i] = (char)p[i];
    }
    return n;
}

/* ---- the kernel-side face (Cmd+C / Cmd+V land here) ----------------------- */

int clip_set_text(const char *s, int len)
{ return (int)clip_set_common(CLIP_F_TEXT, 0, s, len, 0, 0); }

int clip_get_text(char *buf, int max)
{
    long r = clip_get_common(CLIP_F_TEXT, buf, max, 0);
    return r < 0 ? 0 : (int)r;      /* "no text" and "empty text" both paste nothing */
}

int clip_len(int flavour)
{
    if (flavour < 0 || flavour >= CLIP_NFLAVOUR) return 0;
    return g_flav[flavour].p ? g_flav[flavour].len : 0;
}

/* ---- the syscall back end ------------------------------------------------- */

long clip_syscall(long num, long a, long b, long c, int pid)
{
    switch (num) {
    case SYS_CLIP_SET:
        /* flavour in the low half, flags in the high half: one argument, so the
         * other two stay the buffer and its length. */
        return clip_set_common((int)(a & 0xFFFF), (int)((a >> 16) & 0xFFFF),
                               (const char *)b, (int)c, 1, pid);
    case SYS_CLIP_GET:
        return clip_get_common((int)a, (char *)b, (int)c, 1);
    case SYS_CLIP_INFO:
        switch ((int)a) {
        case CLIP_Q_FLAVOURS: {
            long m = 0;
            for (int i = 0; i < CLIP_NFLAVOUR; i++) if (g_flav[i].p) m |= 1L << i;
            return m;
        }
        case CLIP_Q_LEN:
            if ((int)b < 0 || (int)b >= CLIP_NFLAVOUR) return CLIP_E_ARG;
            return g_flav[(int)b].p ? g_flav[(int)b].len : CLIP_E_EMPTY;
        case CLIP_Q_SERIAL: return (long)g_serial;
        case CLIP_Q_OWNER:  return g_owner_pid;
        case CLIP_Q_MAX:    return CLIP_MAX_BYTES;
        default:            return CLIP_E_ARG;
        }
    default:
        return CLIP_E_ARG;
    }
}
