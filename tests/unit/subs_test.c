/* tests/unit/subs_test.c -- host driver for c/lib/media/subs.c.
 *
 *   subs_test cues <file.vtt>      parse as WebVTT, print canonical cue dump
 *   subs_test srt  <file.srt>      parse as SRT, print the same canonical dump
 *   subs_test auto <file>          subs_parse() (sniff), print the same dump
 *   subs_test units                this file's own hand-written assertions
 *
 * The canonical dump (one "CUE ...", then "REGION ..." lines, then a final
 * "SKIPPED n") is line-for-line what tests/unit/subs_oracle.py prints for
 * the same file -- tests/unit/subs_diff.py runs both and compares field by
 * field with a numeric tolerance on doubles (a WebVTT `line` value can be
 * Number.MAX_VALUE; string-diffing two languages' float formatting would be
 * comparing formatting, not parsing). On a bad signature, prints
 * "FORMAT-ERROR" and exits 1, matching the oracle.
 *
 * `units` is the second, independent line of defense: a HANDFUL of cases
 * from the same corpus, hand-traced against the spec text once (rather than
 * against the oracle, which after all shares no code with subs.c but DOES
 * share the one human who read the spec) and asserted directly, so a bug
 * that happened to fool both the C implementation AND the Python oracle in
 * the same way -- unlikely, given they share no code, but not impossible if
 * a spec passage was simply misread -- still has a chance of being caught.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "subs.h"

static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    long got = n > 0 ? (long)fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != n) { free(buf); return NULL; }
    *out_len = n;
    return buf;
}

static void print_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\') fputs("\\\\", stdout);
        else if (*s == '\n') fputs("\\n", stdout);
        else fputc(*s, stdout);
    }
}

static const char *align_name(subs_align a)
{
    switch (a) {
    case SUBS_ALIGN_START: return "start";
    case SUBS_ALIGN_CENTER: return "center";
    case SUBS_ALIGN_END: return "end";
    case SUBS_ALIGN_LEFT: return "left";
    case SUBS_ALIGN_RIGHT: return "right";
    }
    return "?";
}
static const char *lalign_name(subs_line_align a)
{
    switch (a) {
    case SUBS_LALIGN_START: return "start";
    case SUBS_LALIGN_CENTER: return "center";
    case SUBS_LALIGN_END: return "end";
    }
    return "?";
}
static const char *palign_name(subs_pos_align a)
{
    switch (a) {
    case SUBS_PALIGN_LINE_LEFT: return "line-left";
    case SUBS_PALIGN_CENTER: return "center";
    case SUBS_PALIGN_LINE_RIGHT: return "line-right";
    case SUBS_PALIGN_AUTO: return "auto";
    }
    return "?";
}
static const char *vert_name(subs_vertical v)
{
    switch (v) {
    case SUBS_VERTICAL_NONE: return "";
    case SUBS_VERTICAL_RL: return "rl";
    case SUBS_VERTICAL_LR: return "lr";
    }
    return "?";
}

static void dump_track(const subs_track *tr)
{
    int n = subs_cue_count(tr);
    for (int i = 0; i < n; i++) {
        const subs_cue *c = subs_cue_at(tr, i);
        const subs_settings *st = &c->settings;
        /* id and text are the only free-form (escaped) fields and are kept
         * LAST on purpose -- subs_diff.py locates "id=" by fixed position
         * (everything before it is space-separated enums/numbers with no
         * spaces of their own) and "text=" via the rightmost match, which
         * is correct regardless of what id/text themselves contain. */
        printf("CUE %lld %lld vert=%s snap=%d line=",
               (long long)c->start_ms, (long long)c->end_ms,
               vert_name(st->vertical), st->snap_to_lines ? 1 : 0);
        if (st->line_is_auto) printf("auto"); else printf("%.17g", st->line);
        printf(" lalign=%s pos=", lalign_name(st->line_align));
        if (st->position_is_auto) printf("auto"); else printf("%.17g", st->position);
        printf(" palign=%s size=%.17g align=%s region=%d id=",
               palign_name(st->position_align), st->size, align_name(st->align), st->region);
        print_escaped(c->id);
        printf(" text=");
        print_escaped(c->text);
        printf("\n");
    }
    int nr = subs_region_count(tr);
    for (int i = 0; i < nr; i++) {
        const subs_region *r = subs_region_at(tr, i);
        printf("REGION %d id=%s width=%.17g lines=%d ax=%.17g ay=%.17g vx=%.17g vy=%.17g scroll=%s\n",
               i, r->id, r->width, r->lines, r->anchor_x, r->anchor_y,
               r->viewport_x, r->viewport_y, r->scroll == SUBS_SCROLL_UP ? "up" : "");
    }
    printf("SKIPPED %d\n", subs_skipped_count(tr));
}

/* ------------------------------------------------------------- units ---- */
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static subs_track *vtt(const char *s) { return subs_parse_vtt((const uint8_t *)s, (long)strlen(s), NULL); }
static subs_track *srt(const char *s) { return subs_parse_srt((const uint8_t *)s, (long)strlen(s), NULL); }

static void test_signature(void)
{
    int err;
    subs_track *t0 = subs_parse_vtt((const uint8_t *)"WEBVTT", 6, &err);
    CHECK(t0 != NULL, "exact 6-byte signature is valid");
    subs_close(t0);
    subs_track *t1 = subs_parse_vtt((const uint8_t *)"WEBVTT\n", 7, &err);
    CHECK(t1 != NULL, "WEBVTT+LF valid");
    subs_close(t1);
    CHECK(subs_parse_vtt((const uint8_t *)"VTTWEB\n", 7, &err) == NULL && err == SUBS_ERR_FORMAT, "wrong 6 bytes rejected");
    CHECK(subs_parse_vtt((const uint8_t *)"webvtt\n", 7, &err) == NULL, "lowercase rejected (case-sensitive)");
    CHECK(subs_parse_vtt((const uint8_t *)"WEBVTTfoo\n", 10, &err) == NULL, "7th byte must be space/tab/LF");
    CHECK(subs_parse_vtt((const uint8_t *)"WEBVTT\x0c\n", 8, &err) == NULL, "form feed is NOT a valid 7th byte");
    CHECK(subs_parse_vtt((const uint8_t *)"", 0, &err) == NULL, "empty file rejected");
    {
        const uint8_t bom_only[] = { 0xEF, 0xBB, 0xBF, 'W','E','B','V','T','T','\n' };
        subs_track *t = subs_parse_vtt(bom_only, sizeof bom_only, &err);
        CHECK(t != NULL && subs_cue_count(t) == 0, "single BOM + WEBVTT is valid, 0 cues");
        subs_close(t);
    }
    {
        /* two BOMs: only the first is a byte-order mark; the second survives
         * decoding as a literal U+FEFF, which is not 'W' -- signature fails. */
        const uint8_t two_boms[] = { 0xEF,0xBB,0xBF, 0xEF,0xBB,0xBF, 'W','E','B','V','T','T','\n' };
        CHECK(subs_parse_vtt(two_boms, sizeof two_boms, &err) == NULL, "second BOM is not stripped, signature fails");
    }
    {
        /* NUL -> U+FFFD happens BEFORE the signature check, so a NUL at
         * byte 6 does not satisfy the space/tab/LF requirement. */
        const uint8_t nul_sig[] = { 'W','E','B','V','T','T', 0x00, '\n' };
        CHECK(subs_parse_vtt(nul_sig, sizeof nul_sig, &err) == NULL, "NUL-replaced 7th byte still rejected");
    }
}

static void test_header_and_signature_timings(void)
{
    /* header-garbage / header-space / header-tab: extra header content,
     * with or without an actual blank line before the first cue, must not
     * stop the first cue from parsing. */
    subs_track *t = vtt("WEBVTT\nfoobar\n\n00:00:00.000 --> 00:00:01.000\ntext\n");
    CHECK(t && subs_cue_count(t) == 1, "header-garbage: 1 cue");
    if (t) { CHECK(strcmp(subs_cue_at(t, 0)->text, "text") == 0, "header-garbage: text"); subs_close(t); }

    /* header-space: "WEBVTT\n \n00:00..." -- NO blank line at all between
     * the header and the timing line; the single-space line is in-header
     * content, and the arrow line right after still starts a cue. */
    t = vtt("WEBVTT\n \n00:00:00.000 --> 00:00:01.000\ntext\n");
    CHECK(t && subs_cue_count(t) == 1, "header-space, no blank separator: 1 cue");
    subs_close(t);

    /* signature-timings: "WEBVTT 00:00:00.000 --> 00:00:01.000\ntext\n" --
     * the arrow is on the SIGNATURE line itself, discarded as header text
     * (in_header disables cue creation), so 0 cues. */
    t = vtt("WEBVTT 00:00:00.000 --> 00:00:01.000\ntext\n");
    CHECK(t && subs_cue_count(t) == 0, "signature-timings: an arrow on the header line is not a cue");
    subs_close(t);
}

static void test_ids(void)
{
    subs_track *t = vtt(
        "WEBVTT\n\n"
        " leading space\n00:00:00.000 --> 00:00:01.000\ntext0\n\n"
        "trailing space \n00:00:00.000 --> 00:00:01.000\ntext1\n\n"
        "-- >\n00:00:00.000 --> 00:00:01.000\ntext2\n\n"
        "->\n00:00:00.000 --> 00:00:01.000\ntext3\n\n"
        " \n00:00:00.000 --> 00:00:01.000\ntext4\n");
    CHECK(t && subs_cue_count(t) == 5, "ids.test: 5 cues");
    if (t) {
        CHECK(strcmp(subs_cue_at(t, 0)->id, " leading space") == 0, "id 0");
        CHECK(strcmp(subs_cue_at(t, 1)->id, "trailing space ") == 0, "id 1");
        CHECK(strcmp(subs_cue_at(t, 2)->id, "-- >") == 0, "id 2 (near-miss arrow is not \"-->\")" );
        CHECK(strcmp(subs_cue_at(t, 3)->id, "->") == 0, "id 3");
        CHECK(strcmp(subs_cue_at(t, 4)->id, " ") == 0, "id 4");
        subs_close(t);
    }
}

static void test_arrows_self_resync(void)
{
    /* arrows.test's core claim: a line containing "-->" that fails to parse
     * as a timing line is dropped, and the NEXT real timing line still
     * starts a cue -- it does not corrupt the rest of the block. */
    subs_track *t = vtt(
        "WEBVTT\n\n"
        "-->\n00:00:00.000 --> 00:00:01.000\ntext0\n"
        "foo-->\n00:00:00.000 --> 00:00:01.000\ntext1\n"
        "-->foo\n00:00:00.000 --> 00:00:01.000\ntext2\n"
        "--->\n00:00:00.000 --> 00:00:01.000\ntext3\n"
        "-->-->\n00:00:00.000 --> 00:00:01.000\ntext4\n"
        "00:00:00.000 --> 00:00:01.000\ntext5\n\n"
        "00:00:00.000 -a -->\n\n"
        "00:00:00.000 --a -->\n\n"
        "00:00:00.000 - -->\n\n"
        "00:00:00.000 -- -->");
    CHECK(t && subs_cue_count(t) == 6, "arrows.test: exactly 6 cues (%d)", t ? subs_cue_count(t) : -1);
    if (t) {
        for (int i = 0; i < subs_cue_count(t); i++) {
            char want[16]; snprintf(want, sizeof want, "text%d", i);
            CHECK(strcmp(subs_cue_at(t, i)->text, want) == 0, "arrows cue %d text", i);
            CHECK(subs_cue_at(t, i)->id[0] == '\0', "arrows cue %d id empty", i);
        }
        /* every failed attempt is a counted malformed cue: 5 discarded
         * "-->..."/"...-->..." lines at the top, plus 4 near-miss hyphen
         * attempts at the bottom = 9. */
        CHECK(subs_skipped_count(t) == 9, "arrows.test: 9 malformed attempts (%d)", subs_skipped_count(t));
        subs_close(t);
    }
}

static void test_no_blank_between_cues(void)
{
    /* The bug this file's header warns about: a cue immediately followed by
     * the NEXT cue's timing line, with no blank line between them, must
     * still get its own text (not lose it to the rewind path). */
    subs_track *t = vtt("WEBVTT\n\n00:00:00.000 --> 00:00:01.000\ntext4\n"
                         "00:00:00.000 --> 00:00:01.000\ntext5\n\n");
    CHECK(t && subs_cue_count(t) == 2, "back-to-back cues: 2 cues");
    if (t) {
        CHECK(strcmp(subs_cue_at(t, 0)->text, "text4") == 0, "first cue keeps its text across the rewind");
        CHECK(strcmp(subs_cue_at(t, 1)->text, "text5") == 0, "second cue text");
        subs_close(t);
    }
}

static void test_newlines(void)
{
    subs_track *t = vtt("WEBVTT\r\r"
                         "cr\r00:00:00.000 --> 00:00:01.000\rtext0\n"
                         "\nlf\n00:00:00.000 --> 00:00:01.000\ntext1\r\n"
                         "\r\ncrlf\r\n00:00:00.000 --> 00:00:01.000\r\ntext2\n"
                         "\rlfcr\r00:00:00.000 --> 00:00:01.000\ntext3\n\r\n");
    CHECK(t && subs_cue_count(t) == 4, "newlines.test: 4 cues across CR/LF/CRLF/LFCR");
    if (t) {
        const char *want_id[4] = { "cr", "lf", "crlf", "lfcr" };
        for (int i = 0; i < 4; i++) {
            CHECK(strcmp(subs_cue_at(t, i)->id, want_id[i]) == 0, "newlines id %d", i);
            char want_text[8]; snprintf(want_text, sizeof want_text, "text%d", i);
            CHECK(strcmp(subs_cue_at(t, i)->text, want_text) == 0, "newlines text %d", i);
        }
        subs_close(t);
    }
}

static void test_comment_in_cue_text(void)
{
    subs_track *t = vtt("WEBVTT\n\n"
                         "NOTE this is real comment that should be ignored\n\n"
                         "00:00:00.000 --> 00:00:01.000\nNOTE text\n\n"
                         "NOTE\nthis is also a real comment that should be ignored\n"
                         "this is also a real comment that should be ignored\n\n"
                         "00:00:01.000 --> 00:00:02.000\nNOTE text\nNOTE text2\n");
    CHECK(t && subs_cue_count(t) == 2, "comment-in-cue-text: 2 cues");
    if (t) {
        CHECK(strcmp(subs_cue_at(t, 0)->text, "NOTE text") == 0, "cue0 text is literally 'NOTE text'");
        CHECK(strcmp(subs_cue_at(t, 1)->text, "NOTE text\nNOTE text2") == 0, "cue1 multi-line text");
        subs_close(t);
    }
}

static void test_timings_negative_60_omitted_hours(void)
{
    subs_track *t = vtt("WEBVTT\n\n"
                         "00:00:00.000 --> 00:00:00.000\ntext0\n\n"
                         "00:00:01.000 --> 00:00:00.999\ntext1\n\n"
                         "00:01:00.000 --> 00:00:59.999\ntext2\n\n"
                         "01:00:00.000 --> 00:59:59.999\ntext3\n");
    CHECK(t && subs_cue_count(t) == 4, "timings-negative: 4 cues");
    if (t) {
        int64_t want_start[4] = { 0, 1000, 60000, 3600000 };
        int64_t want_end[4]   = { 0, 999, 59999, 3599999 };
        for (int i = 0; i < 4; i++) {
            CHECK(subs_cue_at(t, i)->start_ms == want_start[i], "neg start %d", i);
            CHECK(subs_cue_at(t, i)->end_ms == want_end[i], "neg end %d", i);
        }
        subs_close(t);
    }

    t = vtt("WEBVTT\n\n"
            "00:00:60.000 --> 00:00:01.000\ninvalid\n\n"
            "00:60:00.000 --> 00:00:01.000\ninvalid\n\n"
            "00:00:00.000 --> 00:00:60.000\ninvalid\n\n"
            "00:00:00.000 --> 00:60:00.000\ninvalid\n\n"
            "00:00:00.000 --> 60:00:01.000\ntext1\n\n"
            "60:00:00.000 --> 60:00:01.000\ntext2\n");
    CHECK(t && subs_cue_count(t) == 2, "timings-60: 2 valid cues (out-of-range 60 rejected 4x)");
    if (t) {
        CHECK(subs_cue_at(t, 0)->end_ms == 216001000LL, "timings-60: hour-forced end = 216001s");
        CHECK(subs_cue_at(t, 1)->start_ms == 216000000LL, "timings-60: hour-forced start = 216000s");
        subs_close(t);
    }

    t = vtt("WEBVTT\n\n"
            "00:00.000 --> 00:00:01.000\ntext0\n\n"
            "00:00:00.000 --> 00:01.000\ntext1\n\n"
            "00:00.000 --> 00:01.000\ntext2\n");
    CHECK(t && subs_cue_count(t) == 3, "timings-omitted-hours: all 3 valid");
    if (t) {
        for (int i = 0; i < 3; i++) CHECK(subs_cue_at(t, i)->start_ms == 0, "omitted-hours start %d", i);
        subs_close(t);
    }
}

static void test_whitespace_chars(void)
{
    subs_track *t = vtt("WEBVTT\n\n"
                         "spaces\n   00:00:00.000    -->  00:00:01.000 \n   text0\n\n"
                         "tabs\n\t\t\t00:00:00.000\t\t\t\t-->\t\t00:00:01.000\t\ntext1\n\n"
                         "form feed\n\f\f\f00:00:00.000\f\f\f\f-->\f\f00:00:01.000\f\ntext2\n\n"
                         "vertical tab\n\v\v\v00:00:00.000\v\v\v\v-->\v\v00:00:01.000\v\ninvalid\n");
    CHECK(t && subs_cue_count(t) == 3, "whitespace-chars: FF is whitespace, VT is not -> 3 cues (%d)",
          t ? subs_cue_count(t) : -1);
    if (t) {
        CHECK(strcmp(subs_cue_at(t, 0)->text, "   text0") == 0, "leading spaces in PAYLOAD text are kept");
        subs_close(t);
    }
}

static void test_settings_vertical_and_multiple(void)
{
    subs_track *t = vtt("WEBVTT\n\n"
                         "00:00:00.000 --> 00:00:01.000\ntext0\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical:lr\ntext1\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical:rl\ntext2\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical:rl vertical:lr\ntext3\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical:\ninvalid4\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical:RL\ninvalid5\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical: rl\ninvalid6\n\n"
                         "00:00:00.000 --> 00:00:01.000 vertical:vertical-rl\ninvalid7\n");
    CHECK(t && subs_cue_count(t) == 8, "settings-vertical: 8 cues");
    if (t) {
        subs_vertical want[8] = { SUBS_VERTICAL_NONE, SUBS_VERTICAL_LR, SUBS_VERTICAL_RL, SUBS_VERTICAL_LR,
                                   SUBS_VERTICAL_NONE, SUBS_VERTICAL_NONE, SUBS_VERTICAL_NONE, SUBS_VERTICAL_NONE };
        for (int i = 0; i < 8; i++)
            CHECK(subs_cue_at(t, i)->settings.vertical == want[i], "vertical cue %d", i);
        subs_close(t);
    }

    t = vtt("WEBVTT\n\n"
            "id0\n00:00:00.000 --> 00:00:01.000 align:start line:1% vertical:lr size:50% position:25%\ntext0\n\n"
            "id1\n00:00:00.000 --> 00:00:01.000 align:center line:1 vertical:rl size:0% position:100%\ntext1\n");
    CHECK(t && subs_cue_count(t) == 2, "settings-multiple: 2 cues");
    if (t) {
        const subs_cue *c0 = subs_cue_at(t, 0), *c1 = subs_cue_at(t, 1);
        CHECK(strcmp(c0->id, "id0") == 0 && c0->settings.align == SUBS_ALIGN_START &&
              !c0->settings.line_is_auto && c0->settings.line == 1 && c0->settings.line_is_percent &&
              !c0->settings.snap_to_lines && c0->settings.vertical == SUBS_VERTICAL_LR &&
              c0->settings.size == 50 && c0->settings.position == 25, "settings-multiple cue0");
        CHECK(strcmp(c1->id, "id1") == 0 && c1->settings.align == SUBS_ALIGN_CENTER &&
              c1->settings.line == 1 && !c1->settings.line_is_percent &&
              c1->settings.vertical == SUBS_VERTICAL_RL && c1->settings.size == 0 &&
              c1->settings.position == 100, "settings-multiple cue1");
        subs_close(t);
    }
}

static void test_style_and_region(void)
{
    /* stylesheets.test: two STYLE blocks (one containing a NOTE with a
     * near-miss "arrow" inside a CSS comment) plus two real cues around and
     * between them. */
    subs_track *t = vtt(
        "WEBVTT\n\nSTYLE\n::cue(#foo) {\n    width: 20px;\n} /*\nNOTE hello\n"
        "00:00:00.000 -- > 00:00:01.000\n*/\n.foo {\n    width: 19px;\n}\n\n"
        ".bar {\n    width: 18px;\n}\n\n"
        "foo\n00:00:00.000 --> 00:00:01.000\ntext\n\n"
        "STYLE\n::cue(::bar) {\n    width: 18px;\n}\n\n"
        "bar\n00:00:00.000 --> 00:00:01.000\ntext\n");
    CHECK(t && subs_cue_count(t) == 2, "stylesheets.test: 2 real cues around/after two STYLE blocks (%d)",
          t ? subs_cue_count(t) : -1);
    if (t) {
        CHECK(strcmp(subs_cue_at(t, 0)->id, "foo") == 0, "stylesheets cue0 id");
        CHECK(strcmp(subs_cue_at(t, 1)->id, "bar") == 0, "stylesheets cue1 id");
        subs_close(t);
    }

    /* settings-region.test: duplicate-id regions, last-matching-id wins,
     * token order within one setting line also last-wins. */
    t = vtt("WEBVTT\n\n"
            "REGION\nid:foo\n\nREGION\nid:bar\n\nREGION\nid:foo\n\nREGION\nwidth:10%\n\n"
            "00:00:00.000 --> 00:00:01.000 region:foo\ntext0\n\n"
            "00:00:00.000 --> 00:00:01.000 region:bar\ntext1\n\n"
            "00:00:00.000 --> 00:00:01.000 region:foo region:bar\ntext2\n\n"
            "00:00:00.000 --> 00:00:01.000 region:invalid\ntext3\n\n"
            "00:00:00.000 --> 00:00:01.000 region:invalid region:foo\ntext4\n");
    CHECK(t && subs_region_count(t) == 4 && subs_cue_count(t) == 5, "settings-region: 4 regions, 5 cues");
    if (t) {
        int r_foo_last = subs_cue_at(t, 0)->settings.region;  /* resolves to region index 2, the LAST id:foo */
        int r_bar = subs_cue_at(t, 1)->settings.region;
        CHECK(r_foo_last == 2, "region:foo resolves to the LAST region with that id (idx 2, got %d)", r_foo_last);
        CHECK(r_bar == 1, "region:bar resolves to idx 1 (got %d)", r_bar);
        CHECK(subs_cue_at(t, 2)->settings.region == r_bar, "region:foo region:bar -- last token wins (bar)");
        CHECK(subs_cue_at(t, 3)->settings.region == -1, "region:invalid -> no match");
        CHECK(subs_cue_at(t, 4)->settings.region == r_foo_last, "region:invalid region:foo -- last token wins (foo)");
        subs_close(t);
    }
}

static void test_active_at(void)
{
    subs_track *t = vtt("WEBVTT\n\n"
                         "00:00:00.000 --> 00:00:05.000\nA\n\n"
                         "00:00:03.000 --> 00:00:08.000\nB\n\n"
                         "00:00:10.000 --> 00:00:12.000\nC\n");
    CHECK(t && subs_cue_count(t) == 3, "active_at fixture: 3 cues");
    if (!t) return;
    int idx[8];
    int n = subs_active_at(t, 1000, idx, 8);
    CHECK(n == 1 && idx[0] == 0, "t=1s: only A active (n=%d)", n);
    n = subs_active_at(t, 4000, idx, 8);
    CHECK(n == 2 && idx[0] == 0 && idx[1] == 1, "t=4s: A and B overlap, in start order (n=%d)", n);
    n = subs_active_at(t, 6000, idx, 8);
    CHECK(n == 1 && idx[0] == 1, "t=6s: only B (n=%d)", n);
    n = subs_active_at(t, 9000, idx, 8);
    CHECK(n == 0, "t=9s: gap, nothing active (n=%d)", n);
    n = subs_active_at(t, 5000, idx, 8);
    CHECK(n == 1 && idx[0] == 1, "t=5s: A's end is EXCLUSIVE, only B (n=%d)", n);
    n = subs_active_at(t, 11000, idx, 8);
    CHECK(n == 1 && idx[0] == 2, "t=11s: only C (n=%d)", n);
    /* max_out smaller than the true count: return value is the TRUE count,
     * only the first max_out slots are written -- snprintf's convention. */
    n = subs_active_at(t, 4000, idx, 1);
    CHECK(n == 2 && idx[0] == 0, "t=4s with max_out=1: true count 2, first index still written");
    subs_close(t);
}

static void test_malformed_skip_and_count(void)
{
    /* timings-garbage-shaped: every attempt fails, 0 cues, and each is
     * counted. Two lines here: one with a garbage start timestamp, one with
     * a garbage end timestamp. */
    subs_track *t = vtt("WEBVTT\n\n"
                         "x00:00:00.000 --> 00:00:01.000\ninvalid\n\n"
                         "00:00:00.000 --> x00:00:01.000\ninvalid\n");
    CHECK(t && subs_cue_count(t) == 0, "malformed-only file: 0 cues");
    CHECK(t && subs_skipped_count(t) == 2, "malformed-only file: 2 skipped (%d)", t ? subs_skipped_count(t) : -1);
    subs_close(t);
}

static void test_srt_basic(void)
{
    subs_track *t = srt("1\n00:00:01,000 --> 00:00:04,000\nHello\nworld\n\n"
                         "2\n00:00:05,500 --> 00:00:07,000\nMore text\n");
    CHECK(t && subs_track_format(t) == SUBS_FMT_SRT, "srt basic: format is SRT");
    CHECK(t && subs_cue_count(t) == 2, "srt basic: 2 cues");
    if (t) {
        CHECK(strcmp(subs_cue_at(t, 0)->id, "1") == 0, "srt id 0");
        CHECK(subs_cue_at(t, 0)->start_ms == 1000 && subs_cue_at(t, 0)->end_ms == 4000, "srt times 0");
        CHECK(strcmp(subs_cue_at(t, 0)->text, "Hello\nworld") == 0, "srt multi-line text 0");
        CHECK(subs_cue_at(t, 1)->start_ms == 5500 && subs_cue_at(t, 1)->end_ms == 7000, "srt times 1");
        subs_close(t);
    }
}

static void test_srt_crlf_and_blank_framing(void)
{
    subs_track *t = srt("1\r\n00:00:01,000 --> 00:00:02,000\r\nA\r\n\r\n\r\n"
                         "2\r\n00:00:03,000 --> 00:00:04,000\r\nB\r\n");
    CHECK(t && subs_cue_count(t) == 2, "srt CRLF + multiple blank lines between blocks: 2 cues (%d)",
          t ? subs_cue_count(t) : -1);
    subs_close(t);

    /* No numeric index line at all -- tolerated, id becomes "". */
    t = srt("00:00:01.000 --> 00:00:02.000\nno index here\n");
    CHECK(t && subs_cue_count(t) == 1 && subs_cue_at(t, 0)->id[0] == '\0',
          "srt with no index line: id defaults to empty");
    subs_close(t);
}

static void test_srt_malformed_skip(void)
{
    subs_track *t = srt("1\nnot a timing line\ntext\n\n"
                         "2\n00:00:01,000 --> 00:00:02,000\nvalid\n");
    CHECK(t && subs_cue_count(t) == 1, "srt: 1 valid cue after 1 malformed block");
    CHECK(t && subs_skipped_count(t) == 1, "srt: 1 malformed block counted (%d)", t ? subs_skipped_count(t) : -1);
    if (t) { CHECK(strcmp(subs_cue_at(t, 0)->text, "valid") == 0, "srt: the valid cue survives"); subs_close(t); }
}

static void test_bom_and_sniff(void)
{
    const uint8_t vtt_bom[] = { 0xEF,0xBB,0xBF, 'W','E','B','V','T','T','\n','\n',
        '0','0',':','0','0',':','0','0','.','0','0','0',' ','-','-','>',' ',
        '0','0',':','0','0',':','0','1','.','0','0','0','\n','h','i','\n' };
    subs_format fmt; int err;
    subs_track *t = subs_parse(vtt_bom, sizeof vtt_bom, &fmt, &err);
    CHECK(t && fmt == SUBS_FMT_VTT && subs_cue_count(t) == 1, "sniff: BOM+WEBVTT -> VTT, 1 cue");
    subs_close(t);

    const char *srt_text = "1\n00:00:01,000 --> 00:00:02,000\nhi\n";
    t = subs_parse((const uint8_t *)srt_text, (long)strlen(srt_text), &fmt, &err);
    CHECK(t && fmt == SUBS_FMT_SRT && subs_cue_count(t) == 1, "sniff: no WEBVTT signature -> SRT");
    subs_close(t);
}

static void test_strict_negctl_shape(void)
{
    /* This is NOT what -DSUBS_STRICT tests (that needs a separate build --
     * see tests/subs.mk). It just pins that a file with ZERO malformed cues
     * behaves identically regardless: subs_skipped_count() == 0 here is
     * what makes tests/subs.mk's negative control meaningful -- STRICT can
     * only change behaviour on inputs that have something to be strict
     * about. */
    subs_track *t = vtt("WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nclean\n");
    CHECK(t && subs_cue_count(t) == 1 && subs_skipped_count(t) == 0, "clean file: 0 skipped");
    subs_close(t);
}

static void run_units(void)
{
    test_signature();
    test_header_and_signature_timings();
    test_ids();
    test_arrows_self_resync();
    test_no_blank_between_cues();
    test_newlines();
    test_comment_in_cue_text();
    test_timings_negative_60_omitted_hours();
    test_whitespace_chars();
    test_settings_vertical_and_multiple();
    test_style_and_region();
    test_active_at();
    test_malformed_skip_and_count();
    test_srt_basic();
    test_srt_crlf_and_blank_framing();
    test_srt_malformed_skip();
    test_bom_and_sniff();
    test_strict_negctl_shape();
    if (fails) printf("%d CHECK(S) FAILED\n", fails);
    else printf("all unit checks passed\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: subs_test cues|srt|auto <file> | subs_test units\n"); return 2; }

    if (strcmp(argv[1], "units") == 0) { run_units(); return fails ? 1 : 0; }

    if (argc < 3) { fprintf(stderr, "usage: subs_test %s <file>\n", argv[1]); return 2; }
    long len;
    unsigned char *data = read_all(argv[2], &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }

    int err = SUBS_OK;
    subs_track *t = NULL;
    if (strcmp(argv[1], "cues") == 0) t = subs_parse_vtt(data, len, &err);
    else if (strcmp(argv[1], "srt") == 0) t = subs_parse_srt(data, len, &err);
    else if (strcmp(argv[1], "auto") == 0) { subs_format f; t = subs_parse(data, len, &f, &err); }
    else { fprintf(stderr, "unknown subcommand %s\n", argv[1]); return 2; }

    free(data);
    if (!t) {
        if (err == SUBS_ERR_FORMAT) { printf("FORMAT-ERROR\n"); return 1; }
        if (err == SUBS_ERR_STRICT) { printf("STRICT-ABORT\n"); return 1; }
        printf("PARSE-ERROR %d\n", err);
        return 1;
    }
    dump_track(t);
    subs_close(t);
    return 0;
}
