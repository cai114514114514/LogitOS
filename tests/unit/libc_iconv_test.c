/* iconv_open/iconv/iconv_close case list, PLUS (see the LANGINFO section near
 * the end of main()) a real diffed gate for nl_langinfo() -- same two-build
 * diff strategy as libc_fnmatch_test.c (tests/libc.mk compiles this once
 * against our own <iconv.h>/iconv.c (+<langinfo.h>/langinfo.c), once as a
 * normal host program against glibc's real iconv()/nl_langinfo(), and diffs
 * stdout byte for byte). See <iconv.h> for the supported charset list and the
 * BOM/{IGNORE,TRANSLIT} policy this file pins.
 *
 * WHY nl_langinfo() IS HERE AND NOT IN A FILE OF ITS OWN: this unit
 * (u8-iconv-langinfo) was allowed exactly one test file
 * (tests/unit/libc_iconv_test.c), and the original implementation of this
 * unit used it only for iconv, leaving nl_langinfo()/catopen()/catgets()/
 * catclose() "independently hand-verified... not part of the graded gate"
 * (see the unit's own report) -- despite langinfo.h's and langinfo.c's file
 * headers BOTH claiming "pinned by tests/unit/libc_iconv_test.c's host diff",
 * which was never true (grep confirms zero calls to nl_langinfo anywhere in
 * the original file). That is a real documentation defect, not just a
 * missing test -- a future reader trusting the header comment over the JSON
 * report would believe protection exists that doesn't. Fixed two ways: the
 * false claim in langinfo.h/langinfo.c is corrected to say what's actually
 * true, and the LANGINFO section below makes it true by actually adding
 * nl_langinfo() to this file's diff. catopen/catgets/catclose remain
 * hand-verified only -- see <nl_types.h>, their contract really has no
 * glibc oracle to diff against (a catalog is never available here; a real
 * catalog-backed glibc run cannot exercise the same "always ENOENT" path).
 *
 * BUILD REQUIREMENT THIS ADDITION INTRODUCED, IN BOLD BECAUSE IT FAILS
 * SILENTLY: the "ours" build of this file MUST now also link
 * c/apps/libc/src/langinfo.c AND c/apps/libc/src/locale.c (RADIXCHAR/THOUSEP
 * read localeconv(), see langinfo.c). The original gate_command for this
 * unit (iconv.c + wchar.c + malloc.c + stdlib.c + dtoa.c +
 * libc_host_errno_shim.c) does NOT include them -- and building this file
 * with that exact command does NOT fail to link. It LINKS FINE, because
 * host build (a) is a normal dynamically-linked host program (no
 * -nostdlib): an nl_langinfo symbol this file references but no local
 * translation unit defines is silently resolved against the HOST'S OWN
 * REAL glibc nl_langinfo() at link time -- exactly the same fallback the
 * file header of tests/unit/libc_iconv_test.c's build recipe already warns
 * about for errno. The difference is that errno failing this way is a
 * *link* error you notice; nl_langinfo succeeding this way is a *runtime*
 * segfault you might not connect to the missing source file: glibc's own
 * nl_langinfo() expects ITS internal packed category/index encoding, and
 * this file passes THIS library's own small sequential <langinfo.h> values
 * (CODESET=1, D_T_FMT=2, ...) -- which glibc's nl_langinfo reads as
 * whatever internal locale-table offset those bit patterns happen to
 * decode to, and one of those combinations reads out of bounds. Confirmed
 * on the host: building with the OLD gate_command plus this file's LANGINFO
 * section links cleanly and then segfaults on the very first linfo() call.
 * Verified the fix: adding langinfo.c + locale.c to the link line makes the
 * local definitions win (standard link-time precedence for a symbol defined
 * in a translation unit being linked, over the same symbol in a shared
 * library) and every LANGINFO line then diffs byte-identical against real
 * glibc. See the unit's verification report for the corrected gate_command.
 *
 * Every printed line reports return value, errno NAME (never a number --
 * resolved against whichever build's own <errno.h> is in scope, so the
 * comparison is of behaviour, not of two implementations agreeing on a bit
 * pattern), bytes left in the input, bytes consumed, and the output buffer
 * AS HEX. Hex, not text, on purpose: a terminal would mangle non-ASCII output
 * and a string compare would hide an endianness bug -- exactly the class of
 * bug this converter is most likely to have.
 *
 * DELIBERATELY EXCLUDED, WITH REASONS (not silently dropped -- see the unit's
 * final report for the same list):
 *   - //TRANSLIT: this library refuses it with EINVAL at iconv_open() time
 *     (see <iconv.h>); glibc implements a real substitution table and
 *     succeeds. Exercising it here would necessarily diff, not because
 *     either side is wrong, but because they document different, both
 *     legitimate, answers to "what should happen." Not tested.
 *   - fromcode == "" : glibc's iconv_open() treats an empty charset name as
 *     "the current locale's codeset" and succeeds; this library requires an
 *     explicit name and reports EINVAL. A real, intentional difference --
 *     not tested.
 *   - Any real charset glibc supports that isn't in this library's closed
 *     set (Shift-JIS, KOI8-R, UTF-7, ...): glibc accepts these and we
 *     legitimately don't (see <iconv.h> for why the set is closed). The
 *     "invalid charset name" section below therefore uses names that are
 *     fake on BOTH sides (glibc verified to reject them too), never a name
 *     that is merely unsupported by us.
 *   - iconv_open(NULL, ...), iconv_open(..., NULL), iconv_close(NULL) and
 *     iconv((iconv_t)NULL, ...): this library returns EINVAL/EBADF for all
 *     four (see <iconv.h>) rather than crashing. Verified on the host that
 *     real glibc does NOT NULL-check any of these -- it segfaults on all
 *     four. That makes them fundamentally undiffable, not merely untested:
 *     a NULL-argument case in this file would kill the "glibc" reference
 *     binary outright, which would desync every line printed after it from
 *     "ours" (a crash mid-stdout, not a clean divergent line), corrupting
 *     the whole comparison rather than just that one case. This library's
 *     graceful EINVAL/EBADF here is strictly safer than glibc's crash, not
 *     a bug -- just not something the two-build diff can observe.
 */
#include <iconv.h>
#include <langinfo.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Host-link plumbing only, same idea as tests/unit/libc_host_errno_shim.c:
 * building "ours" still links the whole of wchar.c, because iconv.c reuses
 * its mbrtowc/wcrtomb rather than re-deriving UTF-8 validation a third time
 * (see iconv.c's file header). wchar.c and the stdlib.c it pulls in have a
 * couple of symbols normally supplied by stdio.c / crt0 on the real target
 * (buffered-stream flush on exit, and the exec-time envp pointer); nothing
 * this test calls ever reaches them, but the host linker still wants them
 * defined. This is link-time plumbing for the diff harness, not part of the
 * library under test. */
void __libc_flush_all(void) {}
char **__libc_environ_hook;

static const char *errname(int e)
{
    if (e == EILSEQ) return "EILSEQ";
    if (e == EINVAL) return "EINVAL";
    if (e == E2BIG)  return "E2BIG";
    if (e == ENOMEM) return "ENOMEM";
    if (e == EBADF)  return "EBADF";
    if (e == ENOENT) return "ENOENT";
    return "OTHER";
}

static void hexout(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

/* One printed assertion line. `r` is iconv()'s raw return (size_t, so
 * (size_t)-1 on failure); `il`/`consumed` describe the input side after the
 * call; `out`/`outn` the bytes actually written this call. */
static void line(const char *tag, size_t r, size_t il, size_t consumed,
                  const unsigned char *out, size_t outn)
{
    if (r == (size_t)-1)
        printf("%-24s r=-1 errno=%-7s il=%zu consumed=%zu out=", tag, errname(errno), il, consumed);
    else
        printf("%-24s r=%zu errno=%-7s il=%zu consumed=%zu out=", tag, r, "-", il, consumed);
    hexout(out, outn);
    printf(" (%zu)\n", outn);
}

static void openfail(const char *tag)
{
    printf("%-24s open=FAIL errno=%s\n", tag, errname(errno));
}

/* nl_langinfo() prints its own tag + the returned string, quoted. nl_langinfo
 * never returns NULL (a bad item just returns ""), so unlike `line()` there
 * is no error path to print -- and every value it CAN return here is plain
 * ASCII (format strings, day/month names, AM/PM, the two locale separators,
 * yes/no regexes), so a quoted literal is exactly as unambiguous as hex would
 * be for this data, and far more readable. */
static void linfo(const char *tag, nl_item item)
{
    printf("%-24s \"%s\"\n", tag, nl_langinfo(item));
}

/* Single iconv_open + one iconv() call + iconv_close, the common case. */
static void run1(const char *tag, const char *to, const char *from,
                  const unsigned char *in, size_t inlen, size_t outcap)
{
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) { openfail(tag); return; }
    unsigned char outbuf[128];
    if (outcap > sizeof outbuf) outcap = sizeof outbuf;
    char *ip = (char *)in, *op = (char *)outbuf;
    size_t il = inlen, ol = outcap;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    line(tag, r, il, (size_t)(ip - (char *)in), outbuf, outcap - ol);
    iconv_close(cd);
}

/* UTF-8 -> cs -> UTF-8 round trip: proves both directions of a charset agree
 * with each other AND, via the outer diff, with glibc's own pair. */
static void roundtrip(const char *name, const char *cs,
                       const unsigned char *utf8, size_t utf8len)
{
    char tag[40];
    unsigned char mid[128], back[128];

    iconv_t cd = iconv_open(cs, "UTF-8");
    snprintf(tag, sizeof tag, "rt-%s-fwd", name);
    if (cd == (iconv_t)-1) { openfail(tag); return; }
    char *ip = (char *)utf8, *op = (char *)mid;
    size_t il = utf8len, ol = sizeof mid;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    size_t midlen = sizeof(mid) - ol;
    line(tag, r, il, (size_t)(ip - (char *)utf8), mid, midlen);
    iconv_close(cd);

    cd = iconv_open("UTF-8", cs);
    snprintf(tag, sizeof tag, "rt-%s-back", name);
    if (cd == (iconv_t)-1) { openfail(tag); return; }
    ip = (char *)mid; op = (char *)back;
    il = midlen; ol = sizeof back;
    r = iconv(cd, &ip, &il, &op, &ol);
    size_t backlen = sizeof(back) - ol;
    line(tag, r, il, (size_t)(ip - (char *)mid), back, backlen);
    iconv_close(cd);

    snprintf(tag, sizeof tag, "rt-%s-match", name);
    int match = (backlen == utf8len) && (memcmp(back, utf8, utf8len) == 0);
    printf("%-24s %s\n", tag, match ? "YES" : "NO");
}

int main(void)
{
    /* 'A' + e-acute (U+00E9) + CJK 'zhong' (U+4E2D) + grinning face (U+1F600,
     * astral, needs a UTF-16 surrogate pair) -- exercises 1/2/3/4-byte UTF-8
     * and every width UTF-16/UTF-32 have to represent a code point. Bytes
     * spelled out numerically, not as source-file literals, for the same
     * reason output is printed as hex: no encoding ambiguity anywhere. */
    static const unsigned char ASTRAL[] = { 0x41, 0xC3,0xA9, 0xE4,0xB8,0xAD, 0xF0,0x9F,0x98,0x80 };
    static const unsigned char BMP[]    = { 0x41, 0xC3,0xA9, 0xE4,0xB8,0xAD };            /* UCS-2 safe */
    static const unsigned char LAT[]    = { 0x41, 0xC3,0xA9 };                             /* Latin-1 safe */
    static const unsigned char ASC[]    = { 0x41 };                                        /* ASCII safe */

    /* ---- round trips, one per supported charset ------------------------ */
    roundtrip("utf16",   "UTF-16",   ASTRAL, sizeof ASTRAL);
    roundtrip("utf16le", "UTF-16LE", ASTRAL, sizeof ASTRAL);
    roundtrip("utf16be", "UTF-16BE", ASTRAL, sizeof ASTRAL);
    roundtrip("utf32",   "UTF-32",   ASTRAL, sizeof ASTRAL);
    roundtrip("utf32le", "UTF-32LE", ASTRAL, sizeof ASTRAL);
    roundtrip("utf32be", "UTF-32BE", ASTRAL, sizeof ASTRAL);
    roundtrip("ucs4",    "UCS-4",    ASTRAL, sizeof ASTRAL);
    roundtrip("wchart",  "WCHAR_T",  ASTRAL, sizeof ASTRAL);
    roundtrip("ucs2",    "UCS-2",    BMP,    sizeof BMP);
    roundtrip("latin1",  "LATIN1",   LAT,    sizeof LAT);
    roundtrip("ascii",   "ASCII",    ASC,    sizeof ASC);

    /* ---- generic UTF-16/UTF-32 BOM: decode side ------------------------- */
    { unsigned char in[] = {0xFF,0xFE,0x41,0x00};                 run1("u16-decode-le-bom",  "UTF-8", "UTF-16", in, sizeof in, 32); }
    { unsigned char in[] = {0xFE,0xFF,0x00,0x41};                 run1("u16-decode-be-bom",  "UTF-8", "UTF-16", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0x00};                           run1("u16-decode-no-bom",  "UTF-8", "UTF-16", in, sizeof in, 32); }
    { unsigned char in[] = {0xFF,0xFE,0,0,0x41,0,0,0};            run1("u32-decode-le-bom",  "UTF-8", "UTF-32", in, sizeof in, 32); }
    { unsigned char in[] = {0,0,0xFE,0xFF,0,0,0,0x41};            run1("u32-decode-be-bom",  "UTF-8", "UTF-32", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0,0,0};                          run1("u32-decode-no-bom",  "UTF-8", "UTF-32", in, sizeof in, 32); }

    /* ---- generic UTF-16/UTF-32 BOM: encode side (single char, matches the
     * exact byte pattern documented in <iconv.h>) --------------------------*/
    { unsigned char in[] = {0x41};                                run1("u16-encode-bom", "UTF-16", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};                                run1("u32-encode-bom", "UTF-32", "UTF-8", in, sizeof in, 32); }

    /* ---- LE/BE explicit forms: a leading FF FE / FE FF is an ORDINARY
     * character (U+FEFF), never consumed/emitted as a BOM ------------------*/
    { unsigned char in[] = {0xFF,0xFE,0x41,0x00};                 run1("u16le-not-a-bom", "UTF-8", "UTF-16LE", in, sizeof in, 32); }
    { unsigned char in[] = {0xFE,0xFF,0x00,0x41};                 run1("u16be-not-a-bom", "UTF-8", "UTF-16BE", in, sizeof in, 32); }

    /* ---- chunked BOM: endianness established by call 1 persists into call
     * 2 on the same descriptor ---------------------------------------------*/
    {
        iconv_t cd = iconv_open("UTF-8", "UTF-16");
        unsigned char bom[]  = {0xFE,0xFF};
        unsigned char rest[] = {0x00,0x42};
        unsigned char out[32];
        char *ip = (char*)bom, *op = (char*)out; size_t il = 2, ol = sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-chunk1-bom", r, il, (size_t)(ip-(char*)bom), out, sizeof(out)-ol);
        ip = (char*)rest; op = (char*)out; il = 2; ol = sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-chunk2-be", r, il, (size_t)(ip-(char*)rest), out, sizeof(out)-ol);
        iconv_close(cd);
    }
    { /* split BOM itself: 1 of 2 bytes present -> EINVAL, nothing consumed */
        iconv_t cd = iconv_open("UTF-8", "UTF-16");
        unsigned char in[] = {0xFF};
        unsigned char out[32];
        char *ip=(char*)in, *op=(char*)out; size_t il=1, ol=sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-split-bom", r, il, (size_t)(ip-(char*)in), out, sizeof(out)-ol);
        iconv_close(cd);
    }

    /* ---- reset (NULL inbuf): re-arms both the encode-side BOM latch and
     * the decode-side BOM/endianness latch on the same descriptor ---------- */
    {
        iconv_t cd = iconv_open("UTF-16", "UTF-8");
        unsigned char a[] = {0x41}, b[] = {0x42};
        unsigned char out[32];
        char *ip=(char*)a, *op=(char*)out; size_t il=1, ol=sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-reset-conv1", r, il, (size_t)(ip-(char*)a), out, sizeof(out)-ol);
        op=(char*)out; ol=sizeof out;
        r = iconv(cd, NULL, NULL, &op, &ol);
        line("u16-reset-call", r, 0, 0, out, sizeof(out)-ol);
        ip=(char*)b; op=(char*)out; il=1; ol=sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-reset-conv2", r, il, (size_t)(ip-(char*)b), out, sizeof(out)-ol);
        iconv_close(cd);
    }
    {
        iconv_t cd = iconv_open("UTF-32", "UTF-8");
        unsigned char a[] = {0x41}, b[] = {0x42};
        unsigned char out[32];
        char *ip=(char*)a, *op=(char*)out; size_t il=1, ol=sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("u32-reset-conv1", r, il, (size_t)(ip-(char*)a), out, sizeof(out)-ol);
        op=(char*)out; ol=sizeof out;
        r = iconv(cd, NULL, NULL, &op, &ol);
        line("u32-reset-call", r, 0, 0, out, sizeof(out)-ol);
        ip=(char*)b; op=(char*)out; il=1; ol=sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("u32-reset-conv2", r, il, (size_t)(ip-(char*)b), out, sizeof(out)-ol);
        iconv_close(cd);
    }
    { /* decode-side reset: a post-reset BOM-shaped prefix is re-detected as
       * a BOM, not decoded as a literal U+FEFF (proves from_started itself
       * resets, not just from_le). */
        iconv_t cd = iconv_open("UTF-8", "UTF-16");
        unsigned char first[]  = {0x41,0x00};    /* no BOM -> native default */
        unsigned char second[] = {0xFF,0xFE};    /* BOM-shaped */
        unsigned char out[32];
        char *ip=(char*)first, *op=(char*)out; size_t il=2, ol=sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-drs-conv1", r, il, (size_t)(ip-(char*)first), out, sizeof(out)-ol);
        op=(char*)out; ol=sizeof out;
        r = iconv(cd, NULL, NULL, &op, &ol);
        line("u16-drs-reset", r, 0, 0, out, sizeof(out)-ol);
        ip=(char*)second; op=(char*)out; il=2; ol=sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-drs-conv2", r, il, (size_t)(ip-(char*)second), out, sizeof(out)-ol);
        iconv_close(cd);
    }

    /* ---- truncated UTF-8 at every position of a 2/3/4-byte character ----- */
    { unsigned char in[] = {0x41,0xC3};                    run1("trunc-2of2-a", "UTF-8", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xE4};                    run1("trunc-1of3-a", "UTF-8", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xE4,0xB8};               run1("trunc-2of3-a", "UTF-8", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xF0};                    run1("trunc-1of4-a", "UTF-8", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xF0,0x9F};               run1("trunc-2of4-a", "UTF-8", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xF0,0x9F,0x98};          run1("trunc-3of4-a", "UTF-8", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xC3};                    run1("trunc-2of2-b", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xE4};                    run1("trunc-1of3-b", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xE4,0xB8};               run1("trunc-2of3-b", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xF0};                    run1("trunc-1of4-b", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xF0,0x9F};               run1("trunc-2of4-b", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xF0,0x9F,0x98};          run1("trunc-3of4-b", "UTF-32LE", "UTF-8", in, sizeof in, 32); }

    /* ---- truncated non-UTF-8 sources -------------------------------------*/
    { unsigned char in[] = {0x41};                         run1("trunc-u16le-1", "UTF-8", "UTF-16LE", in, sizeof in, 32); }
    { unsigned char in[] = {0x00,0xD8,0x00};               run1("trunc-u16le-surr", "UTF-8", "UTF-16LE", in, sizeof in, 32); }  /* high surrogate + 1 byte */
    { unsigned char in[] = {0x41};                         run1("trunc-ucs2-1", "UTF-8", "UCS-2", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0,0};                     run1("trunc-u32le-1", "UTF-8", "UTF-32LE", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0};                       run1("trunc-u32le-2", "UTF-8", "UTF-32LE", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};                         run1("trunc-u32le-3", "UTF-8", "UTF-32LE", in, sizeof in, 32); }
    { unsigned char in[] = {0,0,0};                        run1("trunc-ucs4-1", "UTF-8", "UCS-4", in, sizeof in, 32); }
    { unsigned char in[] = {0,0};                          run1("trunc-ucs4-2", "UTF-8", "UCS-4", in, sizeof in, 32); }
    { unsigned char in[] = {0};                             run1("trunc-ucs4-3", "UTF-8", "UCS-4", in, sizeof in, 32); }

    /* ---- overlong encodings, unpaired surrogates, out-of-range values --- */
    { unsigned char in[] = {0xC0,0x80};                    run1("overlong-2byte-c0", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0xC1,0xBF};                    run1("overlong-2byte-c1", "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0xE0,0x80,0x80};               run1("overlong-3byte",    "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0xF0,0x80,0x80,0x80};          run1("overlong-4byte",    "UTF-32LE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0xED,0xA0,0x80};               run1("utf8-surrogate",    "UTF-32LE", "UTF-8", in, sizeof in, 32); }  /* would-be U+D800 */
    { unsigned char in[] = {0x00,0xD8,0x41,0x00};          run1("u16-lone-high",     "UTF-8", "UTF-16LE", in, sizeof in, 32); }
    { unsigned char in[] = {0x00,0xDC,0x41,0x00};          run1("u16-lone-low",      "UTF-8", "UTF-16LE", in, sizeof in, 32); }
    { unsigned char in[] = {0x00,0xD8,0x00,0xD8};          run1("u16-high-high",     "UTF-8", "UTF-16LE", in, sizeof in, 32); }
    { unsigned char in[] = {0xD8,0x00,0x41,0x00};          run1("u16be-lone-high",   "UTF-8", "UTF-16BE", in, sizeof in, 32); }
    { unsigned char in[] = {0x00,0x00,0x11,0x00};          run1("u32-too-big",       "UTF-8", "UTF-32LE", in, sizeof in, 32); }
    /* NOT tested: UCS-4 with a value above U+10FFFF (e.g. the raw BE bytes
     * 00 11 00 00 = 0x00110000). Verified on the host: real glibc's UCS-4
     * module preserves ISO/IEC 10646's ORIGINAL 31-bit code space (it
     * accepts anything up to 0x7FFFFFFF, encoding to legacy 5/6-byte
     * pre-RFC-3629 "UTF-8" forms, and only rejects values with the top bit
     * set) rather than the Unicode-restricted 0..0x10FFFF range every other
     * charset here enforces. This library's internal representation between
     * decode and encode is a plain Unicode scalar value -- never a
     * surrogate, never above U+10FFFF, ENFORCED UNIFORMLY for every charset
     * (see iconv.c) -- and that invariant is what lets encode_one() ignore
     * which charset it's writing. Special-casing UCS-4 alone to carry a
     * wider range would mean threading a second representation through
     * every encoder for a code space nothing produced after 2003 (RFC 3629
     * restricted UTF-8 itself to <= U+10FFFF for UTF-16 compatibility, which
     * is also why this library's own UTF-8 encoder, reused from wchar.c,
     * cannot even emit the 5/6-byte forms glibc's answer relies on). EILSEQ
     * here for anything above U+10FFFF is a deliberate, principled
     * divergence from that one legacy corner of glibc, not a bug -- an
     * in-range UCS-4 round trip is already covered above (rt-ucs4-*). */
    { unsigned char in[] = {0x00,0x00,0x01,0x00};          run1("ucs2-astral",       "UCS-2", "UTF-32LE", in, sizeof in, 32); }

    /* ---- ASCII/Latin-1 range edges --------------------------------------- */
    { unsigned char in[] = {0x80};                         run1("ascii-highbit",  "UTF-32LE", "ASCII", in, sizeof in, 32); }
    { unsigned char in[] = {0x7F};                         run1("ascii-max",      "UTF-32LE", "ASCII", in, sizeof in, 32); }
    { unsigned char in[] = {0x00};                         run1("latin1-min",     "UTF-8", "ISO-8859-1", in, sizeof in, 32); }
    { unsigned char in[] = {0x7F};                         run1("latin1-mid",     "UTF-8", "ISO-8859-1", in, sizeof in, 32); }
    { unsigned char in[] = {0x80};                         run1("latin1-c1",      "UTF-8", "ISO-8859-1", in, sizeof in, 32); }
    { unsigned char in[] = {0xFF};                         run1("latin1-max",     "UTF-8", "ISO-8859-1", in, sizeof in, 32); }
    { unsigned char in[] = {0xC3,0xA9};                    run1("ascii-unmap-noignore", "ASCII", "UTF-8", in, sizeof in, 32); } /* e-acute, no //IGNORE */

    /* ---- E2BIG: stops before the character that doesn't fit, leaves the
     * input pointer there, and a retry with a fresh buffer resumes cleanly - */
    {
        iconv_t cd = iconv_open("ASCII", "UTF-8");
        unsigned char in[] = {0x41,0x42,0x43,0x44,0x45};
        unsigned char out[3];
        char *ip = (char*)in, *op = (char*)out; size_t il = 5, ol = 3;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-ascii-1", r, il, (size_t)(ip-(char*)in), out, sizeof(out)-ol);
        char *resume = ip;                        /* start of what call 2 consumes */
        op = (char*)out; ol = sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-ascii-2", r, il, (size_t)(ip-resume), out, sizeof(out)-ol);
        iconv_close(cd);
    }
    {
        iconv_t cd = iconv_open("UTF-16", "UTF-8");
        unsigned char in[] = {0x41};
        unsigned char out[3];                     /* room for the BOM, not for 'A' */
        char *ip = (char*)in, *op = (char*)out; size_t il = 1, ol = 3;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-u16-bom-1", r, il, (size_t)(ip-(char*)in), out, sizeof(out)-ol);
        char *resume = ip;
        op = (char*)out; ol = sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-u16-bom-2", r, il, (size_t)(ip-resume), out, sizeof(out)-ol);
        iconv_close(cd);
    }

    /* ---- //IGNORE: honest glibc semantics (see <iconv.h>) ---------------- */
    { unsigned char in[] = {0x41,0x42,0x43};                run1("ignore-clean",   "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xFF,0x42,0xFE,0x43};      run1("ignore-drops",   "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xC3,0xA9,0x42};           run1("ignore-unmap",   "ASCII//IGNORE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0xE4,0xB8};                run1("ignore-trunc",   "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); }
    {
        iconv_t cd = iconv_open("UTF-8//IGNORE", "UTF-8");
        unsigned char in[] = {0x41,0xFF,0x42,0x43,0x44};
        unsigned char out[2];
        char *ip=(char*)in, *op=(char*)out; size_t il=5, ol=2;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("ignore-e2big", r, il, (size_t)(ip-(char*)in), out, sizeof(out)-ol);
        iconv_close(cd);
    }
    { unsigned char in[] = {0xFF,0x41};                     run1("ignore-lead-drop", "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};                          run1("ignore-lower-suffix", "utf-8//ignore", "UTF-8", in, sizeof in, 32); }

    /* ---- charset name resolution: case-insensitive, '-' optional --
     * every variant below is one glibc ALSO accepts (verified on the host);
     * see the file header for the one variant class (leading '_' as a
     * separator) that is a deliberate superset and stays out of this diff. */
    { unsigned char in[] = {0x41};   run1("name-utf8-lower",   "utf8",       "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-utf8-mixed",   "Utf-8",      "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-utf16le-lower","utf16le",    "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-ascii-lower",  "ascii",      "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-usascii-hyph", "US-ASCII",   "UTF-8", in, sizeof in, 32); }
    /* NOT "USASCII" (hyphen removed, no separator at all): verified on the
     * host that real glibc does not recognise that spelling -- its matcher
     * folds case and treats '-' as insignificant only WITHIN its existing
     * alias strings, it does not reconstruct new aliases by deleting
     * hyphens from a known one (that reconstruction is exactly this
     * library's OWN, deliberately more permissive, behaviour -- see
     * <iconv.h> and the file header above). "USASCII" is therefore a name
     * only this library's normalizer happens to accept; testing it would
     * diff for a reason already fully explained, not demonstrate a bug. */
    { unsigned char in[] = {0x41};   run1("name-usascii-mixed","Us-Ascii",   "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-latin1",       "LATIN1",     "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-iso88591-hyph","ISO-8859-1", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-iso88591-nodh","ISO8859-1",  "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-wchart",       "WCHAR_T",    "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-ucs2-hyph",    "UCS-2",      "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-ucs2-nodh",    "UCS2",       "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-ucs4-hyph",    "UCS-4",      "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-ucs4-nodh",    "UCS4",       "UTF-8", in, sizeof in, 32); }

    /* ---- invalid charset names: fake on BOTH sides (verified) ------------ */
    { unsigned char in[] = {0x41};   run1("bad-name-1", "BOGUS-CHARSET-XYZ", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("bad-name-2", "NOT-A-CHARSET",     "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("bad-name-3", "12345",             "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("bad-name-from", "UTF-8", "BOGUS-CHARSET-XYZ", in, sizeof in, 32); }

    /* ---- zero-length / identity passthroughs ----------------------------- */
    {
        iconv_t cd = iconv_open("UTF-8", "UTF-8");
        char in0[1] = {0};
        unsigned char out[8];
        char *ip = in0, *op = (char*)out; size_t il = 0, ol = sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("zero-length", r, il, 0, out, sizeof(out)-ol);
        iconv_close(cd);
    }
    { unsigned char in[] = {0x41,0xC3,0xA9};                run1("identity-utf8",   "UTF-8",     "UTF-8",     in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0x00};                     run1("identity-u16le",  "UTF-16LE",  "UTF-16LE",  in, sizeof in, 32); }
    { unsigned char in[] = {0xC3,0xA9};                     run1("identity-latin1", "ISO-8859-1","ISO-8859-1",in, sizeof in, 32); }

    /* ======================================================================
     * ADVERSARIAL VERIFICATION ADDITIONS (u8-iconv-langinfo-v)
     *
     * Everything below was added during independent re-verification of this
     * unit, targeting spots the original 127 cases never exercised. One of
     * these (group SFX) found a real bug -- see resolve_to() in iconv.c and
     * its updated comment -- and was fixed there, not worked around here.
     * ==================================================================== */

    /* ---- SFX: unrecognised "//suffix" tokens -----------------------------
     * Verified on the host (throwaway probes, not committed): real glibc
     * SUCCEEDS opening "UTF-8//BOGUS" -- identically to plain "UTF-8" -- and
     * an unrecognised token elsewhere in a "//A//B" chain is silently
     * skipped, not an open-time error. The original version of resolve_to()
     * got this backwards (EINVAL on any token that wasn't exactly IGNORE or
     * TRANSLIT) on an unverified assumption; these cases are what caught it
     * and what now pins the fix. */
    { unsigned char in[] = {0x41,0xFF,0x42};   run1("sfx-bogus-noeffect",  "UTF-8//BOGUS",         "UTF-8", in, sizeof in, 32); } /* bad byte still EILSEQ: BOGUS did NOT turn on ignore */
    { unsigned char in[] = {0x41,0xFF,0x42};   run1("sfx-bogus-then-ign", "UTF-8//BOGUS//IGNORE", "UTF-8", in, sizeof in, 32); } /* IGNORE recognised after a leading unknown token */
    { unsigned char in[] = {0x41,0xFF,0x42};   run1("sfx-ign-then-bogus", "UTF-8//IGNORE//BOGUS", "UTF-8", in, sizeof in, 32); } /* IGNORE recognised before a trailing unknown token */
    { unsigned char in[] = {0x41};             run1("sfx-trailing-empty", "UTF-8//",              "UTF-8", in, sizeof in, 32); } /* bare trailing "//": open succeeds, behaves like plain UTF-8 */
    { unsigned char in[] = {0x41};             run1("sfx-digits",         "ASCII//12345",         "UTF-8", in, sizeof in, 32); } /* unrecognised token that isn't even alphabetic */
    { unsigned char in[] = {0x41};             run1("sfx-mixedcase-bogus","UTF-8//Bogus",         "UTF-8", in, sizeof in, 32); } /* case-folded before compare, still doesn't match IGNORE/TRANSLIT */
    { unsigned char in[] = {0x41,0xFF,0x42};   run1("sfx-ign-empty-ign",  "UTF-8//IGNORE//",       "UTF-8", in, sizeof in, 32); } /* trailing empty token after a real one */

    /* ---- BOM: split at every byte offset, both generic forms ------------- */
    { unsigned char in[] = {0xFE};                          run1("bom-u16-split-be1", "UTF-8", "UTF-16", in, sizeof in, 32); } /* lone 0xFE (BE-looking start), not just 0xFF as the original suite covered */
    { unsigned char in[] = {0xFF};                          run1("bom-u32-split-1",   "UTF-8", "UTF-32", in, sizeof in, 32); }
    { unsigned char in[] = {0xFF,0xFE};                     run1("bom-u32-split-2",   "UTF-8", "UTF-32", in, sizeof in, 32); }
    { unsigned char in[] = {0xFF,0xFE,0x00};                run1("bom-u32-split-3",   "UTF-8", "UTF-32", in, sizeof in, 32); }
    { unsigned char in[] = {0xFE,0xFF,0x00};                run1("bom-u16-be-trunc",  "UTF-8", "UTF-16", in, sizeof in, 32); } /* BOM consumed, then only 1 of 2 bytes of the first real unit */
    { unsigned char in[] = {0xFF,0xFE,0x00,0xDC};           run1("bom-u16-lone-low",  "UTF-8", "UTF-16", in, sizeof in, 32); } /* BOM consumed, then a lone low surrogate right after it */

    /* ---- zero-length input into a BOM-emitting target: no BOM should
     * appear when there is nothing to convert ------------------------------ */
    { unsigned char in[1] = {0};              run1("bom-u16-zerolen", "UTF-16", "UTF-8", in, 0, 32); }
    { unsigned char in[1] = {0};              run1("bom-u32-zerolen", "UTF-32", "UTF-8", in, 0, 32); }

    /* ---- exact encode/decode boundary values ------------------------------ */
    { unsigned char in[] = {0xFF,0xFF,0,0};                 run1("bound-u16-max-bmp",    "UTF-16LE", "UTF-32LE", in, sizeof in, 32); } /* U+FFFF: last non-surrogate BMP value, no pairing */
    { unsigned char in[] = {0,0,1,0};                       run1("bound-u16-min-astral", "UTF-16LE", "UTF-32LE", in, sizeof in, 32); } /* U+10000: first value needing a surrogate pair */
    { unsigned char in[] = {0xFF,0xFF,0x10,0};              run1("bound-u8-max-scalar",  "UTF-8",    "UTF-32LE", in, sizeof in, 32); } /* U+10FFFF: highest legal scalar value, via UTF-8 */
    { unsigned char in[] = {0xFF,0xFF,0x10,0};              run1("bound-ucs4-max",       "UCS-4",    "UTF-32LE", in, sizeof in, 32); } /* same value, encoded to fixed-BE UCS-4 */
    { unsigned char in[] = {0,0,0x11,0};                    run1("bound-u32-over-max",   "UTF-32LE", "UTF-32LE", in, sizeof in, 32); } /* U+110000: one past the ceiling -- must be DEC_INVALID, not silently masked */
    { unsigned char in[] = {0xFF,0xFF,0,0};                 run1("bound-ucs2-max",       "UCS-2",    "UTF-32LE", in, sizeof in, 32); } /* U+FFFF: last value UCS-2 can still represent */
    { unsigned char in[] = {0x7F,0,0,0};                    run1("bound-ascii-max-enc",  "ASCII",    "UTF-32LE", in, sizeof in, 32); } /* encode side (not decode): 0x7F is the last legal ASCII value */
    { unsigned char in[] = {0x80,0,0,0};                    run1("bound-ascii-over-enc", "ASCII",    "UTF-32LE", in, sizeof in, 32); } /* 0x80 has no ASCII representation -- EILSEQ on the ENCODE side */
    { unsigned char in[] = {0xFF,0,0,0};                    run1("bound-latin1-max-enc", "ISO-8859-1","UTF-32LE",in, sizeof in, 32); } /* 0xFF is the last legal Latin-1 value */
    { unsigned char in[] = {0,1,0,0};                       run1("bound-latin1-over-enc","ISO-8859-1","UTF-32LE",in, sizeof in, 32); } /* U+0100 has no Latin-1 representation -- EILSEQ on the ENCODE side */
    { unsigned char in[] = {0xFF,0xD7};                     run1("bound-surrogate-below","UTF-32LE", "UTF-16LE", in, sizeof in, 32); } /* U+D7FF: last scalar value below the surrogate range */
    { unsigned char in[] = {0x00,0xE0};                     run1("bound-surrogate-above","UTF-32LE", "UTF-16LE", in, sizeof in, 32); } /* U+E000: first scalar value above the surrogate range */

    /* ---- E2BIG with an output buffer too small even for one unit --------- */
    {
        iconv_t cd = iconv_open("UTF-8", "UTF-8");
        unsigned char in[] = {0x41,0x42};
        unsigned char out[4];
        char *ip=(char*)in, *op=(char*)out; size_t il=2, ol=0;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-zero-outcap", r, il, (size_t)(ip-(char*)in), out, 0);
        iconv_close(cd);
    }
    {
        iconv_t cd = iconv_open("UTF-16LE", "UTF-8");   /* explicit LE: no BOM to eat the budget */
        unsigned char in[] = {0x41};
        unsigned char out[4];
        char *ip=(char*)in, *op=(char*)out; size_t il=1, ol=1;   /* needs 2, has 1 */
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-u16le-1byte", r, il, (size_t)(ip-(char*)in), out, 1-ol);
        iconv_close(cd);
    }
    {
        iconv_t cd = iconv_open("UTF-32LE", "UTF-8");   /* explicit LE: no BOM */
        unsigned char in[] = {0x41};
        unsigned char out[4];
        char *ip=(char*)in, *op=(char*)out; size_t il=1, ol=3;   /* needs 4, has 3 */
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-u32le-3byte", r, il, (size_t)(ip-(char*)in), out, 3-ol);
        iconv_close(cd);
    }
    {
        iconv_t cd = iconv_open("UTF-16", "UTF-8");   /* generic: BOM needs 2, budget is 1 -- fails before the BOM is even written */
        unsigned char in[] = {0x41};
        unsigned char out[4];
        char *ip=(char*)in, *op=(char*)out; size_t il=1, ol=1;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-u16-bom-too-small", r, il, (size_t)(ip-(char*)in), out, 1-ol);
        iconv_close(cd);
    }
    {
        iconv_t cd = iconv_open("UTF-32", "UTF-8");   /* generic: BOM needs 4, budget is 3 */
        unsigned char in[] = {0x41};
        unsigned char out[4];
        char *ip=(char*)in, *op=(char*)out; size_t il=1, ol=3;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("e2big-u32-bom-too-small", r, il, (size_t)(ip-(char*)in), out, 3-ol);
        iconv_close(cd);
    }

    /* ---- chunked resumption: an astral UTF-8 character fed one byte at a
     * time (worst-case granularity), decoding into UTF-32LE ---------------- */
    {
        iconv_t cd = iconv_open("UTF-32LE", "UTF-8");
        static const unsigned char whole[] = {0xF0,0x9F,0x98,0x80};  /* grinning face, U+1F600 */
        unsigned char out[8];
        for (size_t got = 1; got <= sizeof whole; got++) {
            char *ip = (char *)whole, *op = (char *)out;
            size_t il = got, ol = sizeof out;
            size_t r = iconv(cd, &ip, &il, &op, &ol);
            char tag[24];
            snprintf(tag, sizeof tag, "chunk-astral-%zuof4", got);
            line(tag, r, il, (size_t)(ip - (char *)whole), out, sizeof(out) - ol);
        }
        iconv_close(cd);
    }

    /* ---- two consecutive REAL (non-reset) calls on a fresh generic-UTF-16
     * encode descriptor: the BOM must appear exactly once, on the first
     * call, with nothing special done to request it (no prior error/retry,
     * unlike the existing e2big-u16-bom-* pair) --------------------------- */
    {
        iconv_t cd = iconv_open("UTF-16", "UTF-8");
        unsigned char a[] = {0x41}, b[] = {0x42};
        unsigned char out[32];
        char *ip=(char*)a, *op=(char*)out; size_t il=1, ol=sizeof out;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-two-calls-1st", r, il, (size_t)(ip-(char*)a), out, sizeof(out)-ol);
        ip=(char*)b; op=(char*)out; il=1; ol=sizeof out;
        r = iconv(cd, &ip, &il, &op, &ol);
        line("u16-two-calls-2nd", r, il, (size_t)(ip-(char*)b), out, sizeof(out)-ol);
        iconv_close(cd);
    }

    /* ---- charset-name resolution: a couple of variants the original 20-odd
     * name-* cases didn't try ------------------------------------------------ */
    { unsigned char in[] = {0x41};   run1("name-iso88591-lower",    "iso8859-1",  "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-iso88591-lowerhyph","iso-8859-1", "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-ucs2-lowerhyph",    "ucs-2",      "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41};   run1("name-wchart-lower",      "wchar_t",    "UTF-8", in, sizeof in, 32); }

    /* ---- //IGNORE combined with an unrecognised trailing charset-name
     * suffix, exercising both fixed points (resolve_to's skip-unknown AND
     * the ignore-drops decode path) in the same descriptor ------------------ */
    { unsigned char in[] = {0x41,0xFF,0x42,0xFE,0x43};   run1("sfx-ignore-plus-bogus-drops", "UTF-8//IGNORE//WHATEVER", "UTF-8", in, sizeof in, 32); }

    /* ---- //IGNORE resync granularity for a VALID lead byte followed by a
     * BAD continuation byte (as opposed to the original suite's only
     * invalid-byte cases, 0xFF/0xFE, which are invalid AS LEAD BYTES and so
     * only ever prove a 1-byte skip in the trivial case). unit_size()
     * returns 1 for CS_UTF8 unconditionally -- this checks that resyncing by
     * exactly 1 byte after a multi-byte lead is ALSO what glibc does (it
     * would be easy to assume, wrongly, that glibc resyncs past the whole
     * malformed sequence instead). Verified on the host first (throwaway
     * probe): dropping just the lead byte and re-decoding the stray
     * continuation-shaped byte as its own (here, ASCII-valid) unit is
     * exactly what real glibc's //IGNORE does. */
    { unsigned char in[] = {0x41,0xE4,0x58,0x42};        run1("ignore-lead3-badcont", "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); } /* 3-byte lead + non-continuation byte */
    { unsigned char in[] = {0x41,0xF0,0x9F,0x58,0x42};   run1("ignore-lead4-badcont", "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); } /* 4-byte lead, 1 good continuation, then bad */
    { unsigned char in[] = {0x41,0xC3,0x58,0x42};        run1("ignore-lead2-badcont", "UTF-8//IGNORE", "UTF-8", in, sizeof in, 32); } /* 2-byte lead + non-continuation byte */

    /* ---- embedded NUL is DATA, not a string terminator -- iconv.c's own
     * comment on decode_one's CS_UTF8 case claims mbrtowc's k==0 return is
     * special-cased so a NUL mid-buffer consumes exactly 1 byte and
     * conversion keeps going; nothing in the original 127 cases actually
     * fed a NUL through iconv() to prove it (the "zero-length" case's
     * placeholder byte is never read, since its inbytesleft is 0). ------- */
    { unsigned char in[] = {0x41,0x00,0x42};             run1("embedded-nul-utf8",   "UTF-8",    "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x00};                       run1("solo-nul-utf8",       "UTF-8",    "UTF-8", in, sizeof in, 32); }
    { unsigned char in[] = {0x41,0x00,0x42};             run1("embedded-nul-u32le",  "UTF-32LE", "UTF-8", in, sizeof in, 32); } /* NUL round-tripped through the generic decode/encode path, not just CS_UTF8's own shortcut */

    /* ======================================================================
     * LANGINFO: nl_langinfo(), for real this time (see the file header for
     * why this belongs in this file and why the claim that it already
     * happened was wrong). Every item this library implements (per
     * <langinfo.h>) gets one line here; there is no error return to check --
     * nl_langinfo() itself is documented to return "" for an unsupported
     * item and never NULL, so every call below either matches glibc's real
     * string or doesn't, with nothing else to go wrong at this layer.
     * ==================================================================== */
    linfo("li-codeset", CODESET);
    linfo("li-d-t-fmt", D_T_FMT);
    linfo("li-d-fmt",   D_FMT);
    linfo("li-t-fmt",   T_FMT);
    linfo("li-am-str",  AM_STR);
    linfo("li-pm-str",  PM_STR);
    linfo("li-radixchar", RADIXCHAR);
    linfo("li-thousep",   THOUSEP);
    linfo("li-yesexpr",   YESEXPR);
    linfo("li-noexpr",    NOEXPR);

    linfo("li-day-1", DAY_1); linfo("li-day-2", DAY_2); linfo("li-day-3", DAY_3);
    linfo("li-day-4", DAY_4); linfo("li-day-5", DAY_5); linfo("li-day-6", DAY_6);
    linfo("li-day-7", DAY_7);
    linfo("li-abday-1", ABDAY_1); linfo("li-abday-2", ABDAY_2); linfo("li-abday-3", ABDAY_3);
    linfo("li-abday-4", ABDAY_4); linfo("li-abday-5", ABDAY_5); linfo("li-abday-6", ABDAY_6);
    linfo("li-abday-7", ABDAY_7);

    linfo("li-mon-1", MON_1);   linfo("li-mon-2", MON_2);   linfo("li-mon-3", MON_3);
    linfo("li-mon-4", MON_4);   linfo("li-mon-5", MON_5);   linfo("li-mon-6", MON_6);
    linfo("li-mon-7", MON_7);   linfo("li-mon-8", MON_8);   linfo("li-mon-9", MON_9);
    linfo("li-mon-10", MON_10); linfo("li-mon-11", MON_11); linfo("li-mon-12", MON_12);
    linfo("li-abmon-1", ABMON_1);   linfo("li-abmon-2", ABMON_2);   linfo("li-abmon-3", ABMON_3);
    linfo("li-abmon-4", ABMON_4);   linfo("li-abmon-5", ABMON_5);   linfo("li-abmon-6", ABMON_6);
    linfo("li-abmon-7", ABMON_7);   linfo("li-abmon-8", ABMON_8);   linfo("li-abmon-9", ABMON_9);
    linfo("li-abmon-10", ABMON_10); linfo("li-abmon-11", ABMON_11); linfo("li-abmon-12", ABMON_12);

    /* ---- unsupported item: nl_langinfo's own documented "" answer, not a
     * crash or an out-of-bounds read. 0 is not a valid nl_item on either
     * side (both headers number their real items starting at 1), so this
     * exercises exactly the fallback path <langinfo.h> promises. ---------- */
    linfo("li-unsupported-zero", (nl_item)0);

    return 0;
}
