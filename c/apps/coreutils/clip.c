#include "clib.h"

/* clip -- the clipboard from the shell, and the instrument the clipboard tests
 * are built out of.
 *
 * It is a demonstration second and a TEST HARNESS first, which is why it has
 * subcommands nobody would type. The claim being tested is that the clipboard
 * crosses a process boundary and survives the death of the process that filled
 * it; the only way to test that is to fill it in one process, let that process
 * EXIT, and read it back in a different one. Two runs of this program under
 * /bin/sh are two real processes, so:
 *
 *     clip gen 4096          process A: puts a known payload on the clipboard
 *     clip verify 4096       process B: regenerates it and compares every byte
 *
 * The payload is generated rather than passed, deliberately. A CJK string typed
 * into a serial console is a claim about the console's byte handling as much as
 * about the clipboard, and when it fails you cannot tell which. Generating the
 * same bytes independently in both processes takes the console out of the loop
 * entirely -- what is compared is what the clipboard returned against what the
 * clipboard was given, and nothing else is in the path.
 *
 *   clip copy TEXT...   put text on the clipboard (arguments joined by spaces)
 *   clip paste          write the text flavour to stdout
 *   clip info           what is on it, without moving the payload
 *   clip gen N          put the deterministic N-byte UTF-8 pattern on it
 *   clip verify N       read it back and compare, byte for byte
 *   clip trunc          EVERY short-buffer paste of the current text, checked
 *                       for a split character
 *   clip big N          try to set N bytes; report what the cap did
 *   clip html TEXT...   add an HTML flavour to the current content
 */

#define BUF_MAX (CLIP_MAX_BYTES + 8)
static char buf[BUF_MAX];
static char gen[BUF_MAX];

/* The test payload. ASCII, then a 2-byte codepoint, a 3-byte one and a 4-byte
 * one, cycling -- so that a naive cut at an arbitrary offset lands inside a
 * multi-byte sequence most of the time, and inside a FOUR-byte one often
 * enough to matter. Deterministic in `n` and computed identically in both
 * processes; that is the whole point.
 *
 *   'a'..'z'            1 byte
 *   U+00E9  é           2 bytes  C3 A9
 *   U+4E2D  中          3 bytes  E4 B8 AD
 *   U+1F600 (emoji)     4 bytes  F0 9F 98 80
 *
 * Returns the length actually produced: it only ever emits WHOLE characters, so
 * the result may be a byte or three short of `n`. That is the correct thing for
 * a UTF-8 generator to do and it is also the first thing the cap test needs. */
static int payload(char *d, int n)
{
    /* Three entries, indexed 0..2, and the modulus below is 6 so that
     * `pick - 3` cannot leave the array. The first version cycled mod 7 into a
     * 4-entry table with an unused leading "" and read one past the end -- the
     * generator crashed with a null read on the machine before it had put a
     * single byte on the clipboard, which is precisely the sort of thing a
     * test that only ever ran on the host would not have found. */
    static const char *SEQ[3] = { "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80" };
    int i = 0;
    unsigned k = 0;
    while (i < n) {
        int pick = (int)(k % 6);
        const char *s;
        char one[2];
        if (pick < 3) { one[0] = (char)('a' + (k % 26)); one[1] = 0; s = one; }
        else          { s = SEQ[pick - 3]; }
        int l = c_strlen(s);
        if (i + l > n) break;              /* never emit a partial character */
        for (int j = 0; j < l; j++) d[i + j] = s[j];
        i += l;
        k++;
    }
    return i;
}

/* Well-formed UTF-8? The same strictness the kernel applies on the way in --
 * duplicated here ON PURPOSE. A validator that shares code with the thing it is
 * checking agrees with it by construction, including where both are wrong; two
 * independent implementations that agree is evidence. */
static int utf8_ok(const unsigned char *s, int n)
{
    int i = 0;
    while (i < n) {
        unsigned c = s[i];
        int need; unsigned lo, hi;
        if (c < 0x80)                    { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { need = 1; lo = 0x80; hi = 0xBF; }
        else if (c == 0xE0)              { need = 2; lo = 0xA0; hi = 0xBF; }
        else if (c >= 0xE1 && c <= 0xEC) { need = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xED)              { need = 2; lo = 0x80; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { need = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF0)              { need = 3; lo = 0x90; hi = 0xBF; }
        else if (c >= 0xF1 && c <= 0xF3) { need = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF4)              { need = 3; lo = 0x80; hi = 0x8F; }
        else return 0;
        if (i + need >= n) return 0;
        if (s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (int k = 2; k <= need; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}

static void say(const char *tag, long v) { outs(tag); outn(v); outc('\n'); }

int main(int argc, char **argv)
{
    if (argc < 2) { errs("usage: clip copy|paste|info|gen|verify|trunc|big|html\n"); return 2; }
    const char *cmd = argv[1];

    if (c_streq(cmd, "copy")) {
        int n = 0;
        for (int i = 2; i < argc && n < BUF_MAX - 2; i++) {
            for (int j = 0; argv[i][j] && n < BUF_MAX - 2; j++) buf[n++] = argv[i][j];
            if (i + 1 < argc) buf[n++] = ' ';
        }
        int r = clip_set(CLIP_F_TEXT, buf, n);
        if (r < 0) { say("CLIP_SET_ERR ", r); return 1; }
        say("CLIP_COPIED ", r);
        return 0;
    }

    if (c_streq(cmd, "paste")) {
        int n = clip_get(CLIP_F_TEXT, buf, BUF_MAX);
        if (n < 0) { say("CLIP_GET_ERR ", n); return 1; }
        sys_write(1, buf, n);
        outc('\n');
        return 0;
    }

    if (c_streq(cmd, "html")) {
        int n = 0;
        for (int i = 2; i < argc && n < BUF_MAX - 2; i++) {
            for (int j = 0; argv[i][j] && n < BUF_MAX - 2; j++) buf[n++] = argv[i][j];
            if (i + 1 < argc) buf[n++] = ' ';
        }
        int r = clip_add(CLIP_F_HTML, buf, n);
        if (r < 0) { say("CLIP_ADD_ERR ", r); return 1; }
        say("CLIP_HTML_ADDED ", r);
        return 0;
    }

    if (c_streq(cmd, "info")) {
        outs("CLIP_FLAVOURS "); outn(clip_flavours());
        outs(" TEXT "); outn(clip_len(CLIP_F_TEXT));
        outs(" HTML "); outn(clip_len(CLIP_F_HTML));
        outs(" SERIAL "); outn(clip_serial());
        outs(" OWNER "); outn(clip_owner());
        outs(" PID "); outn(sys_getpid());
        outs(" MAX "); outn(clip_max());
        outc('\n');
        return 0;
    }

    if (c_streq(cmd, "gen")) {
        int want = argc > 2 ? c_atoi(argv[2]) : 4096;
        if (want < 0 || want > CLIP_MAX_BYTES) want = CLIP_MAX_BYTES;
        int n = payload(gen, want);
        int r = clip_set(CLIP_F_TEXT, gen, n);
        if (r != n) { say("CLIP_GEN_ERR ", r); return 1; }
        outs("CLIP_GEN_OK bytes "); outn(n);
        outs(" pid "); outn(sys_getpid());
        outc('\n');
        return 0;
    }

    if (c_streq(cmd, "verify")) {
        int want = argc > 2 ? c_atoi(argv[2]) : 4096;
        if (want < 0 || want > CLIP_MAX_BYTES) want = CLIP_MAX_BYTES;
        int n = payload(gen, want);
        int held = clip_len(CLIP_F_TEXT);
        int got = clip_get(CLIP_F_TEXT, buf, BUF_MAX);
        if (got < 0) { say("CLIP_VERIFY_BAD get ", got); return 1; }
        if (held != n) { outs("CLIP_VERIFY_BAD len "); outn(held);
                         outs(" want "); outn(n); outc('\n'); return 1; }
        if (got != n)  { outs("CLIP_VERIFY_BAD got "); outn(got);
                         outs(" want "); outn(n); outc('\n'); return 1; }
        for (int i = 0; i < n; i++)
            if (buf[i] != gen[i]) {
                outs("CLIP_VERIFY_BAD byte "); outn(i);
                outs(" got "); outn((unsigned char)buf[i]);
                outs(" want "); outn((unsigned char)gen[i]); outc('\n');
                return 1;
            }
        /* Byte-for-byte, never a length check: a clipboard that returned the
         * right NUMBER of someone else's bytes passes a length check. */
        outs("CLIP_VERIFY_OK bytes "); outn(n);
        outs(" pid "); outn(sys_getpid());
        outs(" owner "); outn(clip_owner());
        outc('\n');
        return 0;
    }

    if (c_streq(cmd, "trunc")) {
        /* EVERY short-buffer paste of whatever is on the clipboard, checked.
         *
         * The interesting cases are the ones where the requested cut falls
         * inside a multi-byte character; `splits` counts them, and a run where
         * splits == 0 would mean this test proved nothing, so it is printed and
         * asserted on rather than left implicit. For each cut:
         *   - the returned prefix must be well-formed UTF-8 (independently
         *     validated, see utf8_ok above)
         *   - it must not exceed what was asked for
         *   - it must give up at most 3 bytes, because that is the longest a
         *     trailing partial character can be. Returning 0 would satisfy
         *     "never split a character" and be useless. */
        int held = clip_len(CLIP_F_TEXT);
        if (held <= 0) { errs("clip trunc: clipboard is empty\n"); return 1; }
        int bad = 0, splits = 0, worst = 0;
        for (int max = 1; max <= held; max++) {
            int got = clip_get(CLIP_F_TEXT, buf, max);
            if (got < 0) { say("CLIP_TRUNC_BAD get ", got); return 1; }
            if (got > max) { outs("CLIP_TRUNC_BAD over "); outn(max); outc('\n'); bad++; break; }
            if (max - got > worst) worst = max - got;
            if (max - got > 0) splits++;
            if (!utf8_ok((const unsigned char *)buf, got)) {
                outs("CLIP_TRUNC_BAD split at max "); outn(max);
                outs(" got "); outn(got); outc('\n');
                bad++;
                break;
            }
            if (max - got > 3) {
                outs("CLIP_TRUNC_BAD gave up "); outn(max - got);
                outs(" bytes at max "); outn(max); outc('\n');
                bad++;
                break;
            }
        }
        if (bad) return 1;
        if (!splits) { errs("CLIP_TRUNC_BAD no cut ever landed mid-character\n"); return 1; }
        outs("CLIP_TRUNC_OK cuts "); outn(held);
        outs(" midchar "); outn(splits);
        outs(" worst "); outn(worst);
        outc('\n');
        return 0;
    }

    if (c_streq(cmd, "big")) {
        /* The cap, enforced rather than assumed. Exactly at the cap must be
         * accepted and one byte past it must be refused with CLIP_E_TOOBIG --
         * NOT silently truncated, which is the failure this asserts against.
         * A refusal is checked by re-reading what is on the clipboard: an
         * implementation that returned an error AND clobbered the content
         * would pass a check on the return value alone. */
        int cap = clip_max();
        int at = payload(gen, cap);
        int r1 = clip_set(CLIP_F_TEXT, gen, at);
        int before = clip_len(CLIP_F_TEXT);
        int over = cap + 1;
        if (over > BUF_MAX) over = BUF_MAX;
        for (int i = 0; i < over; i++) buf[i] = 'x';
        int r2 = clip_set(CLIP_F_TEXT, buf, over);
        int after = clip_len(CLIP_F_TEXT);
        outs("CLIP_BIG cap "); outn(cap);
        outs(" at "); outn(r1);
        outs(" over "); outn(r2);
        outs(" held_before "); outn(before);
        outs(" held_after "); outn(after);
        outc('\n');
        if (r1 != at || r2 != CLIP_E_TOOBIG || after != before) {
            outs("CLIP_BIG_BAD\n");
            return 1;
        }
        /* And bad UTF-8 is refused too, which is what makes the boundary rule
         * on the way out sound: a lone continuation byte is not text. */
        buf[0] = (char)0x80; buf[1] = 'x';
        int r3 = clip_set(CLIP_F_TEXT, buf, 2);
        int r4 = clip_set(CLIP_F_PATH, buf, 2);   /* ...but not for a byte flavour */
        outs("CLIP_BIG_UTF8 text "); outn(r3);
        outs(" path "); outn(r4); outc('\n');
        if (r3 != CLIP_E_UTF8 || r4 != 2) { outs("CLIP_BIG_BAD\n"); return 1; }
        outs("CLIP_BIG_OK\n");
        return 0;
    }

    errs("clip: unknown subcommand\n");
    return 2;
}
