/* ch_sse.c -- SSE record framing + a minimal JSON string reader.
 *
 * Read ch_sse.h first: it states what this refuses and why the split-across-
 * chunk-boundaries case is the one that shapes the whole design.
 *
 * The rule the file is written against: NOTHING is parsed until it is whole.
 * A byte joins a line; a line joins a record; a record is parsed. There is no
 * function here that takes "the bytes that just arrived" and looks for a token
 * in them, because that is exactly the reader that works against a recorded
 * file and fails against a server.
 *
 * Freestanding: no libc, no allocation, no float. The only headers are the
 * compiler's own <stdint.h>-free primitives (there are none needed), so this
 * object file is identical whether it is linked into ch.aex or into a host
 * test binary.
 */

#include "ch_sse.h"

/* ---- the three character helpers this file needs ---------------------- */

static int is_ws(int c)    { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int eqn(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

const char *sse_strerror(int code)
{
    switch (code) {
    case SSE_OK:       return "ok";
    case SSE_E_LINE:   return "an event line was longer than the 4 KiB bound";
    case SSE_E_RECORD: return "an event record was larger than the 16 KiB bound";
    case SSE_E_JSON:   return "a data: payload was not a readable JSON object";
    case SSE_E_TEXT:   return "a single delta was larger than the 8 KiB bound";
    case SSE_E_TRUNC:  return "the stream ended before data: [DONE]";
    }
    return "unknown";
}

/* ======================================================================
 * The JSON reader.
 *
 * Three operations, and the first is the one that makes the other two safe:
 * jskip() advances past ONE complete value, whatever it is. Everything else is
 * built on it, which is why a `{` inside a string, or the word "content"
 * appearing inside somebody's message, cannot fool a lookup. A scanner that
 * searched for the literal "\"content\":" would be twenty lines shorter and
 * would mis-read the first reply that quoted a JSON snippet back at us.
 * ====================================================================== */

static const char *jws(const char *p, const char *e)
{
    while (p < e && is_ws(*p)) p++;
    return p;
}

/* Past a string literal, p pointing AT the opening quote. NULL if unterminated. */
static const char *jstr_end(const char *p, const char *e)
{
    if (p >= e || *p != '"') return 0;
    p++;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }   /* \" and \\ both handled here */
        if (*p == '"') return p + 1;
        p++;
    }
    return 0;
}

static const char *jskip(const char *p, const char *e, int depth);

/* Past an object or array body. `close` is '}' or ']'. p points past the
 * opening bracket. */
static const char *jseq(const char *p, const char *e, char close, int depth)
{
    p = jws(p, e);
    if (p < e && *p == close) return p + 1;
    for (;;) {
        if (close == '}') {                      /* "key" : value */
            p = jws(p, e);
            p = jstr_end(p, e);
            if (!p) return 0;
            p = jws(p, e);
            if (p >= e || *p != ':') return 0;
            p++;
        }
        p = jskip(p, e, depth + 1);
        if (!p) return 0;
        p = jws(p, e);
        if (p >= e) return 0;
        if (*p == ',') { p++; continue; }
        if (*p == close) return p + 1;
        return 0;
    }
}

static const char *jskip(const char *p, const char *e, int depth)
{
    if (depth > SSE_JSON_DEPTH) return 0;
    p = jws(p, e);
    if (p >= e) return 0;
    if (*p == '"') return jstr_end(p, e);
    if (*p == '{') return jseq(p + 1, e, '}', depth);
    if (*p == '[') return jseq(p + 1, e, ']', depth);
    /* A number, or true/false/null. Consume the run of characters any of them
     * can be made of; the terminator is whatever the caller expects next, and
     * the caller checks it. Deliberately permissive: this scanner's job is to
     * find one string, not to validate the whole document. */
    const char *s = p;
    while (p < e && !is_ws(*p) && *p != ',' && *p != '}' && *p != ']') p++;
    return p > s ? p : 0;
}

/* Value for `key` in the object at p ('{'), or NULL. */
static const char *jobj_get(const char *p, const char *e, const char *key, int klen)
{
    if (p >= e || *p != '{') return 0;
    p = jws(p + 1, e);
    if (p < e && *p == '}') return 0;
    for (;;) {
        p = jws(p, e);
        const char *ks = p;
        const char *ke = jstr_end(p, e);
        if (!ke) return 0;
        /* Compare the RAW key bytes. Escapes in a key would not match, which is
         * correct for the four ASCII keys this program looks up and is stated
         * rather than silently assumed. */
        int rawlen = (int)(ke - ks) - 2;
        int hit = (rawlen == klen) && eqn(ks + 1, key, klen);
        p = jws(ke, e);
        if (p >= e || *p != ':') return 0;
        p++;
        p = jws(p, e);
        if (hit) return p;
        p = jskip(p, e, 1);
        if (!p) return 0;
        p = jws(p, e);
        if (p >= e) return 0;
        if (*p == ',') { p++; continue; }
        return 0;                                  /* '}' -- key not present */
    }
}

/* Element `idx` of the array at p ('['), or NULL. */
static const char *jarr_get(const char *p, const char *e, int idx)
{
    if (p >= e || *p != '[') return 0;
    p = jws(p + 1, e);
    if (p < e && *p == ']') return 0;
    for (int i = 0;; i++) {
        p = jws(p, e);
        if (i == idx) return p;
        p = jskip(p, e, 1);
        if (!p) return 0;
        p = jws(p, e);
        if (p >= e) return 0;
        if (*p == ',') { p++; continue; }
        return 0;
    }
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Encode one code point as UTF-8. Returns bytes written, 0 if it did not fit. */
static int utf8(char *o, int room, unsigned cp)
{
    if (cp < 0x80)    { if (room < 1) return 0; o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { if (room < 2) return 0;
        o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { if (room < 3) return 0;
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    if (room < 4) return 0;
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode the JSON string at p ('"') into out. Returns the length, SSE_E_JSON if
 * it is not a well-formed string, SSE_E_TEXT if it does not fit. */
static int jstr_decode(const char *p, const char *e, char *out, int outmax)
{
    if (p >= e || *p != '"') return SSE_E_JSON;
    p++;
    int o = 0;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') { if (o >= outmax) return SSE_E_TEXT; out[o] = 0; return o; }
        if (c != '\\') {
            if (o >= outmax) return SSE_E_TEXT;
            out[o++] = (char)c; p++; continue;
        }
        p++;
        if (p >= e) return SSE_E_JSON;
        int esc = *p++;
        unsigned cp;
        switch (esc) {
        case '"':  cp = '"';  break;
        case '\\': cp = '\\'; break;
        case '/':  cp = '/';  break;
        case 'b':  cp = 8;    break;
        case 'f':  cp = 12;   break;
        case 'n':  cp = 10;   break;
        case 'r':  cp = 13;   break;
        case 't':  cp = 9;    break;
        case 'u': {
            if (e - p < 4) return SSE_E_JSON;
            int h0 = hexv(p[0]), h1 = hexv(p[1]), h2 = hexv(p[2]), h3 = hexv(p[3]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return SSE_E_JSON;
            cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            p += 4;
            /* A surrogate PAIR is one code point spelled as two escapes. A lone
             * surrogate is not a code point at all; it becomes U+FFFD rather
             * than being dropped (which would silently shorten the reply) or
             * encoded as-is (which would be invalid UTF-8 in the window). */
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    int g0 = hexv(p[2]), g1 = hexv(p[3]), g2 = hexv(p[4]), g3 = hexv(p[5]);
                    if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
                        unsigned lo = (unsigned)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            p += 6;
                        } else cp = 0xFFFD;
                    } else cp = 0xFFFD;
                } else cp = 0xFFFD;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            break;
        }
        default: return SSE_E_JSON;              /* not an escape JSON defines */
        }
        int n = utf8(out + o, outmax - o, cp);
        if (!n) return SSE_E_TEXT;
        o += n;
    }
    return SSE_E_JSON;                            /* unterminated */
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Walk `path` from the document root and return the value's start, or NULL. */
static const char *jlookup(const char *js, int len,
                           const char *const *path, int npath)
{
    const char *p = jws(js, js + len), *e = js + len;
    for (int i = 0; i < npath; i++) {
        if (p >= e) return 0;
        if (*p == '[') {
            /* Array: the component must be a decimal index. A non-numeric
             * component here is a caller bug, and answering NULL rather than
             * guessing is what keeps that bug visible. */
            int idx = 0, k = 0;
            for (const char *c = path[i]; *c; c++, k++) {
                if (!is_digit((unsigned char)*c)) return 0;
                idx = idx * 10 + (*c - '0');
                if (idx > 100000) return 0;
            }
            if (!k) return 0;
            p = jarr_get(p, e, idx);
        } else if (*p == '{') {
            p = jobj_get(p, e, path[i], slen(path[i]));
        } else {
            return 0;                              /* a scalar has no members */
        }
        if (!p) return 0;
        p = jws(p, e);
    }
    return p;
}

int sse_json_string(const char *js, int len, const char *const *path, int npath,
                    char *out, int outmax)
{
    const char *v = jlookup(js, len, path, npath);
    if (!v) return SSE_E_JSON;
    return jstr_decode(v, js + len, out, outmax);
}

/* ======================================================================
 * SSE framing.
 * ====================================================================== */

static void refuse(struct sse *s, int code, const char *why)
{
    s->last_err = code;
    s->n_refused++;
    if (s->on_refuse) s->on_refuse(s->ctx, code, why);
}

void sse_init(struct sse *s, sse_on_delta on_delta, sse_on_refuse on_refuse, void *ctx)
{
    char *p = (char *)s;
    for (unsigned i = 0; i < sizeof *s; i++) p[i] = 0;
    s->on_delta  = on_delta;
    s->on_refuse = on_refuse;
    s->ctx       = ctx;
    s->bom       = 1;          /* a leading UTF-8 BOM, if present, is skipped */
}

int sse_done(const struct sse *s) { return s->done; }

/* One complete record. `s->data` holds the concatenated data: lines. */
static void dispatch(struct sse *s)
{
    if (!s->have_data) {           /* a blank line with no data is not a record */
        s->poisoned = 0;
        return;
    }
    s->n_records++;

    /* The spec appends '\n' after every data: line; the last one is stripped. */
    if (s->data_len > 0 && s->data[s->data_len - 1] == '\n') s->data_len--;

    if (s->poisoned) {
        refuse(s, s->poison_code ? s->poison_code : SSE_E_RECORD,
               "a record was dropped because one of its lines was lost");
        goto reset;
    }
    if (s->data_len == 6 && eqn(s->data, "[DONE]", 6)) {
        s->done = 1;
        goto reset;
    }

    {
        const char *e = s->data + s->data_len;
        /* Distinguish "the stream said something we do not model" from "the
         * stream is broken", because conflating them makes one of the two
         * useless. An OpenAI-compatible frame is a JSON OBJECT; a payload that
         * is not one at all (an HTML error page dumped into the body, a proxy's
         * plain-text notice) is a refusal and is reported. A well-formed object
         * with no content member -- the role-only opening frame, the
         * finish_reason closing frame -- is NOT: refusing those would put a
         * complaint in the window on every correct stream, which trains the
         * user to ignore the one that matters. They are still counted:
         * n_records minus n_deltas is exactly how many arrived. */
        const char *p0 = jws(s->data, e);
        if (p0 >= e || *p0 != '{') {
            refuse(s, SSE_E_JSON, "a data: payload was not a JSON object");
            goto reset;
        }
        if (!jskip(s->data, e, 0)) {
            refuse(s, SSE_E_JSON, "a data: payload did not parse as JSON");
            goto reset;
        }
        static const char *const PATH[4] = { "choices", "0", "delta", "content" };
        const char *v = jlookup(s->data, s->data_len, PATH, 4);
        if (!v) goto reset;
        if (*v == 'n') goto reset;                 /* content: null -- no delta */
        int n = jstr_decode(v, s->data + s->data_len, s->text, SSE_TEXT_MAX);
        if (n < 0) {
            refuse(s, n, n == SSE_E_TEXT
                   ? "a single delta was too large for the decode buffer"
                   : "choices[0].delta.content was not a JSON string");
            goto reset;
        }
        s->n_deltas++;
        if (n > 0 && s->on_delta) s->on_delta(s->ctx, s->text, n);
    }

reset:
    s->data_len = 0;
    s->have_data = 0;
    s->poisoned = 0;
    s->poison_code = 0;
}

/* One complete line, terminator already removed. */
static void take_line(struct sse *s, const char *l, int n)
{
    /* A UTF-8 BOM is a property of the STREAM, so it can only be the first
     * three bytes of the first line. Stripping it here rather than in the byte
     * loop means a lone 0xEF that merely LOOKS like the start of one is never
     * eaten: the test is on all three bytes at once or not at all. */
    if (s->bom) {
        s->bom = 0;
        if (n >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB
                   && (unsigned char)l[2] == 0xBF) { l += 3; n -= 3; }
    }
    if (n == 0) { dispatch(s); return; }
    if (l[0] == ':') { s->n_comments++; return; }   /* keep-alive comment */

    int c = 0;
    while (c < n && l[c] != ':') c++;
    const char *val = l + c;
    int vlen = 0;
    if (c < n) {                                    /* there was a colon */
        val = l + c + 1;
        vlen = n - c - 1;
        if (vlen > 0 && val[0] == ' ') { val++; vlen--; }   /* exactly one space */
    }
    if (!(c == 4 && eqn(l, "data", 4))) { s->n_fields++; return; }

    if (s->data_len + vlen + 1 > SSE_DATA_MAX) {
        if (!s->poisoned) {
            refuse(s, SSE_E_RECORD, "a record's data: payload exceeded 16 KiB");
            s->poisoned = 1;
            s->poison_code = SSE_E_RECORD;
        }
        s->have_data = 1;
        return;
    }
    for (int i = 0; i < vlen; i++) s->data[s->data_len++] = val[i];
    s->data[s->data_len++] = '\n';
    s->have_data = 1;
}

int sse_feed(struct sse *s, const void *buf, int len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned refused0 = s->n_refused;

#ifdef SSE_NAIVE_CHUNK
    /* ---- THE NEGATIVE CONTROL, and it is the whole reason the host gate
     * exists. This is the reader everybody writes first: it assumes the buffer
     * it was just handed begins on a line boundary, so it throws away whatever
     * partial line was in flight. Against a recorded stream replayed in one
     * lump it is indistinguishable from the real thing and every assertion
     * about the final text passes. Against a real server -- or against the
     * byte-at-a-time and split-sweep cases in ch_sse_test.c -- it loses a piece
     * of every delta that straddles a read. `make test-ch-negctl` requires the
     * host gate to FAIL when this is defined. */
    s->line_len  = 0;
    s->line_over = 0;
    s->skip_lf   = 0;
#endif

    for (int i = 0; i < len; i++) {
        unsigned char c = p[i];
        s->n_bytes++;

        if (s->skip_lf) {
            s->skip_lf = 0;
            if (c == '\n') continue;               /* the LF of a CRLF */
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r') s->skip_lf = 1;
            if (s->line_over) {
                refuse(s, SSE_E_LINE, "an event line exceeded 4 KiB and was dropped");
                s->poisoned = 1;                   /* the record is now a lie */
                s->poison_code = SSE_E_LINE;
                s->line_over = 0;
                s->line_len = 0;
                continue;
            }
            take_line(s, s->line, s->line_len);
            s->line_len = 0;
            continue;
        }
        if (s->line_over) continue;                /* discard to the terminator */
        if (s->line_len >= SSE_LINE_MAX) { s->line_over = 1; continue; }
        s->line[s->line_len++] = (char)c;
    }

    /* The return value is about THIS call: SSE_OK unless something was refused
     * while these bytes were being consumed. s->last_err keeps the most recent
     * refusal for the whole stream, which is what the window shows. */
    return s->n_refused == refused0 ? SSE_OK : s->last_err;
}

int sse_eof(struct sse *s)
{
    /* A server that closes without a final blank line still sent a record; the
     * close IS the terminator. Feeding one keeps that path in one place. */
    if (s->line_len || s->line_over) sse_feed(s, "\n", 1);
    if (s->have_data) dispatch(s);
    if (!s->done) {
        s->last_err = SSE_E_TRUNC;
        return SSE_E_TRUNC;
    }
    return SSE_OK;
}
