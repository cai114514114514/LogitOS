/* The clipboard store, on the host, without an emulator.
 *
 * What this can check and the boot test cannot: every boundary, exhaustively,
 * in a second. UTF-8 has 4 sequence lengths, 5 classes of malformed input the
 * validator has to reject, and (for a payload of n bytes) n+1 places a paste
 * can be cut -- and the only interesting property is a statement about ALL of
 * them, not about the three somebody thought to type into a shell.
 *
 * What it deliberately cannot check is that the store survives a process, which
 * is a statement about processes and is therefore tested where processes exist
 * (tests/boot/run-clip-test.sh). Two tests, two claims.
 *
 *     cc -o clipboard_test tests/unit/clipboard_test.c c/kernel/gui/clipboard.c ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logit_abi.h"
#include "clipboard.h"

/* ---- the kernel underneath, stubbed -------------------------------------- */

static long g_alloc_live;                /* outstanding kmalloc blocks */
static int  g_alloc_fail;                /* make the next kmalloc fail */
static int  g_notified;                  /* notify_post calls */

void *kmalloc(size_t n) { if (g_alloc_fail) return NULL; g_alloc_live++; return malloc(n); }
void  kfree(void *p) { if (p) g_alloc_live--; free(p); }

/* The host has one address space, so "is this the caller's memory" is not a
 * question that exists here; these are the identity. The kernel path they stand
 * in for is exercised on the machine, by the boot test, through real syscalls
 * from a real ring-3 process. */
int user_range_ok(const void *p, unsigned long long n, int w) { (void)n; (void)w; return p != NULL; }
int user_range_mapped(const void *p, unsigned long long n, int w) { (void)n; (void)w; return p != NULL; }
int user_copy_from(void *d, const void *s, unsigned long long n) { memcpy(d, s, (size_t)n); return 0; }
int user_copy_to(void *d, const void *s, unsigned long long n) { memcpy(d, s, (size_t)n); return 0; }
int user_copy_string(char *d, int max, const char *s)
{ if (!s) return -1; snprintf(d, (size_t)max, "%s", s); return (int)strlen(d); }

int notify_post(const char *t, const char *b, int l) { (void)t; (void)b; (void)l; return ++g_notified; }

/* ---- harness -------------------------------------------------------------- */

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } } while (0)

#define SET(f, p, n)  clip_syscall(SYS_CLIP_SET,  (f), (long)(p), (n), 7)
#define ADD(f, p, n)  clip_syscall(SYS_CLIP_SET,  (f) | (CLIP_SET_ADD << 16), (long)(p), (n), 7)
#define GET(f, p, n)  clip_syscall(SYS_CLIP_GET,  (f), (long)(p), (n), 7)
#define INFO(q, a)    clip_syscall(SYS_CLIP_INFO, (q), (a), 0, 7)

/* ---- 1. UTF-8 validation on the way in ------------------------------------
 *
 * The store's ONE rule is that whatever is in CLIP_F_TEXT is well-formed UTF-8;
 * everything else it promises rests on that. So the rejections matter as much
 * as the acceptances, and the four classic evasions each get their own case --
 * an overlong form, a surrogate, an out-of-range codepoint and a truncated
 * tail. A validator that only counts continuation bytes accepts all four. */
static void t_utf8_in(void)
{
    struct { const char *bytes; int len; int ok; const char *what; } V[] = {
        { "hello",                     5, 1, "ascii" },
        { "",                          0, 1, "the empty selection is a selection" },
        { "\xC3\xA9",                  2, 1, "2-byte  U+00E9" },
        { "\xE4\xB8\xAD\xE6\x96\x87",  6, 1, "3-byte  CJK" },
        { "\xF0\x9F\x98\x80",          4, 1, "4-BYTE  U+1F600 -- the one a 16-bit"
                                              " clipboard gets wrong" },
        { "\xF4\x8F\xBF\xBF",          4, 1, "4-byte  U+10FFFF, the last codepoint" },

        { "\x80",                      1, 0, "a lone continuation byte" },
        { "\xC3",                      1, 0, "a lead byte with nothing after it" },
        { "\xE4\xB8",                  2, 0, "a 3-byte sequence cut short" },
        { "\xF0\x9F\x98",              3, 0, "a 4-byte sequence cut short" },
        { "\xC0\xAF",                  2, 0, "OVERLONG '/' -- the classic evasion" },
        { "\xE0\x80\xAF",              3, 0, "overlong, 3-byte form" },
        { "\xF0\x80\x80\xAF",          4, 0, "overlong, 4-byte form" },
        { "\xED\xA0\x80",              3, 0, "a SURROGATE, U+D800" },
        { "\xF4\x90\x80\x80",          4, 0, "past U+10FFFF" },
        { "\xF5\x80\x80\x80",          4, 0, "an F5 lead byte, which cannot exist" },
        { "\xFF\xFE",                  2, 0, "a UTF-16 BOM pasted as bytes" },
        { "ok\xC0\xAFno",              7, 0, "bad bytes in the MIDDLE of good text" },
    };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        long r = SET(CLIP_F_TEXT, V[i].bytes, V[i].len);
        if (V[i].ok) CHECK(r == V[i].len, "accepted: %s (got %ld)", V[i].what, r);
        else         CHECK(r == CLIP_E_UTF8, "refused: %s (got %ld)", V[i].what, r);
    }
    /* ...but a byte flavour is bytes. The validation is a property of TEXT, not
     * of the clipboard, or a file path with a stray byte in it could not be
     * carried at all. */
    CHECK(SET(CLIP_F_PATH, "\xFF\xFE", 2) == 2, "a byte flavour takes any bytes");
}

/* ---- 2. the cut, at every offset ------------------------------------------
 *
 * This is the assertion the whole feature turns on. Build a string of mixed
 * 1/2/3/4-byte characters and paste it into a buffer of EVERY size from 0 to
 * its length. At each size the result must be well-formed, must not exceed the
 * buffer, and must not give up more than 3 bytes -- returning 0 would satisfy
 * "never split a character" and be useless.
 *
 * `midcut` counts the sizes where the naive answer would have split something.
 * It is asserted on: a run in which no cut ever landed mid-character would pass
 * every check above while testing nothing at all. */
static void t_cut_everywhere(void)
{
    /* a b é 中 (emoji) c d é 中 (emoji) ... -- every length, repeatedly */
    char src[512];
    int n = 0;
    for (int rep = 0; rep < 12; rep++) {
        src[n++] = (char)('a' + rep);
        memcpy(src + n, "\xC3\xA9", 2);             n += 2;
        memcpy(src + n, "\xE4\xB8\xAD", 3);         n += 3;
        memcpy(src + n, "\xF0\x9F\x98\x80", 4);     n += 4;
    }
    CHECK(SET(CLIP_F_TEXT, src, n) == n, "the mixed-width string went in whole");

    /* An independent validator: the same rules, written again, so that a wrong
     * rule in clipboard.c cannot agree with itself. */
    int midcut = 0, worst = 0;
    for (int max = 0; max <= n; max++) {
        char out[512];
        long got = GET(CLIP_F_TEXT, out, max);
        CHECK(got >= 0, "cut at %d returned %ld", max, got);
        CHECK(got <= max, "cut at %d returned MORE than asked (%ld)", max, got);
        if (max - got > worst) worst = (int)(max - got);
        if (got < max && max < n) midcut++;
        /* well-formed? decode it and see. */
        int i = 0, bad = 0;
        while (i < got) {
            unsigned c = (unsigned char)out[i];
            int need = c < 0x80 ? 0 : (c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : (c >= 0xC0 ? 1 : -1)));
            if (need < 0) { bad = 1; break; }
            if (i + need >= got) { bad = 1; break; }
            for (int k = 1; k <= need; k++)
                if (((unsigned char)out[i + k] & 0xC0) != 0x80) { bad = 1; break; }
            if (bad) break;
            i += need + 1;
        }
        CHECK(!bad, "cut at %d SPLIT a character (returned %ld bytes)", max, got);
        CHECK(memcmp(out, src, (size_t)got) == 0, "cut at %d changed the bytes", max);
    }
    CHECK(midcut > 0, "at least one cut landed mid-character (%d did) -- otherwise "
                      "this test proved nothing", midcut);
    CHECK(worst <= 3, "never gave up more than 3 bytes (worst %d)", worst);
    CHECK(GET(CLIP_F_TEXT, (char[512]){0}, 512) == n, "a big enough buffer gets it all");
}

/* ---- 3. the cap is a refusal, not a truncation ---------------------------- */
static void t_cap(void)
{
    char *big = malloc(CLIP_MAX_BYTES + 16);
    memset(big, 'a', (size_t)CLIP_MAX_BYTES + 16);

    CHECK(SET(CLIP_F_TEXT, big, CLIP_MAX_BYTES) == CLIP_MAX_BYTES, "exactly at the cap is fine");
    long held = INFO(CLIP_Q_LEN, CLIP_F_TEXT);
    int before = g_notified;
    CHECK(SET(CLIP_F_TEXT, big, CLIP_MAX_BYTES + 1) == CLIP_E_TOOBIG, "one byte past it is REFUSED");
    CHECK(INFO(CLIP_Q_LEN, CLIP_F_TEXT) == held,
          "...and the refusal did not disturb what was already there");
    /* The kernel-side notification path, on a real event: the user is told, and
     * the app was told separately by the return value. */
    CHECK(g_notified == before + 1, "a refused copy raised a notification");

    CHECK(SET(CLIP_F_TEXT, big, -1) == CLIP_E_ARG, "a negative length is refused");
    CHECK(SET(99, big, 4) == CLIP_E_ARG, "an unknown flavour is refused");
    CHECK(GET(99, big, 4) == CLIP_E_ARG, "...on the way out too");
    free(big);
}

/* ---- 4. flavours ---------------------------------------------------------- */
static void t_flavours(void)
{
    CHECK(SET(CLIP_F_TEXT, "plain", 5) == 5, "text set");
    CHECK(ADD(CLIP_F_HTML, "<b>plain</b>", 12) == 12, "html ADDED to the same content");
    CHECK(INFO(CLIP_Q_FLAVOURS, 0) == ((1 << CLIP_F_TEXT) | (1 << CLIP_F_HTML)),
          "both flavours are advertised");
    CHECK(INFO(CLIP_Q_LEN, CLIP_F_TEXT) == 5 && INFO(CLIP_Q_LEN, CLIP_F_HTML) == 12,
          "each flavour keeps its own length");
    CHECK(INFO(CLIP_Q_LEN, CLIP_F_URI) == CLIP_E_EMPTY, "an absent flavour says so");
    CHECK(GET(CLIP_F_URI, (char[8]){0}, 8) == CLIP_E_EMPTY, "...and pastes nothing");

    /* A REPLACING set drops the others: they described the old content, and
     * keeping them is how paste-as-HTML yields the previous selection. */
    CHECK(SET(CLIP_F_TEXT, "next", 4) == 4, "a new copy");
    CHECK(INFO(CLIP_Q_FLAVOURS, 0) == (1 << CLIP_F_TEXT), "the stale HTML flavour is GONE");

    long s0 = INFO(CLIP_Q_SERIAL, 0);
    SET(CLIP_F_TEXT, "a", 1);
    CHECK(INFO(CLIP_Q_SERIAL, 0) == s0 + 1, "the serial moves on every set");
    SET(CLIP_F_TEXT, "a", 1);
    CHECK(INFO(CLIP_Q_SERIAL, 0) == s0 + 2, "...even for identical bytes");
    CHECK(INFO(CLIP_Q_OWNER, 0) == 7, "the owner pid is recorded");
    CHECK(INFO(CLIP_Q_MAX, 0) == CLIP_MAX_BYTES, "the cap is readable from the kernel");
}

/* ---- 5. the kernel-side face (Cmd+C / Cmd+V land here) -------------------- */
static void t_kernel_face(void)
{
    CHECK(clip_set_text("\xE4\xB8\xAD\xE6\x96\x87", 6) == 6, "kernel-side set");
    char b[8];
    CHECK(clip_get_text(b, 8) == 6, "kernel-side get");
    CHECK(clip_get_text(b, 4) == 3, "kernel-side get keeps the boundary rule too");
    CHECK(clip_len(CLIP_F_TEXT) == 6, "clip_len agrees");
    CHECK(clip_set_text("\x80", 1) == CLIP_E_UTF8,
          "the shortcut path gets the SAME validation as the syscall -- one "
          "implementation, so the two cannot disagree about the same bytes");
}

/* ---- 6. no leak, and a failing allocator is survivable -------------------- */
static void t_memory(void)
{
    SET(CLIP_F_TEXT, "x", 1);
    long live = g_alloc_live;
    for (int i = 0; i < 200; i++) SET(CLIP_F_TEXT, "abcdefghij", 10);
    CHECK(g_alloc_live == live, "200 sets leak nothing (live %ld, was %ld)", g_alloc_live, live);

    for (int i = 0; i < 50; i++) { SET(CLIP_F_TEXT, "a", 1); ADD(CLIP_F_HTML, "b", 1);
                                  ADD(CLIP_F_URI, "c", 1); ADD(CLIP_F_PATH, "d", 1); }
    SET(CLIP_F_TEXT, "final", 5);
    CHECK(g_alloc_live == 1, "a replacing set frees every other flavour (live %ld)", g_alloc_live);

    long held = INFO(CLIP_Q_LEN, CLIP_F_TEXT);
    g_alloc_fail = 1;
    CHECK(SET(CLIP_F_TEXT, "nope", 4) == CLIP_E_NOMEM, "a failing kmalloc is reported");
    g_alloc_fail = 0;
    CHECK(INFO(CLIP_Q_LEN, CLIP_F_TEXT) == held,
          "...and the OLD content is still there -- the store is never dropped "
          "before the replacement exists");
}

int main(void)
{
    printf("clipboard: UTF-8 in\n");            t_utf8_in();
    printf("clipboard: the cut, at every offset\n"); t_cut_everywhere();
    printf("clipboard: the cap\n");             t_cap();
    printf("clipboard: flavours\n");            t_flavours();
    printf("clipboard: the kernel-side face\n"); t_kernel_face();
    printf("clipboard: memory\n");              t_memory();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
