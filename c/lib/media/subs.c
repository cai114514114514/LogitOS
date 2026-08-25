/* c/lib/media/subs.c -- WebVTT and SRT parsing. See subs.h for the API and
 * the scope this library deliberately does not cover.
 *
 * THE WEBVTT HALF IS A TRANSCRIPTION of https://w3c.github.io/webvtt/
 * section 6, close enough to trace line by line against the spec text:
 *   6.1  "WebVTT parser algorithm" / "collect a WebVTT block"   -> vtt_parse, collect_block
 *   6.2  "WebVTT region settings parsing"                        -> parse_region_settings
 *   6.3  "WebVTT cue timings and settings parsing" /
 *        "collect a WebVTT timestamp"                            -> parse_cue_timings, collect_timestamp
 *   percentage syntax ("parse a percentage string")               -> parse_percentage
 * 6.4 (the cue TEXT node tree) is NOT transcribed -- see subs.h.
 *
 * A handful of behaviours in here look like bugs until you trace the spec,
 * so they are named up front rather than left for a future reader to
 * rediscover by staring at web-platform-tests/webvtt failures:
 *
 *   - A line containing "-->" ends the CURRENT block whether or not it
 *     becomes a cue. If it is not eligible to become one (not the block's
 *     first line, or its second line when the first line already tried and
 *     failed), the block ends WITHOUT consuming that line, and the next
 *     block starts there. This is what makes a garbage line like "foo-->"
 *     self-resynchronising instead of corrupting the rest of the file --
 *     see web-platform-tests' arrows.test, which this file's test corpus
 *     includes.
 *   - Once a line at cue-eligible position DOES contain "-->", the block
 *     commits to trying to parse it as a timing line even if that parse then
 *     fails -- it does not fall back to treating the line as an identifier.
 *     A failed attempt still consumes the line and still marks "an arrow was
 *     seen" for the rest of the block, which is why a bad line-2 timing
 *     attempt does not get a second chance at line 3.
 *   - STYLE and REGION are recognised only as the SECOND line of a block
 *     (line 1 must be exactly "STYLE" or "REGION" with nothing else) and
 *     only before the first cue anywhere in the file. Anything that looks
 *     like a second STYLE/REGION block after a cue -- or any two-line block
 *     that isn't STYLE/REGION/a cue -- is a silent no-op: its lines are
 *     consumed and thrown away. tests/subs.mk's corpus has both shapes.
 *   - WebVTT's "split on spaces" for cue settings and (separately) region
 *     settings BOTH split on any of space/tab/LF/form-feed -- not just
 *     U+0020. This matters only for region settings, whose "input" is
 *     several physical lines joined by LF before splitting: a REGION block
 *     with "id:foo" and "lines:2" on separate physical lines has to come out
 *     as two tokens, not one token containing an embedded newline. Verified
 *     against regions-lines.test / regions-id.test, which rely on exactly
 *     this multi-line splitting to disambiguate which of several same-id
 *     regions a cue's "region:" setting picks up (the LAST matching one).
 *   - U+000B VERTICAL TAB is deliberately NOT whitespace here (only SPACE,
 *     TAB and FORM FEED are, matching the WebVTT/Infra "ASCII whitespace"
 *     set minus CR, which normalization has already turned into LF). A cue
 *     timing line padded with form feeds parses; one padded with vertical
 *     tabs does not. whitespace-chars.test is built to catch exactly a
 *     4/5-whitespace-character mixup here.
 *   - A double-and-add "let cue's line be number" applies ONLY after BOTH
 *     the numeric part and (if present) the comma-alignment part validate;
 *     a syntactically fine number followed by an invalid alignment keyword
 *     discards the WHOLE "line:" setting, not just the alignment half.
 *     settings-line.test's `line:100%,` case (trailing comma, empty
 *     alignment) is the one that catches a parser that applies the number
 *     unconditionally.
 *   - Successfully setting `vertical` to rl/lr, or `line` to a non-auto
 *     value, or `size` to anything but 100, each clear the cue's region
 *     (there are no vertical/offset/resized regions) -- and because cue
 *     settings apply left to right, a `region:x vertical:lr` loses the
 *     region while `vertical:lr region:x` keeps it. Order-dependent on
 *     purpose; not a bug to "simplify" into an unconditional order.
 *
 * EVERY INPUT BYTE IS UNTRUSTED, the same rule media.h states for
 * containers: nothing here reads outside the buffer the caller handed in
 * regardless of what the file's own lengths/counts claim, and ceilings
 * (SUBS_MAX_*) bound every allocation to a multiple of the input size, never
 * to a value the file gets to pick unboundedly.
 *
 * "MALFORMED CUE", for subs_skipped_count() and -DSUBS_STRICT, means
 * specifically: a line that WAS eligible to start a cue (WebVTT: line 1 or
 * line 2 of a block, containing "-->") and DID fail to parse as valid
 * timings, or (SRT) a block that reached its timing-line position and could
 * not be parsed as one. It does NOT count the WebVTT spec's own silent
 * orphan-block cases above (a STYLE/REGION-shaped block that arrives after
 * the first cue, a two-line block matching neither) -- those are correct,
 * specified behaviour, not malformed input, and counting them would make
 * "skipped" fire on every ordinary file with more than one STYLE block.
 */
#include "subs.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------- ceilings */
#define SUBS_MAX_FILE_LEN   (64L * 1024 * 1024)
#define SUBS_MAX_LINES      (SUBS_MAX_FILE_LEN)   /* one line can be 1 byte */

/* ------------------------------------------------------------- utilities -- */
static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\f'; }
static int is_ws_nl(int c) { return is_ws(c) || c == '\n'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

static void skip_ws(const char *s, long n, long *pos)
{
    while (*pos < n && is_ws((unsigned char)s[*pos])) (*pos)++;
}

/* Substring search bounded by length (no NUL assumption -- cue text can
 * legitimately contain embedded NULs, replaced by U+FFFD upstream, but the
 * *line* buffers here are never NUL-terminated storage in the first place). */
static int contains(const char *s, long n, const char *needle)
{
    long nn = (long)strlen(needle);
    if (nn == 0 || n < nn) return 0;
    for (long i = 0; i <= n - nn; i++)
        if (memcmp(s + i, needle, (size_t)nn) == 0) return 1;
    return 0;
}

static char *dupn(const char *s, long n, long cap)
{
    if (n > cap) n = cap;
    if (n < 0) n = 0;
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------- line-ending norm - */
/* Spec 6: NUL -> U+FFFD, CRLF -> LF, lone CR -> LF, all before anything else
 * looks at the bytes. Applied to BOTH formats -- SRT has no such rule of its
 * own, but a CRLF-authored .srt is the overwhelmingly common case in
 * practice and there is no reading of "keep the CR" that helps a caller. */
static uint8_t *normalize(const uint8_t *data, long len, long *out_len)
{
    uint8_t *buf = (uint8_t *)malloc((size_t)len * 3 + 1);
    if (!buf) return NULL;
    long o = 0;
    for (long i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == 0) {
            buf[o++] = 0xEF; buf[o++] = 0xBF; buf[o++] = 0xBD; /* U+FFFD */
        } else if (c == '\r') {
            buf[o++] = '\n';
            if (i + 1 < len && data[i + 1] == '\n') i++;
        } else {
            buf[o++] = c;
        }
    }
    *out_len = o;
    return buf;
}

typedef struct { const char *p; long n; } line_t;

static line_t *split_lines(const char *buf, long len, long *out_n)
{
    long cap = 64, n = 0;
    line_t *lines = (line_t *)malloc((size_t)cap * sizeof *lines);
    if (!lines) return NULL;
    long start = 0;
    for (long i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            if (n == cap) {
                cap *= 2;
                line_t *nl = (line_t *)realloc(lines, (size_t)cap * sizeof *nl);
                if (!nl) { free(lines); return NULL; }
                lines = nl;
            }
            lines[n].p = buf + start;
            lines[n].n = i - start;
            n++;
            start = i + 1;
        }
    }
    *out_n = n;
    return lines;
}

/* --------------------------------------------------- percentage / numbers - */
/* "WebVTT percentage": one or more digits, optionally '.' + one or more
 * digits, then exactly one trailing '%' and nothing else. */
static int parse_percentage(const char *s, long n, double *out)
{
    if (n < 2 || s[n - 1] != '%') return 0;
    long body = n - 1;
    long i = 0, digits_before = 0, digits_after = 0;
    int seen_dot = 0;
    for (; i < body; i++) {
        if (is_digit((unsigned char)s[i])) { if (seen_dot) digits_after++; else digits_before++; }
        else if (s[i] == '.' && !seen_dot) seen_dot = 1;
        else return 0;
    }
    if (digits_before == 0) return 0;
    if (seen_dot && digits_after == 0) return 0;
    char tmp[512];
    if (body >= (long)sizeof tmp) return 0;   /* absurd length; not a real percentage */
    memcpy(tmp, s, (size_t)body); tmp[body] = '\0';
    double v = strtod(tmp, NULL);
    if (!(v >= 0.0 && v <= 100.0)) return 0;
    *out = v;
    return 1;
}

/* HTML "rules for parsing floating-point number values", restricted to the
 * character class 6.3 allows for a bare `line` value: optional leading '-',
 * digits, at most one '.', a digit on both sides of it. Overflow to +-Inf
 * is a parse ERROR in the HTML algorithm (not saturation) -- settings-line's
 * one-ULP-past-DBL_MAX cases depend on that. */
static int parse_signed_number(const char *s, long n, double *out)
{
    if (n == 0) return 0;
    long i = 0;
    if (s[0] == '-') {
        i = 1;
        for (long j = 1; j < n; j++) if (s[j] == '-') return 0; /* '-' only first */
    }
    int seen_dot = 0, digits_before = 0, digits_after = 0;
    for (long j = i; j < n; j++) {
        char c = s[j];
        if (is_digit((unsigned char)c)) { if (seen_dot) digits_after++; else digits_before++; }
        else if (c == '.') { if (seen_dot) return 0; seen_dot = 1; }
        else return 0;
    }
    if (digits_before == 0) return 0;
    if (seen_dot && digits_after == 0) return 0;
    if (seen_dot && s[i + digits_before] != '.') return 0; /* dot not adjacent, defensive */
    char tmp[1024];
    if (n >= (long)sizeof tmp) return 0;
    memcpy(tmp, s, (size_t)n); tmp[n] = '\0';
    double v = strtod(tmp, NULL);
    if (!isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* -------------------------------------------------------- WebVTT timestamp */
/* Spec 6.3 "collect a WebVTT timestamp". Digit runs are accumulated with a
 * saturating multiply -- real timestamps are tiny; an attacker-chosen
 * million-digit run must not overflow, it must just saturate. */
static int collect_digits_sat(const char *s, long n, long *pos, long *out_len, int64_t *out_val)
{
    long start = *pos;
    int64_t v = 0;
    while (*pos < n && is_digit((unsigned char)s[*pos])) {
        int64_t d = s[*pos] - '0';
        if (v > (INT64_MAX - d) / 10) v = INT64_MAX / 2; /* saturate, stay finite */
        else v = v * 10 + d;
        (*pos)++;
    }
    *out_len = *pos - start;
    *out_val = v;
    return *out_len > 0;
}

static int collect_timestamp(const char *s, long n, long *pos, int64_t *out_ms)
{
    long p = *pos;
    if (p >= n || !is_digit((unsigned char)s[p])) return 0;

    long len1; int64_t v1;
    if (!collect_digits_sat(s, n, &p, &len1, &v1)) return 0;
    int most_sig_hours = (len1 != 2 || v1 > 59);

    if (p >= n || s[p] != ':') return 0;
    p++;
    long len2; int64_t v2;
    if (!collect_digits_sat(s, n, &p, &len2, &v2)) return 0;
    if (len2 != 2) return 0;

    int64_t v3;
    if (most_sig_hours || (p < n && s[p] == ':')) {
        if (p >= n || s[p] != ':') return 0;
        p++;
        long len3; int64_t vv3;
        if (!collect_digits_sat(s, n, &p, &len3, &vv3)) return 0;
        if (len3 != 2) return 0;
        v3 = vv3;
    } else {
        v3 = v2; v2 = v1; v1 = 0;
    }

    if (p >= n || s[p] != '.') return 0;
    p++;
    long len4; int64_t v4;
    if (!collect_digits_sat(s, n, &p, &len4, &v4)) return 0;
    if (len4 != 3) return 0;

    if (v2 > 59 || v3 > 59) return 0;

    /* v1 (hours) is saturating-clamped above; the multiply below can still
     * overflow for an adversarial hour count, so clamp the final product
     * rather than let it wrap. Real content never comes close. */
    if (v1 > (INT64_MAX - v4 - v3 * 1000 - v2 * 60000) / 3600000) v1 = INT64_MAX / 3600000;
    *out_ms = v1 * 3600000 + v2 * 60000 + v3 * 1000 + v4;
    *pos = p;
    return 1;
}

/* --------------------------------------------------------- region settings */
static void region_defaults(subs_region *r)
{
    memset(r, 0, sizeof *r);
    r->width = 100; r->lines = 3;
    r->anchor_x = 0; r->anchor_y = 100;
    r->viewport_x = 0; r->viewport_y = 100;
    r->scroll = SUBS_SCROLL_NONE;
}

static void parse_region_settings(const char *buf, long n, subs_region *r)
{
    long i = 0;
    while (i < n) {
        while (i < n && is_ws_nl((unsigned char)buf[i])) i++;
        long tok_start = i;
        while (i < n && !is_ws_nl((unsigned char)buf[i])) i++;
        long tok_len = i - tok_start;
        if (tok_len == 0) continue;
        const char *tok = buf + tok_start;
        const char *colon = (const char *)memchr(tok, ':', (size_t)tok_len);
        if (!colon) continue;
        long name_len = colon - tok;
        if (name_len == 0 || name_len == tok_len - 1) continue; /* colon first/last */
        const char *val = colon + 1;
        long val_len = tok_len - name_len - 1;

        if (name_len == 2 && memcmp(tok, "id", 2) == 0) {
            long cap = (long)sizeof r->id - 1;
            long m = val_len < cap ? val_len : cap;
            memcpy(r->id, val, (size_t)m); r->id[m] = '\0';
        } else if (name_len == 5 && memcmp(tok, "width", 5) == 0) {
            double v; if (parse_percentage(val, val_len, &v)) r->width = v;
        } else if (name_len == 5 && memcmp(tok, "lines", 5) == 0) {
            int all_digit = val_len > 0;
            for (long k = 0; k < val_len; k++) if (!is_digit((unsigned char)val[k])) { all_digit = 0; break; }
            if (all_digit) {
                int64_t v = 0;
                for (long k = 0; k < val_len; k++) {
                    int64_t d = val[k] - '0';
                    if (v > (2147483647LL - d) / 10) { v = 2147483647LL; break; }
                    v = v * 10 + d;
                }
                r->lines = (int)v;
            }
        } else if (name_len == 12 && memcmp(tok, "regionanchor", 12) == 0) {
            const char *comma = (const char *)memchr(val, ',', (size_t)val_len);
            if (comma) {
                double ax, ay;
                if (parse_percentage(val, comma - val, &ax) &&
                    parse_percentage(comma + 1, val_len - (comma - val) - 1, &ay)) {
                    r->anchor_x = ax; r->anchor_y = ay;
                }
            }
        } else if (name_len == 14 && memcmp(tok, "viewportanchor", 14) == 0) {
            const char *comma = (const char *)memchr(val, ',', (size_t)val_len);
            if (comma) {
                double ax, ay;
                if (parse_percentage(val, comma - val, &ax) &&
                    parse_percentage(comma + 1, val_len - (comma - val) - 1, &ay)) {
                    r->viewport_x = ax; r->viewport_y = ay;
                }
            }
        } else if (name_len == 6 && memcmp(tok, "scroll", 6) == 0) {
            if (val_len == 2 && memcmp(val, "up", 2) == 0) r->scroll = SUBS_SCROLL_UP;
        }
    }
}

/* ------------------------------------------------------------ cue settings */
static void settings_defaults(subs_settings *st)
{
    memset(st, 0, sizeof *st);
    st->vertical = SUBS_VERTICAL_NONE;
    st->snap_to_lines = 1;
    st->line_is_auto = 1;
    st->line_align = SUBS_LALIGN_START;
    st->position_is_auto = 1;
    st->position_align = SUBS_PALIGN_AUTO;
    st->size = 100;
    st->align = SUBS_ALIGN_CENTER;
    st->region = -1;
}

static int region_lookup(const subs_region *regions, int nregions, const char *val, long vlen)
{
    for (int i = nregions - 1; i >= 0; i--) {
        long idlen = (long)strlen(regions[i].id);
        if (idlen == vlen && memcmp(regions[i].id, val, (size_t)vlen) == 0) return i;
    }
    return -1;
}

static void apply_cue_setting(subs_settings *st, const char *name, long nlen,
                               const char *val, long vlen,
                               const subs_region *regions, int nregions)
{
    if (nlen == 6 && memcmp(name, "region", 6) == 0) {
        st->region = region_lookup(regions, nregions, val, vlen);
    } else if (nlen == 8 && memcmp(name, "vertical", 8) == 0) {
        if (vlen == 2 && memcmp(val, "rl", 2) == 0) st->vertical = SUBS_VERTICAL_RL;
        else if (vlen == 2 && memcmp(val, "lr", 2) == 0) st->vertical = SUBS_VERTICAL_LR;
        else return;
        st->region = -1;
    } else if (nlen == 4 && memcmp(name, "line", 4) == 0) {
        const char *comma = (const char *)memchr(val, ',', (size_t)vlen);
        long poslen = comma ? (comma - val) : vlen;
        const char *linealign = comma ? comma + 1 : NULL;
        long alignlen = comma ? vlen - poslen - 1 : 0;
        int has_digit = 0;
        for (long k = 0; k < poslen; k++) if (is_digit((unsigned char)val[k])) { has_digit = 1; break; }
        if (!has_digit) return;
        double number; int is_pct;
        if (val[poslen - 1] == '%') {
            if (!parse_percentage(val, poslen, &number)) return;
            is_pct = 1;
        } else {
            if (!parse_signed_number(val, poslen, &number)) return;
            is_pct = 0;
        }
        subs_line_align new_align = st->line_align;
        int align_set = 0;
        if (linealign) {
            if (alignlen == 5 && memcmp(linealign, "start", 5) == 0) { new_align = SUBS_LALIGN_START; align_set = 1; }
            else if (alignlen == 6 && memcmp(linealign, "center", 6) == 0) { new_align = SUBS_LALIGN_CENTER; align_set = 1; }
            else if (alignlen == 3 && memcmp(linealign, "end", 3) == 0) { new_align = SUBS_LALIGN_END; align_set = 1; }
            else return; /* comma present but not a valid keyword: whole setting discarded */
        }
        st->line = number; st->line_is_auto = 0; st->line_is_percent = is_pct;
        if (align_set) st->line_align = new_align;
        st->snap_to_lines = !is_pct;
        st->region = -1;
    } else if (nlen == 8 && memcmp(name, "position", 8) == 0) {
        const char *comma = (const char *)memchr(val, ',', (size_t)vlen);
        long poslen = comma ? (comma - val) : vlen;
        const char *colalign = comma ? comma + 1 : NULL;
        long alignlen = comma ? vlen - poslen - 1 : 0;
        double number;
        if (!parse_percentage(val, poslen, &number)) return;
        subs_pos_align new_align = SUBS_PALIGN_AUTO;
        if (colalign) {
            if (alignlen == 9 && memcmp(colalign, "line-left", 9) == 0) new_align = SUBS_PALIGN_LINE_LEFT;
            else if (alignlen == 6 && memcmp(colalign, "center", 6) == 0) new_align = SUBS_PALIGN_CENTER;
            else if (alignlen == 10 && memcmp(colalign, "line-right", 10) == 0) new_align = SUBS_PALIGN_LINE_RIGHT;
            else return;
        }
        st->position = number; st->position_is_auto = 0; st->position_align = new_align;
    } else if (nlen == 4 && memcmp(name, "size", 4) == 0) {
        double number;
        if (!parse_percentage(val, vlen, &number)) return;
        st->size = number;
        if (number != 100) st->region = -1;
    } else if (nlen == 5 && memcmp(name, "align", 5) == 0) {
        if (vlen == 5 && memcmp(val, "start", 5) == 0) st->align = SUBS_ALIGN_START;
        else if (vlen == 6 && memcmp(val, "center", 6) == 0) st->align = SUBS_ALIGN_CENTER;
        else if (vlen == 3 && memcmp(val, "end", 3) == 0) st->align = SUBS_ALIGN_END;
        else if (vlen == 4 && memcmp(val, "left", 4) == 0) st->align = SUBS_ALIGN_LEFT;
        else if (vlen == 5 && memcmp(val, "right", 5) == 0) st->align = SUBS_ALIGN_RIGHT;
    }
}

static void parse_cue_settings(const char *buf, long n, subs_settings *st,
                                const subs_region *regions, int nregions)
{
    long i = 0;
    while (i < n) {
        while (i < n && is_ws_nl((unsigned char)buf[i])) i++;
        long tok_start = i;
        while (i < n && !is_ws_nl((unsigned char)buf[i])) i++;
        long tok_len = i - tok_start;
        if (tok_len == 0) continue;
        const char *tok = buf + tok_start;
        const char *colon = (const char *)memchr(tok, ':', (size_t)tok_len);
        if (!colon) continue;
        long name_len = colon - tok;
        if (name_len == 0 || name_len == tok_len - 1) continue;
        apply_cue_setting(st, tok, name_len, colon + 1, tok_len - name_len - 1, regions, nregions);
    }
}

/* --------------------------------------------- cue timings and settings -- */
static int parse_cue_timings(const char *line, long n, int64_t *start_ms, int64_t *end_ms,
                              subs_settings *st, const subs_region *regions, int nregions)
{
    long pos = 0;
    skip_ws(line, n, &pos);
    if (!collect_timestamp(line, n, &pos, start_ms)) return 0;
    skip_ws(line, n, &pos);
    if (pos >= n || line[pos] != '-') return 0;
    pos++;
    if (pos >= n || line[pos] != '-') return 0;
    pos++;
    if (pos >= n || line[pos] != '>') return 0;
    pos++;
    skip_ws(line, n, &pos);
    if (!collect_timestamp(line, n, &pos, end_ms)) return 0;
    settings_defaults(st);
    parse_cue_settings(line + pos, n - pos, st, regions, nregions);
    return 1;
}

/* ----------------------------------------------------------- cue storage - */
typedef struct { subs_cue cue; long orig_order; } cue_slot;

struct subs_track {
    subs_format fmt;
    cue_slot   *slots;
    int         ncues, capcues;
    subs_region *regions;
    int         nregions;
    int         skipped;
    int64_t    *pmax;      /* prefix max of end_ms over the sorted array */
};

static cue_slot *push_cue(subs_track *tr)
{
    if (tr->ncues >= SUBS_MAX_CUES) return NULL;
    if (tr->ncues == tr->capcues) {
        int nc = tr->capcues ? tr->capcues * 2 : 64;
        cue_slot *ns = (cue_slot *)realloc(tr->slots, (size_t)nc * sizeof *ns);
        if (!ns) return NULL;
        tr->slots = ns; tr->capcues = nc;
    }
    cue_slot *s = &tr->slots[tr->ncues];
    memset(s, 0, sizeof *s);
    s->orig_order = tr->ncues;
    tr->ncues++;
    return s;
}

static int cmp_slot(const void *a, const void *b)
{
    const cue_slot *x = (const cue_slot *)a, *y = (const cue_slot *)b;
    if (x->cue.start_ms < y->cue.start_ms) return -1;
    if (x->cue.start_ms > y->cue.start_ms) return 1;
    if (x->orig_order < y->orig_order) return -1;
    if (x->orig_order > y->orig_order) return 1;
    return 0;
}

static void finish_track(subs_track *tr)
{
    if (tr->ncues > 1) qsort(tr->slots, (size_t)tr->ncues, sizeof *tr->slots, cmp_slot);
    if (tr->ncues > 0) {
        tr->pmax = (int64_t *)malloc((size_t)tr->ncues * sizeof *tr->pmax);
        if (tr->pmax) {
            int64_t m = INT64_MIN;
            for (int i = 0; i < tr->ncues; i++) {
                if (tr->slots[i].cue.end_ms > m) m = tr->slots[i].cue.end_ms;
                tr->pmax[i] = m;
            }
        }
    }
}

/* ============================================================ WebVTT ==== */
/* -DSUBS_STRICT: the negative control. A conforming parser skips a
 * malformed cue and keeps going (see this file's header). Built with
 * SUBS_STRICT, the FIRST one instead aborts the whole parse with
 * SUBS_ERR_STRICT -- tests/subs.mk requires this to redden exactly the
 * fixtures that contain a malformed cue, and pass unchanged on every
 * fixture that does not.
 *
 * Deliberately NOT a macro that itself `return`s: the two call sites
 * (collect_block, whose return type is a block-kind int with the real
 * error threaded out through an `int *err` parameter, and
 * subs_parse_srt, which returns a subs_track*) need different return
 * statements. A macro that hides a `return` inside it produced exactly
 * that bug on the first draft of this file: in collect_block it returned
 * SUBS_ERR_STRICT (-4) AS THE BLOCK KIND, past the `*err` check that was
 * supposed to see it, so a strict build silently kept parsing instead of
 * aborting. Each call site now sets `tr->skipped` here and handles its
 * own abort explicitly, right where its own return statement is. */
static void note_malformed(subs_track *tr) { tr->skipped++; }

enum block_kind { BLOCK_NONE, BLOCK_CUE, BLOCK_STYLE, BLOCK_REGION };

/* One "collect a WebVTT block". `*io_i` is the 0-based line index the block
 * starts at; on return it is the index the NEXT block should start at
 * (which, for a rewind, is the very line this call was asked to start
 * reading STYLE/REGION content from -- see the file header). Returns the
 * kind of block collected (or BLOCK_NONE for an orphan block, thrown away).
 * On BLOCK_CUE, *out_cue is fully populated except for `text`, which the
 * caller fills from the returned buffer. Returns a negative subs_track error
 * only when SUBS_STRICT demands the whole parse abort. */
static int collect_block(const line_t *lines, long nlines, long *io_i, int in_header,
                          const subs_region *regions, int nregions, int *seen_cue_g,
                          subs_cue *out_cue, subs_region *out_region, char **out_text,
                          subs_track *tr, int *err)
{
    *err = SUBS_OK;
    long i = *io_i;
    long prev_i = i;
    int line_count = 0, seen_arrow = 0;
    char *buf = NULL; long buf_len = 0, buf_cap = 0;
    int kind = BLOCK_NONE;
    int have_cue = 0;

#define BUF_APPEND(p_, n_) do { \
        long need = buf_len + (n_) + 1; \
        if (need > buf_cap) { \
            long ncap = buf_cap ? buf_cap * 2 : 256; \
            while (ncap < need) ncap *= 2; \
            char *nb = (char *)realloc(buf, (size_t)ncap); \
            if (!nb) { free(buf); *io_i = nlines; *err = SUBS_ERR_OOM; return BLOCK_NONE; } \
            buf = nb; buf_cap = ncap; \
        } \
        memcpy(buf + buf_len, (p_), (size_t)(n_)); buf_len += (n_); \
    } while (0)

    for (;;) {
        int is_eof = (i >= nlines);
        const char *lp = is_eof ? "" : lines[i].p;
        long ln = is_eof ? 0 : lines[i].n;
        line_count++;

        if (!is_eof && contains(lp, ln, "-->")) {
            int eligible = !in_header && (line_count == 1 || (line_count == 2 && !seen_arrow));
            if (eligible) {
                seen_arrow = 1;
                prev_i = i + 1;
                int64_t s, e;
                subs_settings st;
                /* NOT `&& e >= s`: web-platform-tests' timings-negative.test
                 * requires accepting end < start (e.g. "00:00:01.000 -->
                 * 00:00:00.999") as a VALID cue -- the file-parsing
                 * algorithm has no such check, only rendering would ever
                 * treat it specially (a cue whose interval is empty is
                 * simply never active, which subs_active_at already gets
                 * right for free: start_ms <= t < end_ms can't hold when
                 * end_ms <= start_ms). An earlier draft added this
                 * rejection on the theory that subs_active_at's invariant
                 * needed it; it does not, and the WPT corpus makes the
                 * over-strict version an actual defect, not just an
                 * over-eager guess -- see subs.h's contract comment. */
                if (parse_cue_timings(lp, ln, &s, &e, &st, regions, nregions)) {
                    out_cue->id = dupn(buf, buf_len, SUBS_MAX_ID_LEN);
                    out_cue->start_ms = s; out_cue->end_ms = e; out_cue->settings = st;
                    buf_len = 0;
                    have_cue = 1; kind = BLOCK_CUE;
                    *seen_cue_g = 1;
                } else {
                    note_malformed(tr);
#ifdef SUBS_STRICT
                    free(buf); *io_i = i + 1; *err = SUBS_ERR_STRICT; return BLOCK_NONE;
#endif
                }
                i++;
                continue;
            } else {
                /* Rewind WITHOUT consuming this line -- the next block starts
                 * here. This is reachable with have_cue already true: a cue
                 * created at line 1 or 2, followed by plain payload lines and
                 * then a THIRD line that also contains "-->" (the next cue's
                 * timing line, arriving with no blank-line separator -- see
                 * arrows.test's "text4" case in this file's header comment).
                 * Falling through to the shared post-loop code below is what
                 * finalizes that already-made cue's text from `buf`; an
                 * earlier draft `return`ed straight from here instead and
                 * silently discarded it. */
                *io_i = prev_i;
                break;
            }
        } else if (ln == 0) {
            /* blank line (or EOF-as-blank): consume it, block ends here */
            i++;
            *io_i = i;
            break;
        } else {
            if (!in_header && line_count == 2 && !*seen_cue_g && kind == BLOCK_NONE) {
                int is_style = buf_len >= 5 && memcmp(buf, "STYLE", 5) == 0;
                int is_region = buf_len >= 6 && memcmp(buf, "REGION", 6) == 0;
                if (is_style) {
                    int ok = 1;
                    for (long k = 5; k < buf_len; k++) if (!is_ws((unsigned char)buf[k])) { ok = 0; break; }
                    if (ok) { kind = BLOCK_STYLE; buf_len = 0; }
                } else if (is_region) {
                    int ok = 1;
                    for (long k = 6; k < buf_len; k++) if (!is_ws((unsigned char)buf[k])) { ok = 0; break; }
                    if (ok) { kind = BLOCK_REGION; buf_len = 0; region_defaults(out_region); }
                }
            }
            if (buf_len > 0) BUF_APPEND("\n", 1);
            BUF_APPEND(lp, ln);
            prev_i = i + 1;
            i++;
        }
        if (is_eof) { *io_i = i; break; }
    }

    if (have_cue) {
        *out_text = dupn(buf, buf_len, SUBS_MAX_TEXT_LEN);
        free(buf);
        return BLOCK_CUE;
    }
    if (kind == BLOCK_STYLE) { free(buf); *out_text = NULL; return BLOCK_STYLE; }
    if (kind == BLOCK_REGION) {
        parse_region_settings(buf, buf_len, out_region);
        free(buf);
        *out_text = NULL;
        return BLOCK_REGION;
    }
    free(buf);
    *out_text = NULL;
    return BLOCK_NONE;
#undef BUF_APPEND
}

subs_track *subs_parse_vtt(const uint8_t *data, long len, int *out_err)
{
    if (out_err) *out_err = SUBS_OK;
    if (len < 0 || len > SUBS_MAX_FILE_LEN) { if (out_err) *out_err = SUBS_ERR_RANGE; return NULL; }
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) { data += 3; len -= 3; }

    long nlen;
    uint8_t *norm = normalize(data, len, &nlen);
    if (!norm) { if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }
    const char *buf = (const char *)norm;

    /* Signature: spec 6.1's three exact rules. A byte-level check of byte 6
     * against {SPACE,TAB,LF} is equivalent to a codepoint-level one here --
     * no multi-byte UTF-8 lead byte ever equals one of those three values. */
    if (nlen < 6 || memcmp(buf, "WEBVTT", 6) != 0 ||
        (nlen > 6 && buf[6] != ' ' && buf[6] != '\t' && buf[6] != '\n')) {
        free(norm);
        if (out_err) *out_err = SUBS_ERR_FORMAT;
        return NULL;
    }

    subs_track *tr = (subs_track *)calloc(1, sizeof *tr);
    if (!tr) { free(norm); if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }
    tr->fmt = SUBS_FMT_VTT;
    tr->regions = (subs_region *)calloc(SUBS_MAX_REGIONS, sizeof *tr->regions);
    if (!tr->regions) { free(tr); free(norm); if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }

    /* `lines[]` holds POINTERS INTO `norm` (see split_lines) -- norm must
     * stay alive for the whole parse, not just through split_lines itself.
     * An earlier draft freed it right here and every subsequent line access
     * was a use-after-free that happened not to crash until ASan looked. */
    long nlines;
    line_t *lines = split_lines(buf, nlen, &nlines);
    if (!lines) { free(norm); subs_close(tr); if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }

    /* line 0 is the signature line; consume it and the newline that must
     * follow (spec's "collect not-LF, then require LF, else the file was
     * successfully processed with no cues"). Splitting on '\n' already broke
     * the signature's own trailing text into line[0] entirely, so line 1 is
     * exactly where the header (or, if blank, the body) begins. */
    long li = 1;
    if (li >= nlines) { free(lines); free(norm); return tr; } /* "WEBVTT" with no trailing LF at all */

    int seen_cue = 0;
    int err = SUBS_OK;

    /* Header: one collect-a-WebVTT-block call with in_header set, UNLESS the
     * very next line is already blank (an empty header). */
    if (lines[li].n != 0) {
        subs_cue dummy_cue; subs_region dummy_region; char *dummy_text = NULL;
        collect_block(lines, nlines, &li, 1, tr->regions, tr->nregions, &seen_cue,
                      &dummy_cue, &dummy_region, &dummy_text, tr, &err);
        free(dummy_text);
        if (err) { free(lines); free(norm); subs_close(tr); if (out_err) *out_err = err; return NULL; }
    } else {
        li++;
    }
    while (li < nlines && lines[li].n == 0) li++;

    while (li < nlines) {
        subs_cue cue; subs_region region; char *text = NULL;
        memset(&cue, 0, sizeof cue);
        int kind = collect_block(lines, nlines, &li, 0, tr->regions, tr->nregions, &seen_cue,
                                  &cue, &region, &text, tr, &err);
        if (err) { free(lines); free(norm); subs_close(tr); if (out_err) *out_err = err; return NULL; }
        if (kind == BLOCK_CUE) {
            cue.text = text ? text : dupn("", 0, 0);
            cue_slot *slot = push_cue(tr);
            if (!slot) {
                free((void *)cue.id); free((void *)cue.text); free(lines); free(norm); subs_close(tr);
                if (out_err) *out_err = SUBS_ERR_RANGE;
                return NULL;
            }
            slot->cue = cue;
        } else if (kind == BLOCK_REGION) {
            if (tr->nregions < SUBS_MAX_REGIONS) tr->regions[tr->nregions++] = region;
        }
        while (li < nlines && lines[li].n == 0) li++;
    }

    free(lines);
    free(norm);
    finish_track(tr);
    return tr;
}

/* ============================================================== SRT ===== */
/* No W3C/IETF spec to transcribe -- SRT is a de facto format. The grammar
 * implemented is the one every real encoder/decoder agrees on: an optional
 * all-digit index line, a full h:mm:ss,mmm --> h:mm:ss,mmm timing line
 * (comma OR dot as the fractional separator -- both appear in the wild),
 * then text lines to the next blank line. No positioning extensions (a few
 * players' X1/X2/Y1/Y2 pixel-box suffix) are parsed: converting a pixel box
 * to the percentage-based subs_settings WebVTT uses needs the video's
 * dimensions, which are not available here -- see subs.h. */
static int srt_timestamp(const char *s, long n, long *pos, int64_t *out_ms)
{
    long p = *pos;
    long h_start = p;
    while (p < n && is_digit((unsigned char)s[p])) p++;
    if (p == h_start) return 0;
    int64_t h = 0;
    for (long k = h_start; k < p; k++) { int64_t d = s[k] - '0';
        h = (h > (INT64_MAX - d) / 10) ? INT64_MAX / 2 : h * 10 + d; }
    if (p >= n || s[p] != ':') return 0;
    p++;
    if (p + 2 > n || !is_digit((unsigned char)s[p]) || !is_digit((unsigned char)s[p+1])) return 0;
    int mm = (s[p]-'0')*10 + (s[p+1]-'0'); p += 2;
    if (mm > 59) return 0;
    if (p >= n || s[p] != ':') return 0;
    p++;
    if (p + 2 > n || !is_digit((unsigned char)s[p]) || !is_digit((unsigned char)s[p+1])) return 0;
    int ss = (s[p]-'0')*10 + (s[p+1]-'0'); p += 2;
    if (ss > 59) return 0;
    if (p >= n || (s[p] != ',' && s[p] != '.')) return 0;
    p++;
    if (p + 3 > n || !is_digit((unsigned char)s[p]) || !is_digit((unsigned char)s[p+1]) || !is_digit((unsigned char)s[p+2])) return 0;
    int ms = (s[p]-'0')*100 + (s[p+1]-'0')*10 + (s[p+2]-'0'); p += 3;
    /* Same saturating-multiply guard collect_timestamp() has, and the same
     * bug class the fuzzer found here first: `h` can already be
     * INT64_MAX/2 (its own saturation clamp above, for an absurd digit
     * run), and INT64_MAX/2 * 3600000 overflows a signed 64-bit multiply --
     * UBSan catches it even though the saturated value was never going to
     * be a real duration. */
    int64_t rest = (int64_t)mm * 60000 + (int64_t)ss * 1000 + ms;
    if (h > (INT64_MAX - rest) / 3600000) h = INT64_MAX / 3600000;
    *out_ms = h * 3600000 + rest;
    *pos = p;
    return 1;
}

static int srt_timing_line(const char *s, long n, int64_t *start, int64_t *end)
{
    long pos = 0;
    skip_ws(s, n, &pos);
    if (!srt_timestamp(s, n, &pos, start)) return 0;
    skip_ws(s, n, &pos);
    if (pos >= n || s[pos] != '-') return 0;
    pos++;
    if (pos >= n || s[pos] != '-') return 0;
    pos++;
    if (pos >= n || s[pos] != '>') return 0;
    pos++;
    skip_ws(s, n, &pos);
    if (!srt_timestamp(s, n, &pos, end)) return 0;
    return 1;
}

subs_track *subs_parse_srt(const uint8_t *data, long len, int *out_err)
{
    if (out_err) *out_err = SUBS_OK;
    if (len < 0 || len > SUBS_MAX_FILE_LEN) { if (out_err) *out_err = SUBS_ERR_RANGE; return NULL; }
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) { data += 3; len -= 3; }

    long nlen;
    uint8_t *norm = normalize(data, len, &nlen);
    if (!norm) { if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }

    /* Same lifetime rule as subs_parse_vtt: `lines[]` points INTO `norm`
     * (id_p below is one of those pointers, read much later via dupn), so
     * norm must outlive the whole function, not just split_lines. */
    long nlines;
    line_t *lines = split_lines((const char *)norm, nlen, &nlines);
    if (!lines) { free(norm); if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }

    subs_track *tr = (subs_track *)calloc(1, sizeof *tr);
    if (!tr) { free(lines); free(norm); if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }
    tr->fmt = SUBS_FMT_SRT;
    tr->regions = (subs_region *)calloc(1, sizeof *tr->regions); /* SRT never has any */

    long li = 0;
    while (li < nlines) {
        while (li < nlines && lines[li].n == 0) li++;
        if (li >= nlines) break;

        long block_start = li;
        const char *id_p = ""; long id_n = 0;
        int have_id = 1;
        for (long k = 0; k < lines[li].n; k++) if (!is_digit((unsigned char)lines[li].p[k])) { have_id = 0; break; }
        if (lines[li].n > 0 && have_id) { id_p = lines[li].p; id_n = lines[li].n; li++; }

        int64_t start, end;
        /* No `&& end >= start`: consistent with subs_parse_vtt, see the
         * comment there. */
        int timing_ok = li < nlines && lines[li].n > 0 && srt_timing_line(lines[li].p, lines[li].n, &start, &end);
        if (!timing_ok) {
            note_malformed(tr);
#ifdef SUBS_STRICT
            free(lines); free(norm); subs_close(tr); if (out_err) *out_err = SUBS_ERR_STRICT; return NULL;
#else
            li = block_start + 1;
            while (li < nlines && lines[li].n != 0) li++;
            continue;
#endif
        }
        li++; /* consume timing line */

        char *text = NULL; long text_len = 0, text_cap = 0;
        while (li < nlines && lines[li].n != 0) {
            long add = lines[li].n;
            long need = text_len + add + 1;
            if (need > text_cap) {
                long ncap = text_cap ? text_cap * 2 : 256;
                while (ncap < need) ncap *= 2;
                char *nb = (char *)realloc(text, (size_t)ncap);
                if (!nb) { free(text); free(lines); free(norm); subs_close(tr); if (out_err) *out_err = SUBS_ERR_OOM; return NULL; }
                text = nb; text_cap = ncap;
            }
            if (text_len > 0) text[text_len++] = '\n';
            memcpy(text + text_len, lines[li].p, (size_t)add);
            text_len += add;
            li++;
        }

        cue_slot *slot = push_cue(tr);
        if (!slot) { free(text); free(lines); free(norm); subs_close(tr); if (out_err) *out_err = SUBS_ERR_RANGE; return NULL; }
        slot->cue.id = dupn(id_p, id_n, SUBS_MAX_ID_LEN);
        slot->cue.text = dupn(text ? text : "", text_len, SUBS_MAX_TEXT_LEN);
        slot->cue.start_ms = start; slot->cue.end_ms = end;
        settings_defaults(&slot->cue.settings);
        free(text);
    }

    free(lines);
    free(norm);
    finish_track(tr);
    return tr;
}

/* ============================================================ dispatch == */
subs_track *subs_parse(const uint8_t *data, long len, subs_format *out_fmt, int *out_err)
{
    const uint8_t *p = data; long n = len;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    int is_vtt = n >= 6 && memcmp(p, "WEBVTT", 6) == 0 &&
                 (n == 6 || p[6] == ' ' || p[6] == '\t' || p[6] == '\n');
    subs_format fmt = is_vtt ? SUBS_FMT_VTT : SUBS_FMT_SRT;
    if (out_fmt) *out_fmt = fmt;
    return is_vtt ? subs_parse_vtt(data, len, out_err) : subs_parse_srt(data, len, out_err);
}

void subs_close(subs_track *tr)
{
    if (!tr) return;
    for (int i = 0; i < tr->ncues; i++) {
        free((void *)tr->slots[i].cue.id);
        free((void *)tr->slots[i].cue.text);
    }
    free(tr->slots);
    free(tr->regions);
    free(tr->pmax);
    free(tr);
}

subs_format subs_track_format(const subs_track *tr) { return tr ? tr->fmt : SUBS_FMT_UNKNOWN; }
int subs_cue_count(const subs_track *tr) { return tr ? tr->ncues : 0; }
const subs_cue *subs_cue_at(const subs_track *tr, int index)
{
    if (!tr || index < 0 || index >= tr->ncues) return NULL;
    return &tr->slots[index].cue;
}
int subs_region_count(const subs_track *tr) { return tr ? tr->nregions : 0; }
const subs_region *subs_region_at(const subs_track *tr, int index)
{
    if (!tr || index < 0 || index >= tr->nregions) return NULL;
    return &tr->regions[index];
}
int subs_skipped_count(const subs_track *tr) { return tr ? tr->skipped : 0; }

/* ---------------------------------------------------------- active lookup */
/* Stabbing query over [start_ms,end_ms) intervals, sorted by start_ms with a
 * parallel PREFIX MAX of end_ms (pmax[i] = max(end_ms[0..i])). Two steps:
 *
 *   1. bsearch the largest index `hi` with start_ms[hi] <= t.        O(log n)
 *   2. bsearch, WITHIN [0,hi], the smallest index `lo` with
 *      pmax[lo] >= t. Because pmax is non-decreasing, every index < lo has
 *      pmax < t, i.e. EVERY cue up to lo-1 has already ended -- lo is a
 *      correct (not merely heuristic) lower bound, not a cutoff that could
 *      skip a still-active cue.                                      O(log n)
 *   3. scan [lo,hi] and keep the cues whose OWN end_ms > t.        O(hi-lo+1)
 *
 * Step 3 is genuinely linear in the window, not amortised log n -- BE
 * HONEST ABOUT THAT rather than call the whole thing O(log n), which is
 * what the docstring in subs.h promises and what this comment is here to
 * qualify. The window is bounded by the number of cues whose span reaches
 * back far enough to still cover t, which for ordinary subtitles (a handful
 * of cues overlapping at once, at most) is a small constant; it is NOT
 * bounded in the worst case by n -- a single pathologically long-duration
 * cue sitting under many short ones widens the window to include every cue
 * after it up to hi. That is a correct answer computed slowly, never a
 * wrong one: unlike a naive bsearch-on-start-alone (which would miss any
 * cue whose start is before the query point but whose block isn't the
 * single nearest one), this always returns exactly the right set. */
int subs_active_at(const subs_track *tr, int64_t t_ms, int *out_idx, int max_out)
{
    if (!tr || tr->ncues == 0) return 0;
    long n = tr->ncues;

    /* hi = last index with start_ms <= t, or -1 */
    long lo_b = 0, hi_b = n - 1, hi = -1;
    while (lo_b <= hi_b) {
        long mid = lo_b + (hi_b - lo_b) / 2;
        if (tr->slots[mid].cue.start_ms <= t_ms) { hi = mid; lo_b = mid + 1; }
        else hi_b = mid - 1;
    }
    if (hi < 0) return 0;

    /* lo = smallest index in [0,hi] with pmax[index] >= t (pmax is
     * non-decreasing over the WHOLE array, so this bsearch over [0,hi] is
     * valid regardless of what lies beyond hi). */
    long a = 0, b = hi, lo = hi + 1;
    while (a <= b) {
        long mid = a + (b - a) / 2;
        if (tr->pmax && tr->pmax[mid] >= t_ms) { lo = mid; b = mid - 1; }
        else a = mid + 1;
    }
    if (lo > hi) return 0;

    int count = 0;
    for (long i = lo; i <= hi; i++) {
        const subs_cue *c = &tr->slots[i].cue;
        if (c->start_ms <= t_ms && t_ms < c->end_ms) {
            if (count < max_out) out_idx[count] = (int)i;
            count++;
        }
    }
    return count;
}
