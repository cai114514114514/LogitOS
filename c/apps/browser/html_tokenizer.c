/* html_tokenizer.c -- the WHATWG HTML tokenization stage.
 *
 * Essentially every state in the spec is here, including the ones that look
 * like padding.  That is deliberate: the tokenizer is mechanical and fully
 * testable (html5lib-tests has 6810 cases for it), so a shortcut here buys a
 * few hundred lines and costs a class of bug that only shows up on somebody
 * else's page.  Savings belong in tree construction, where the spec's
 * complexity is genuinely about rare markup.
 *
 * Two families of states are worth explaining, because they are the ones a
 * hand-written tokenizer is most tempted to skip:
 *
 *  - The script-data ESCAPED / DOUBLE-ESCAPED states (11 of them).  Inside
 *    <script>, "<!--" opens a comment-like region in which "</script>" does
 *    NOT close the element; and inside that, "<script" opens a doubly-escaped
 *    region in which "</script>" does not close it either.  This is not
 *    hypothetical markup: ad tags, JSON-LD blobs and inline templates are full
 *    of  "<\/script>", "</scr"+"ipt>"  and  document.write("<script ...").
 *    A tokenizer that just scans for the next "</script>" ends the element in
 *    the middle of a string literal and swallows the entire rest of the page.
 *
 *  - The comment "<!--" ... "--!>" states.  --!> is not valid, but it is what
 *    a long tail of CMS templates emit, and browsers close the comment there.
 *    Treating it as ordinary comment text swallows the document from that
 *    point on -- the same failure mode, from a typo.
 *
 * The three streaming rules the whole file is written to are documented in
 * html_tokenizer.h; the places that implement them are marked "rule N".
 */

#include <stdlib.h>
#include <string.h>

#include "html_tokenizer.h"

#define HTML_TAGS_TABLE
#include "html_tags.inc"        /* second include: pulls in the lookup table */
#include "html_entities.inc"

/* ---------------------------------------------------------- primitives -- */

#define R_NEED  (-2)            /* run(): out of input, eof == 0 */
#define C_NEED  (-2)            /* nextc(): ditto */
#define C_EOF   (-1)

static int is_ws(int c)     { return c == '\t' || c == '\n' || c == '\f' || c == ' '; }
static int is_upper(int c)  { return c >= 'A' && c <= 'Z'; }
static int is_lower(int c)  { return c >= 'a' && c <= 'z'; }
static int is_alpha(int c)  { return is_upper(c) || is_lower(c); }
static int is_digit(int c)  { return c >= '0' && c <= '9'; }
static int is_alnum(int c)  { return is_alpha(c) || is_digit(c); }
static int lc(int c)        { return is_upper(c) ? c + 32 : c; }

/* -------------------------------------------------------------- charbuf -- */
/* Growable byte buffer.  Always NUL-terminated so the contents can be printed
 * straight out of a debugger; the NUL is never counted in ->len. */

static void cb_reset(struct html_charbuf *b) { b->len = 0; if (b->p) b->p[0] = 0; }

static void cb_grow(struct html_charbuf *b, uint32_t need)
{
    uint32_t c = b->cap ? b->cap : 32;
    while (c < need + 1) c *= 2;
    char *n = (char *)realloc(b->p, c);
    if (!n) return;                     /* OOM: silently truncate, never crash */
    b->p = n; b->cap = c;
}

static void cb_put(struct html_charbuf *b, const char *d, uint32_t n)
{
    if (!n) return;
    if (b->len + n + 1 > b->cap) cb_grow(b, b->len + n);
    if (b->len + n + 1 > b->cap) return;
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void cb_putc(struct html_charbuf *b, int c) { char ch = (char)c; cb_put(b, &ch, 1); }
static const char *cb_ptr(const struct html_charbuf *b) { return b->p ? b->p : ""; }

/* UTF-8 encode.  Character references are the only place we synthesise code
 * points; raw input bytes are passed through untouched, so a document in any
 * ASCII-superset encoding survives even though we only speak UTF-8. */
static void cb_putcp(struct html_charbuf *b, uint32_t cp)
{
    char t[4];
    if (cp < 0x80)          { cb_putc(b, (int)cp); return; }
    if (cp < 0x800)         { t[0] = (char)(0xC0 | (cp >> 6));
                              t[1] = (char)(0x80 | (cp & 0x3F));  cb_put(b, t, 2); return; }
    if (cp < 0x10000)       { t[0] = (char)(0xE0 | (cp >> 12));
                              t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                              t[2] = (char)(0x80 | (cp & 0x3F));  cb_put(b, t, 3); return; }
    t[0] = (char)(0xF0 | (cp >> 18));
    t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    t[3] = (char)(0x80 | (cp & 0x3F));
    cb_put(b, t, 4);
}

#define REPLACEMENT 0xFFFDu

/* ------------------------------------------------------------- tag ids --- */

uint16_t html_tag_id(const char *name, uint32_t len)
{
    int lo = 0, hi = HTML_TAG_COUNT;
    if (len > HTML_TAG_MAXLEN) return HTAG_UNKNOWN;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        const struct html_tag_ent *e = &html_tag_ents[m];
        uint32_t n = len < e->len ? len : e->len;
        int c = n ? memcmp(name, e->name, n) : 0;
        if (c == 0) c = (int)len - (int)e->len;
        if (c == 0) return e->id;
        if (c < 0) hi = m; else lo = m + 1;
    }
    return HTAG_UNKNOWN;
}

/* --------------------------------------------- named reference lookup ---- */
/* Longest-prefix match over the name-sorted table.  Narrow [lo,hi) one byte at
 * a time; because a name that is a prefix of another sorts first, an exact
 * match of length k+1 is always the low end of the narrowed range, so spotting
 * it is a single length compare.  O(maxlen * log n) with no allocation, which
 * is what lets "&notin" find "not" and "&notin;" find "notin;".
 *
 * Returns the table index of the longest match, or -1. */
static int html_ref_lookup(const char *w, uint32_t wl)
{
    int lo = 0, hi = HTML_ENT_COUNT, best = -1;
    for (uint32_t k = 0; k < wl && lo < hi; k++) {
        unsigned char ch = (unsigned char)w[k];
        int a, b, m, nlo, nhi;

        /* entries in [lo,hi) all share the prefix w[0..k) */
        a = lo; b = hi;                                   /* first with name[k] >= ch */
        while (a < b) {
            m = (a + b) / 2;
            const struct html_ent *e = &html_ents[m];
            int c = (e->len <= k) ? -1 : ((unsigned char)e->name[k] < ch ? -1
                                        : ((unsigned char)e->name[k] > ch ? 1 : 0));
            if (c < 0) a = m + 1; else b = m;
        }
        nlo = a;
        b = hi;                                           /* first with name[k] > ch */
        while (a < b) {
            m = (a + b) / 2;
            const struct html_ent *e = &html_ents[m];
            int c = (e->len <= k) ? -1 : ((unsigned char)e->name[k] < ch ? -1
                                        : ((unsigned char)e->name[k] > ch ? 1 : 0));
            if (c <= 0) a = m + 1; else b = m;
        }
        nhi = a;

        lo = nlo; hi = nhi;
        if (lo < hi && html_ents[lo].len == k + 1) best = lo;
    }
    return best;
}

/* --------------------------------------------------------- input reader -- */
/* Newline preprocessing lives here: the spec normalises CRLF and lone CR to LF
 * before tokenization, and doing it in the reader means no state has to know
 * about CR at all.  A '\r' at the end of a non-final buffer is the one case
 * that needs more input to resolve (CR vs CRLF), and it takes the rule-3 path
 * like everything else. */
static int nextc(struct html_tokenizer *t)
{
    if (t->pos >= t->len) return t->eof ? C_EOF : C_NEED;
    unsigned char c = (unsigned char)t->buf[t->pos++];
    if (c == '\r') {
        if (t->pos >= t->len) {
            if (!t->eof) { t->pos--; return C_NEED; }
        } else if (t->buf[t->pos] == '\n') {
            t->pos++;
        }
        return '\n';
    }
    return c;
}

/* Raw (un-normalised) lookahead for the fixed keyword matches: "--",
 * "DOCTYPE", "[CDATA[", "PUBLIC", "SYSTEM".  None of them can contain CR, so
 * comparing raw bytes is equivalent to comparing the preprocessed stream.
 * Returns 1 match, 0 no match, R_NEED if the answer needs more input. */
static int lookahead(struct html_tokenizer *t, size_t at, const char *s, uint32_t n, int ci)
{
    if (at + n > t->len) {
        if (!t->eof) return R_NEED;
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        int a = (unsigned char)t->buf[at + i], b = (unsigned char)s[i];
        if (ci) { a = lc(a); b = lc(b); }
        if (a != b) return 0;
    }
    return 1;
}

/* ------------------------------------------------------- token building -- */

#define CUR_NONE 0xFF

static void start_token(struct html_tokenizer *t, int type, size_t src)
{
    t->cur_type = (uint8_t)type;
    t->tok_src = (uint32_t)src;
    cb_reset(&t->name); cb_reset(&t->comment);
    cb_reset(&t->pubid); cb_reset(&t->sysid); cb_reset(&t->attrbuf);
    t->nattr = 0;
    t->self_closing = t->force_quirks = 0;
    t->has_name = t->has_pubid = t->has_sysid = 0;
}

static void new_attr(struct html_tokenizer *t)
{
    if (t->nattr == t->attrcap) {
        uint32_t c = t->attrcap ? t->attrcap * 2 : 8;
        struct html_pattr *n = (struct html_pattr *)realloc(t->attrs, c * sizeof *n);
        if (!n) return;
        t->attrs = n; t->attrcap = c;
    }
    if (t->nattr == t->attrcap) return;
    struct html_pattr *a = &t->attrs[t->nattr++];
    a->noff = a->voff = t->attrbuf.len;
    a->nlen = a->vlen = 0;
    a->dropped = 0;
}

static void attr_name_put(struct html_tokenizer *t, const char *d, uint32_t n)
{
    if (!t->nattr) return;
    uint32_t before = t->attrbuf.len;
    cb_put(&t->attrbuf, d, n);
    t->attrs[t->nattr - 1].nlen += t->attrbuf.len - before;
}

static void attr_val_put(struct html_tokenizer *t, const char *d, uint32_t n)
{
    if (!t->nattr) return;
    uint32_t before = t->attrbuf.len;
    cb_put(&t->attrbuf, d, n);
    t->attrs[t->nattr - 1].vlen += t->attrbuf.len - before;
}

static void attr_val_putcp(struct html_tokenizer *t, uint32_t cp)
{
    if (!t->nattr) return;
    uint32_t before = t->attrbuf.len;
    cb_putcp(&t->attrbuf, cp);
    t->attrs[t->nattr - 1].vlen += t->attrbuf.len - before;
}

/* Called whenever the machine leaves the attribute-name state.  The spec drops
 * the LATER of two attributes with the same name; doing it here means the tree
 * builder never has to think about duplicates, and it is the only point at
 * which the name is known to be complete. */
static void finish_attr_name(struct html_tokenizer *t)
{
    if (!t->nattr) return;
    struct html_pattr *a = &t->attrs[t->nattr - 1];
    for (uint32_t i = 0; i + 1 < t->nattr; i++) {
        struct html_pattr *o = &t->attrs[i];
        if (o->dropped || o->nlen != a->nlen) continue;
        if (!memcmp(t->attrbuf.p + o->noff, t->attrbuf.p + a->noff, a->nlen)) {
            a->dropped = 1;
            break;
        }
    }
    a->voff = t->attrbuf.len;           /* the value starts after the name */
    a->vlen = 0;
}

static void build_current(struct html_tokenizer *t, struct html_token *tok)
{
    memset(tok, 0, sizeof *tok);
    tok->type = t->cur_type;
    tok->src_start = t->tok_src;
    tok->src_end = (uint32_t)t->pos;

    switch (t->cur_type) {
    case TOK_START:
    case TOK_END: {
        tok->name = cb_ptr(&t->name);
        tok->namelen = t->name.len;
        tok->tag = html_tag_id(tok->name, tok->namelen);
        tok->self_closing = t->self_closing;

        uint32_t keep = 0;
        for (uint32_t i = 0; i < t->nattr; i++) if (!t->attrs[i].dropped) keep++;
        if (keep > t->oattrcap) {
            struct html_attr *n = (struct html_attr *)realloc(t->oattrs, keep * sizeof *n);
            if (n) { t->oattrs = n; t->oattrcap = keep; } else keep = 0;
        }
        uint32_t k = 0;
        for (uint32_t i = 0; i < t->nattr && k < keep; i++) {
            struct html_pattr *a = &t->attrs[i];
            if (a->dropped) continue;
            t->oattrs[k].n  = t->attrbuf.p + a->noff;
            t->oattrs[k].nl = a->nlen;
            t->oattrs[k].v  = t->attrbuf.p + a->voff;
            t->oattrs[k].vl = a->vlen;
            k++;
        }
        tok->attrs = t->oattrs;
        tok->nattr = (uint16_t)k;
        break;
    }
    case TOK_COMMENT:
        tok->data = cb_ptr(&t->comment);
        tok->datalen = t->comment.len;
        break;
    case TOK_DOCTYPE:
        tok->name = cb_ptr(&t->name);
        tok->namelen = t->name.len;
        tok->pubid = cb_ptr(&t->pubid);
        tok->pubidlen = t->pubid.len;
        tok->sysid = cb_ptr(&t->sysid);
        tok->sysidlen = t->sysid.len;
        tok->force_quirks = t->force_quirks;
        tok->has_name = t->has_name;
        tok->has_pubid = t->has_pubid;
        tok->has_sysid = t->has_sysid;
        break;
    default:
        break;
    }
}

/* Emit the token under construction.  If characters have accumulated since the
 * last token they were produced FIRST in document order, so they go out now
 * and the real token is held in ->pending for the next call.  The two live in
 * different buffers (chars vs name/comment/attrbuf), so holding one while
 * returning the other cannot alias. */
static int deliver(struct html_tokenizer *t, struct html_token *out)
{
    if (t->chars.len) {
        build_current(t, &t->pending);
        t->has_pending = 1;
        memset(out, 0, sizeof *out);
        out->type = TOK_CHARS;
        out->data = cb_ptr(&t->chars);
        out->datalen = t->chars.len;
        out->src_start = t->chars_src;
        out->src_end = (uint32_t)t->pos;
    } else {
        build_current(t, out);
    }
    t->cur_type = CUR_NONE;
    return 1;
}

static int emit_eof(struct html_tokenizer *t, struct html_token *out)
{
    t->cur_type = TOK_EOF;
    t->tok_src = (uint32_t)t->pos;
    t->finished = 1;
    return deliver(t, out);
}

/* Emit a token and arrange for the next call to produce TOK_EOF.  Used by the
 * "eof-in-comment"/"eof-in-doctype" paths, which produce two tokens. */
enum { ST_EOF_ONLY = HTML_STATE__PUBLIC };

/* ------------------------------------------------------------- states --- */

enum {
    ST_TAG_OPEN = ST_EOF_ONLY + 1,
    ST_END_TAG_OPEN, ST_TAG_NAME,
    ST_RCDATA_LT, ST_RCDATA_END_OPEN, ST_RCDATA_END_NAME,
    ST_RAWTEXT_LT, ST_RAWTEXT_END_OPEN, ST_RAWTEXT_END_NAME,
    ST_SCRIPT_LT, ST_SCRIPT_END_OPEN, ST_SCRIPT_END_NAME,
    ST_SCRIPT_ESC_START, ST_SCRIPT_ESC_START_DASH,
    ST_SCRIPT_ESCAPED, ST_SCRIPT_ESCAPED_DASH, ST_SCRIPT_ESCAPED_DASH_DASH,
    ST_SCRIPT_ESCAPED_LT, ST_SCRIPT_ESCAPED_END_OPEN, ST_SCRIPT_ESCAPED_END_NAME,
    ST_SCRIPT_DESC_START, ST_SCRIPT_DESCAPED, ST_SCRIPT_DESCAPED_DASH,
    ST_SCRIPT_DESCAPED_DASH_DASH, ST_SCRIPT_DESCAPED_LT, ST_SCRIPT_DESC_END,
    ST_BEFORE_ATTR_NAME, ST_ATTR_NAME, ST_AFTER_ATTR_NAME, ST_BEFORE_ATTR_VALUE,
    ST_ATTR_VALUE_DQ, ST_ATTR_VALUE_SQ, ST_ATTR_VALUE_UQ, ST_AFTER_ATTR_VALUE_Q,
    ST_SELF_CLOSING,
    ST_BOGUS_COMMENT, ST_MARKUP_DECL,
    ST_COMMENT_START, ST_COMMENT_START_DASH, ST_COMMENT,
    ST_COMMENT_LT, ST_COMMENT_LT_BANG, ST_COMMENT_LT_BANG_DASH,
    ST_COMMENT_LT_BANG_DASH_DASH,
    ST_COMMENT_END_DASH, ST_COMMENT_END, ST_COMMENT_END_BANG,
    ST_DOCTYPE, ST_BEFORE_DOCTYPE_NAME, ST_DOCTYPE_NAME, ST_AFTER_DOCTYPE_NAME,
    ST_AFTER_DT_PUB_KW, ST_BEFORE_DT_PUB_ID, ST_DT_PUB_ID_DQ, ST_DT_PUB_ID_SQ,
    ST_AFTER_DT_PUB_ID, ST_BETWEEN_DT_PUB_SYS,
    ST_AFTER_DT_SYS_KW, ST_BEFORE_DT_SYS_ID, ST_DT_SYS_ID_DQ, ST_DT_SYS_ID_SQ,
    ST_AFTER_DT_SYS_ID, ST_BOGUS_DOCTYPE,
    ST_CDATA_BRACKET, ST_CDATA_END
};

/* ------------------------------------------------ character references --- */
/* The spec spells this out as nine states.  They are nine because the spec's
 * notation has no subroutines, not because there are nine independent things
 * happening: every one of them ends by "switch to the return state", and none
 * of them can be entered from anywhere else.  One function taking the return
 * state is exactly equivalent and a third of the code -- and it is testable on
 * its own, which nine switch arms buried in a 1500-line machine are not.
 *
 * The ambiguous-ampersand state collapses entirely: its only observable job is
 * to attach a parse error, and for every input its behaviour ("consume alnum,
 * flush it; reconsume anything else in the return state") is byte-for-byte
 * what the return state does with the same characters.  Since we do not report
 * parse errors, flushing "&" and letting the return state re-read the rest is
 * the same tokenization.
 *
 * Returns 0 (handled, state updated) or R_NEED. */

static int in_attr_value(int st)
{
    return st == ST_ATTR_VALUE_DQ || st == ST_ATTR_VALUE_SQ || st == ST_ATTR_VALUE_UQ;
}

static void ref_put(struct html_tokenizer *t, const char *d, uint32_t n)
{
    if (in_attr_value(t->return_state)) attr_val_put(t, d, n);
    else {
        if (!t->chars.len) t->chars_src = t->cpos;
        cb_put(&t->chars, d, n);
    }
}

static void ref_putcp(struct html_tokenizer *t, uint32_t cp)
{
    if (in_attr_value(t->return_state)) attr_val_putcp(t, cp);
    else {
        if (!t->chars.len) t->chars_src = t->cpos;
        cb_putcp(&t->chars, cp);
    }
}

/* windows-1252 rehabilitation of C1 controls.  &#153; means U+2122 on the real
 * web because that is what a generation of authoring tools emitted; the spec
 * codified it rather than lose the pages. */
static uint32_t num_fixup(uint32_t v)
{
    static const uint16_t c1[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (v == 0 || v > 0x10FFFF) return REPLACEMENT;
    if (v >= 0xD800 && v <= 0xDFFF) return REPLACEMENT;   /* surrogate */
    if (v >= 0x80 && v <= 0x9F) return c1[v - 0x80];
    return v;
}

static int char_ref(struct html_tokenizer *t)
{
    /* t->pos is just past the '&'. */
    size_t after_amp = t->pos;

    if (t->pos >= t->len) {
        if (!t->eof) return R_NEED;
        ref_put(t, "&", 1);
        t->state = t->return_state;
        return 0;
    }

    unsigned char c = (unsigned char)t->buf[t->pos];

    /* ---- numeric ------------------------------------------------------- */
    if (c == '#') {
        size_t p = after_amp + 1;
        int hex = 0;
        char xch = 0;

        if (p >= t->len && !t->eof) return R_NEED;
        if (p < t->len && (t->buf[p] == 'x' || t->buf[p] == 'X')) {
            hex = 1; xch = t->buf[p]; p++;
        }
        size_t ds = p;
        uint32_t v = 0;
        while (p < t->len) {
            int d, ch = (unsigned char)t->buf[p];
            if (is_digit(ch)) d = ch - '0';
            else if (hex && ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (hex && ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else break;
            if (v <= 0x10FFFF) v = v * (hex ? 16u : 10u) + (uint32_t)d;
            if (v > 0x10FFFF) v = 0x110000;      /* clamp: any overflow -> FFFD */
            p++;
        }
        if (p >= t->len && !t->eof) return R_NEED;   /* digits may continue */

        if (p == ds) {                                /* absence-of-digits */
            ref_put(t, "&#", 2);
            if (hex) ref_put(t, &xch, 1);
            t->pos = p;
            t->state = t->return_state;
            return 0;
        }
        if (p < t->len && t->buf[p] == ';') p++;      /* else missing-semicolon */
        t->pos = p;
        ref_putcp(t, num_fixup(v));
        t->state = t->return_state;
        return 0;
    }

    /* ---- named --------------------------------------------------------- */
    if (is_alnum(c)) {
        char w[HTML_ENT_MAXLEN];
        uint32_t wl = 0;
        int term = 0;
        size_t p = after_amp;
        while (wl < HTML_ENT_MAXLEN && p < t->len) {
            unsigned char ch = (unsigned char)t->buf[p];
            if (is_alnum(ch)) { w[wl++] = (char)ch; p++; continue; }
            if (ch == ';') { w[wl++] = ';'; p++; }
            term = 1;
            break;
        }
        if (wl == HTML_ENT_MAXLEN) term = 1;
        if (!term && !t->eof) return R_NEED;

        int best = html_ref_lookup(w, wl);
        if (best >= 0) {
            const struct html_ent *e = &html_ents[best];
            int semi = e->name[e->len - 1] == ';';

            /* The historical no-semicolon forms are NOT expanded inside an
             * attribute value when what follows could make the run look like a
             * longer name or a query string: "?a=1&notafter=2" must keep its
             * literal "&not", or half the links on the web break. */
            if (!semi && in_attr_value(t->return_state)) {
                size_t nx = after_amp + e->len;
                if (nx >= t->len && !t->eof) return R_NEED;
                int n = nx < t->len ? (unsigned char)t->buf[nx] : 0;
                if (n == '=' || is_alnum(n)) {
                    ref_put(t, "&", 1);
                    t->pos = after_amp;          /* return state re-reads the name */
                    t->state = t->return_state;
                    return 0;
                }
            }
            t->pos = after_amp + e->len;
            ref_putcp(t, e->cp1);
            if (e->cp2) ref_putcp(t, e->cp2);
            t->state = t->return_state;
            return 0;
        }
        /* no match -- see the ambiguous-ampersand note above */
        ref_put(t, "&", 1);
        t->pos = after_amp;
        t->state = t->return_state;
        return 0;
    }

    /* ---- not a reference at all ---------------------------------------- */
    ref_put(t, "&", 1);
    t->pos = after_amp;
    t->state = t->return_state;
    return 0;
}

/* --------------------------------------------------------- the machine --- */

static void emit_ch(struct html_tokenizer *t, int c)
{
    if (!t->chars.len) t->chars_src = t->cpos;
    cb_putc(&t->chars, c);
}
static void emit_str(struct html_tokenizer *t, const char *s, uint32_t n)
{
    if (!t->chars.len) t->chars_src = t->cpos;
    cb_put(&t->chars, s, n);
}
static void emit_cp(struct html_tokenizer *t, uint32_t cp)
{
    if (!t->chars.len) t->chars_src = t->cpos;
    cb_putcp(&t->chars, cp);
}

/* An end tag is "appropriate" only when it matches the last start tag we
 * emitted.  Without this, "</b>" inside <title> would close the title. */
static int appropriate(struct html_tokenizer *t)
{
    return t->name.len == t->last_start_len &&
           (t->last_start_len == 0 ||
            !memcmp(cb_ptr(&t->name), t->last_start_tag, t->last_start_len));
}

static void remember_start_tag(struct html_tokenizer *t)
{
    uint32_t n = t->name.len;
    if (n > sizeof t->last_start_tag) n = sizeof t->last_start_tag;
    memcpy(t->last_start_tag, cb_ptr(&t->name), n);
    t->last_start_len = n;
}

/* The three RCDATA/RAWTEXT/script end-tag-name states differ only in which
 * state "anything else" falls back to, so they share one body. */
static int end_tag_name(struct html_tokenizer *t, struct html_token *out,
                        int fallback, size_t m, int c)
{
    if (c == C_NEED) return R_NEED;
    if (is_ws(c) && appropriate(t))      { t->state = ST_BEFORE_ATTR_NAME; return 0; }
    if (c == '/'  && appropriate(t))     { t->state = ST_SELF_CLOSING;     return 0; }
    if (c == '>'  && appropriate(t))     { t->state = HTML_STATE_DATA; return deliver(t, out); }
    if (is_alpha(c)) {
        cb_putc(&t->name, lc(c));
        cb_putc(&t->temp, c);
        return 0;
    }
    /* not our end tag after all: the "</" and everything matched so far were
     * ordinary text.  Discard the half-built token and replay them. */
    t->cur_type = CUR_NONE;
    emit_str(t, "</", 2);
    emit_str(t, cb_ptr(&t->temp), t->temp.len);
    t->pos = m;
    t->state = fallback;
    return 0;
}

static int run(struct html_tokenizer *t, struct html_token *out)
{
    for (;;) {
        size_t m = t->pos;
        int c;
        t->cpos = (uint32_t)m;

        switch (t->state) {

        /* ================================================== data-ish ==== */
        case HTML_STATE_DATA: {
            /* rule 2: the zero-copy fast path.  Plain text with no '&', '<',
             * NUL or CR is the overwhelming majority of every real document,
             * and it is the only thing we hand back as a slice of the input. */
            size_t s = t->pos, p = s;
            while (p < t->len) {
                unsigned char ch = (unsigned char)t->buf[p];
                if (ch == '&' || ch == '<' || ch == 0 || ch == '\r') break;
                p++;
            }
            if (p > s) {
                if (p == t->len && !t->eof) return R_NEED;   /* rule 3 */
                if (!t->chars.len) {
                    t->pos = p;
                    memset(out, 0, sizeof *out);
                    out->type = TOK_CHARS;
                    out->data = t->buf + s;
                    out->datalen = (uint32_t)(p - s);
                    out->src_start = (uint32_t)s;
                    out->src_end = (uint32_t)p;
                    return 1;
                }
                emit_str(t, t->buf + s, (uint32_t)(p - s));
                t->pos = p;
                continue;
            }
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '&') { t->return_state = HTML_STATE_DATA;
                            int r = char_ref(t); if (r == R_NEED) return R_NEED; continue; }
            if (c == '<') { t->state = ST_TAG_OPEN; continue; }
            emit_ch(t, c);            /* NUL is emitted as-is in data state */
            continue;
        }

        case HTML_STATE_RCDATA:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '&') { t->return_state = HTML_STATE_RCDATA;
                            int r = char_ref(t); if (r == R_NEED) return R_NEED; continue; }
            if (c == '<') { t->state = ST_RCDATA_LT; continue; }
            if (c == 0)   { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case HTML_STATE_RAWTEXT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '<') { t->state = ST_RAWTEXT_LT; continue; }
            if (c == 0)   { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case HTML_STATE_SCRIPT_DATA:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '<') { t->state = ST_SCRIPT_LT; continue; }
            if (c == 0)   { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case HTML_STATE_PLAINTEXT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == 0)   { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_EOF_ONLY:
            return emit_eof(t, out);

        /* ==================================================== tags ====== */
        case ST_TAG_OPEN:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '!') { t->state = ST_MARKUP_DECL; continue; }
            if (c == '/') { t->state = ST_END_TAG_OPEN; continue; }
            if (is_alpha(c)) {
                start_token(t, TOK_START, m - 1);
                t->pos = m;
                t->state = ST_TAG_NAME;
                continue;
            }
            if (c == '?') {                       /* bogus XML PI etc. */
                start_token(t, TOK_COMMENT, m - 1);
                t->pos = m;
                t->state = ST_BOGUS_COMMENT;
                continue;
            }
            emit_ch(t, '<');
            if (c == C_EOF) return emit_eof(t, out);
            t->pos = m;
            t->state = HTML_STATE_DATA;
            continue;

        case ST_END_TAG_OPEN:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_alpha(c)) {
                start_token(t, TOK_END, m - 2);
                t->pos = m;
                t->state = ST_TAG_NAME;
                continue;
            }
            if (c == '>') { t->state = HTML_STATE_DATA; continue; }   /* "</>" vanishes */
            if (c == C_EOF) { emit_str(t, "</", 2); return emit_eof(t, out); }
            start_token(t, TOK_COMMENT, m - 2);
            t->pos = m;
            t->state = ST_BOGUS_COMMENT;
            continue;

        case ST_TAG_NAME:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);      /* the tag is dropped */
            if (is_ws(c)) { t->state = ST_BEFORE_ATTR_NAME; continue; }
            if (c == '/') { t->state = ST_SELF_CLOSING; continue; }
            if (c == '>') {
                t->state = HTML_STATE_DATA;
                if (t->cur_type == TOK_START) remember_start_tag(t);
                return deliver(t, out);
            }
            if (c == 0) { cb_putcp(&t->name, REPLACEMENT); continue; }
            cb_putc(&t->name, lc(c));
            continue;

        /* ------------------------------------------------ RCDATA "<" ---- */
        case ST_RCDATA_LT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '/') { cb_reset(&t->temp); t->state = ST_RCDATA_END_OPEN; continue; }
            emit_ch(t, '<');
            t->pos = m;
            t->state = HTML_STATE_RCDATA;
            continue;

        case ST_RCDATA_END_OPEN:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_alpha(c)) {
                start_token(t, TOK_END, m - 2);
                t->pos = m;
                t->state = ST_RCDATA_END_NAME;
                continue;
            }
            emit_str(t, "</", 2);
            t->pos = m;
            t->state = HTML_STATE_RCDATA;
            continue;

        case ST_RCDATA_END_NAME: {
            c = nextc(t);
            int r = end_tag_name(t, out, HTML_STATE_RCDATA, m, c);
            if (r) return r;
            continue;
        }

        /* ----------------------------------------------- RAWTEXT "<" ---- */
        case ST_RAWTEXT_LT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '/') { cb_reset(&t->temp); t->state = ST_RAWTEXT_END_OPEN; continue; }
            emit_ch(t, '<');
            t->pos = m;
            t->state = HTML_STATE_RAWTEXT;
            continue;

        case ST_RAWTEXT_END_OPEN:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_alpha(c)) {
                start_token(t, TOK_END, m - 2);
                t->pos = m;
                t->state = ST_RAWTEXT_END_NAME;
                continue;
            }
            emit_str(t, "</", 2);
            t->pos = m;
            t->state = HTML_STATE_RAWTEXT;
            continue;

        case ST_RAWTEXT_END_NAME: {
            c = nextc(t);
            int r = end_tag_name(t, out, HTML_STATE_RAWTEXT, m, c);
            if (r) return r;
            continue;
        }

        /* ============================================ script data ======= */
        case ST_SCRIPT_LT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '/') { cb_reset(&t->temp); t->state = ST_SCRIPT_END_OPEN; continue; }
            if (c == '!') { emit_str(t, "<!", 2); t->state = ST_SCRIPT_ESC_START; continue; }
            emit_ch(t, '<');
            t->pos = m;
            t->state = HTML_STATE_SCRIPT_DATA;
            continue;

        case ST_SCRIPT_END_OPEN:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_alpha(c)) {
                start_token(t, TOK_END, m - 2);
                t->pos = m;
                t->state = ST_SCRIPT_END_NAME;
                continue;
            }
            emit_str(t, "</", 2);
            t->pos = m;
            t->state = HTML_STATE_SCRIPT_DATA;
            continue;

        case ST_SCRIPT_END_NAME: {
            c = nextc(t);
            int r = end_tag_name(t, out, HTML_STATE_SCRIPT_DATA, m, c);
            if (r) return r;
            continue;
        }

        /* "<!--" inside <script> opens a region where "</script>" is TEXT.
         * See the file header: getting this wrong swallows the page. */
        case ST_SCRIPT_ESC_START:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { emit_ch(t, '-'); t->state = ST_SCRIPT_ESC_START_DASH; continue; }
            t->pos = m;
            t->state = HTML_STATE_SCRIPT_DATA;
            continue;

        case ST_SCRIPT_ESC_START_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { emit_ch(t, '-'); t->state = ST_SCRIPT_ESCAPED_DASH_DASH; continue; }
            t->pos = m;
            t->state = HTML_STATE_SCRIPT_DATA;
            continue;

        case ST_SCRIPT_ESCAPED:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '-') { emit_ch(t, '-'); t->state = ST_SCRIPT_ESCAPED_DASH; continue; }
            if (c == '<') { t->state = ST_SCRIPT_ESCAPED_LT; continue; }
            if (c == 0)   { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_SCRIPT_ESCAPED_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '-') { emit_ch(t, '-'); t->state = ST_SCRIPT_ESCAPED_DASH_DASH; continue; }
            if (c == '<') { t->state = ST_SCRIPT_ESCAPED_LT; continue; }
            t->state = ST_SCRIPT_ESCAPED;
            if (c == 0) { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_SCRIPT_ESCAPED_DASH_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '-') { emit_ch(t, '-'); continue; }
            if (c == '<') { t->state = ST_SCRIPT_ESCAPED_LT; continue; }
            if (c == '>') { emit_ch(t, '>'); t->state = HTML_STATE_SCRIPT_DATA; continue; }
            t->state = ST_SCRIPT_ESCAPED;
            if (c == 0) { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_SCRIPT_ESCAPED_LT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '/') { cb_reset(&t->temp); t->state = ST_SCRIPT_ESCAPED_END_OPEN; continue; }
            if (is_alpha(c)) {
                cb_reset(&t->temp);
                emit_ch(t, '<');
                t->pos = m;
                t->state = ST_SCRIPT_DESC_START;
                continue;
            }
            emit_ch(t, '<');
            t->pos = m;
            t->state = ST_SCRIPT_ESCAPED;
            continue;

        case ST_SCRIPT_ESCAPED_END_OPEN:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_alpha(c)) {
                start_token(t, TOK_END, m - 2);
                t->pos = m;
                t->state = ST_SCRIPT_ESCAPED_END_NAME;
                continue;
            }
            emit_str(t, "</", 2);
            t->pos = m;
            t->state = ST_SCRIPT_ESCAPED;
            continue;

        case ST_SCRIPT_ESCAPED_END_NAME: {
            c = nextc(t);
            int r = end_tag_name(t, out, ST_SCRIPT_ESCAPED, m, c);
            if (r) return r;
            continue;
        }

        /* "<script" inside the escaped region: now even "-->" does not get us
         * out until a matching "</script" appears. */
        case ST_SCRIPT_DESC_START:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c) || c == '/' || c == '>') {
                t->state = (t->temp.len == 6 && !memcmp(cb_ptr(&t->temp), "script", 6))
                         ? ST_SCRIPT_DESCAPED : ST_SCRIPT_ESCAPED;
                emit_ch(t, c);
                continue;
            }
            if (is_alpha(c)) { cb_putc(&t->temp, lc(c)); emit_ch(t, c); continue; }
            t->pos = m;
            t->state = ST_SCRIPT_ESCAPED;
            continue;

        case ST_SCRIPT_DESCAPED:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '-') { emit_ch(t, '-'); t->state = ST_SCRIPT_DESCAPED_DASH; continue; }
            if (c == '<') { emit_ch(t, '<'); t->state = ST_SCRIPT_DESCAPED_LT; continue; }
            if (c == 0)   { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_SCRIPT_DESCAPED_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '-') { emit_ch(t, '-'); t->state = ST_SCRIPT_DESCAPED_DASH_DASH; continue; }
            if (c == '<') { emit_ch(t, '<'); t->state = ST_SCRIPT_DESCAPED_LT; continue; }
            t->state = ST_SCRIPT_DESCAPED;
            if (c == 0) { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_SCRIPT_DESCAPED_DASH_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '-') { emit_ch(t, '-'); continue; }
            if (c == '<') { emit_ch(t, '<'); t->state = ST_SCRIPT_DESCAPED_LT; continue; }
            if (c == '>') { emit_ch(t, '>'); t->state = HTML_STATE_SCRIPT_DATA; continue; }
            t->state = ST_SCRIPT_DESCAPED;
            if (c == 0) { emit_cp(t, REPLACEMENT); continue; }
            emit_ch(t, c);
            continue;

        case ST_SCRIPT_DESCAPED_LT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '/') { cb_reset(&t->temp); emit_ch(t, '/'); t->state = ST_SCRIPT_DESC_END; continue; }
            t->pos = m;
            t->state = ST_SCRIPT_DESCAPED;
            continue;

        case ST_SCRIPT_DESC_END:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c) || c == '/' || c == '>') {
                t->state = (t->temp.len == 6 && !memcmp(cb_ptr(&t->temp), "script", 6))
                         ? ST_SCRIPT_ESCAPED : ST_SCRIPT_DESCAPED;
                emit_ch(t, c);
                continue;
            }
            if (is_alpha(c)) { cb_putc(&t->temp, lc(c)); emit_ch(t, c); continue; }
            t->pos = m;
            t->state = ST_SCRIPT_DESCAPED;
            continue;

        /* ============================================== attributes ====== */
        case ST_BEFORE_ATTR_NAME:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) continue;
            if (c == '/' || c == '>' || c == C_EOF) {
                t->pos = m;
                t->state = ST_AFTER_ATTR_NAME;
                continue;
            }
            new_attr(t);
            if (c == '=') { attr_name_put(t, "=", 1); t->state = ST_ATTR_NAME; continue; }
            t->pos = m;
            t->state = ST_ATTR_NAME;
            continue;

        case ST_ATTR_NAME:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c) || c == '/' || c == '>' || c == C_EOF) {
                finish_attr_name(t);
                t->pos = m;
                t->state = ST_AFTER_ATTR_NAME;
                continue;
            }
            if (c == '=') { finish_attr_name(t); t->state = ST_BEFORE_ATTR_VALUE; continue; }
            if (c == 0) {
                uint32_t b = t->attrbuf.len;
                cb_putcp(&t->attrbuf, REPLACEMENT);
                if (t->nattr) t->attrs[t->nattr - 1].nlen += t->attrbuf.len - b;
                continue;
            }
            { char ch = (char)lc(c); attr_name_put(t, &ch, 1); }
            continue;

        case ST_AFTER_ATTR_NAME:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (is_ws(c)) continue;
            if (c == '/') { t->state = ST_SELF_CLOSING; continue; }
            if (c == '=') { t->state = ST_BEFORE_ATTR_VALUE; continue; }
            if (c == '>') {
                t->state = HTML_STATE_DATA;
                if (t->cur_type == TOK_START) remember_start_tag(t);
                return deliver(t, out);
            }
            new_attr(t);
            t->pos = m;
            t->state = ST_ATTR_NAME;
            continue;

        case ST_BEFORE_ATTR_VALUE:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) continue;
            if (c == '"') { t->state = ST_ATTR_VALUE_DQ; continue; }
            if (c == '\'') { t->state = ST_ATTR_VALUE_SQ; continue; }
            if (c == '>') {                        /* missing-attribute-value */
                t->state = HTML_STATE_DATA;
                if (t->cur_type == TOK_START) remember_start_tag(t);
                return deliver(t, out);
            }
            t->pos = m;
            t->state = ST_ATTR_VALUE_UQ;
            continue;

        case ST_ATTR_VALUE_DQ:
        case ST_ATTR_VALUE_SQ: {
            int quote = t->state == ST_ATTR_VALUE_DQ ? '"' : '\'';
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == quote) { t->state = ST_AFTER_ATTR_VALUE_Q; continue; }
            if (c == '&') { t->return_state = t->state;
                            int r = char_ref(t); if (r == R_NEED) return R_NEED; continue; }
            if (c == 0) { attr_val_putcp(t, REPLACEMENT); continue; }
            { char ch = (char)c; attr_val_put(t, &ch, 1); }
            continue;
        }

        case ST_ATTR_VALUE_UQ:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (is_ws(c)) { t->state = ST_BEFORE_ATTR_NAME; continue; }
            if (c == '&') { t->return_state = ST_ATTR_VALUE_UQ;
                            int r = char_ref(t); if (r == R_NEED) return R_NEED; continue; }
            if (c == '>') {
                t->state = HTML_STATE_DATA;
                if (t->cur_type == TOK_START) remember_start_tag(t);
                return deliver(t, out);
            }
            if (c == 0) { attr_val_putcp(t, REPLACEMENT); continue; }
            { char ch = (char)c; attr_val_put(t, &ch, 1); }
            continue;

        case ST_AFTER_ATTR_VALUE_Q:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (is_ws(c)) { t->state = ST_BEFORE_ATTR_NAME; continue; }
            if (c == '/') { t->state = ST_SELF_CLOSING; continue; }
            if (c == '>') {
                t->state = HTML_STATE_DATA;
                if (t->cur_type == TOK_START) remember_start_tag(t);
                return deliver(t, out);
            }
            t->pos = m;                        /* missing-whitespace-between-attributes */
            t->state = ST_BEFORE_ATTR_NAME;
            continue;

        case ST_SELF_CLOSING:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == '>') {
                t->self_closing = 1;
                t->state = HTML_STATE_DATA;
                if (t->cur_type == TOK_START) remember_start_tag(t);
                return deliver(t, out);
            }
            t->pos = m;
            t->state = ST_BEFORE_ATTR_NAME;
            continue;

        /* =============================================== comments ======= */
        case ST_BOGUS_COMMENT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            if (c == 0) { cb_putcp(&t->comment, REPLACEMENT); continue; }
            cb_putc(&t->comment, c);
            continue;

        case ST_MARKUP_DECL: {
            int r = lookahead(t, t->pos, "--", 2, 0);
            if (r == R_NEED) return R_NEED;
            if (r) { t->pos += 2; start_token(t, TOK_COMMENT, m - 2);
                     t->state = ST_COMMENT_START; continue; }
            r = lookahead(t, t->pos, "DOCTYPE", 7, 1);
            if (r == R_NEED) return R_NEED;
            if (r) { t->pos += 7; t->state = ST_DOCTYPE; continue; }
            r = lookahead(t, t->pos, "[CDATA[", 7, 0);
            if (r == R_NEED) return R_NEED;
            if (r) {
                t->pos += 7;
                /* Only legal inside foreign content.  In HTML it is a comment,
                 * which is what keeps "<![CDATA[" in a stray XHTML fragment
                 * from being rendered as text. */
                if (t->in_foreign) { t->state = HTML_STATE_CDATA_SECTION; continue; }
                start_token(t, TOK_COMMENT, m - 2);
                cb_put(&t->comment, "[CDATA[", 7);
                t->state = ST_BOGUS_COMMENT;
                continue;
            }
            start_token(t, TOK_COMMENT, m - 2);
            t->state = ST_BOGUS_COMMENT;
            continue;
        }

        case ST_COMMENT_START:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { t->state = ST_COMMENT_START_DASH; continue; }
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        case ST_COMMENT_START_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { t->state = ST_COMMENT_END; continue; }
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            cb_putc(&t->comment, '-');
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        case ST_COMMENT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            if (c == '<') { cb_putc(&t->comment, '<'); t->state = ST_COMMENT_LT; continue; }
            if (c == '-') { t->state = ST_COMMENT_END_DASH; continue; }
            if (c == 0) { cb_putcp(&t->comment, REPLACEMENT); continue; }
            cb_putc(&t->comment, c);
            continue;

        /* "<!" inside a comment.  These four states exist so that
         * "<!-- <!-- -->" nests the way browsers agree it does. */
        case ST_COMMENT_LT:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '!') { cb_putc(&t->comment, '!'); t->state = ST_COMMENT_LT_BANG; continue; }
            if (c == '<') { cb_putc(&t->comment, '<'); continue; }
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        case ST_COMMENT_LT_BANG:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { t->state = ST_COMMENT_LT_BANG_DASH; continue; }
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        case ST_COMMENT_LT_BANG_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { t->state = ST_COMMENT_LT_BANG_DASH_DASH; continue; }
            t->pos = m;
            t->state = ST_COMMENT_END_DASH;
            continue;

        case ST_COMMENT_LT_BANG_DASH_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            t->pos = m;                          /* '>' and EOF alike */
            t->state = ST_COMMENT_END;
            continue;

        case ST_COMMENT_END_DASH:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { t->state = ST_COMMENT_END; continue; }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            cb_putc(&t->comment, '-');
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        case ST_COMMENT_END:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == '!') { t->state = ST_COMMENT_END_BANG; continue; }
            if (c == '-') { cb_putc(&t->comment, '-'); continue; }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            cb_put(&t->comment, "--", 2);
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        /* "--!>": invalid, but a long tail of templates emit it and every
         * browser closes the comment here.  Not doing so eats the rest of the
         * document. */
        case ST_COMMENT_END_BANG:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '-') { cb_put(&t->comment, "--!", 3); t->state = ST_COMMENT_END_DASH; continue; }
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            cb_put(&t->comment, "--!", 3);
            t->pos = m;
            t->state = ST_COMMENT;
            continue;

        /* ================================================ doctype ======= */
        /* All of it, rather than "skip to '>'": quirks mode is decided by the
         * public and system identifiers, and quirks mode decides the box model
         * for the whole document.  A tokenizer that throws the identifiers away
         * cannot render a 1998 page the way its author saw it. */
        case ST_DOCTYPE:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) { t->state = ST_BEFORE_DOCTYPE_NAME; continue; }
            if (c == C_EOF) {
                start_token(t, TOK_DOCTYPE, m);
                t->force_quirks = 1;
                t->state = ST_EOF_ONLY;
                return deliver(t, out);
            }
            t->pos = m;
            t->state = ST_BEFORE_DOCTYPE_NAME;
            continue;

        case ST_BEFORE_DOCTYPE_NAME:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) continue;
            if (c == C_EOF) {
                start_token(t, TOK_DOCTYPE, m);
                t->force_quirks = 1;
                t->state = ST_EOF_ONLY;
                return deliver(t, out);
            }
            if (c == '>') {
                start_token(t, TOK_DOCTYPE, m);
                t->force_quirks = 1;
                t->state = HTML_STATE_DATA;
                return deliver(t, out);
            }
            start_token(t, TOK_DOCTYPE, m);
            t->has_name = 1;
            if (c == 0) cb_putcp(&t->name, REPLACEMENT);
            else cb_putc(&t->name, lc(c));
            t->state = ST_DOCTYPE_NAME;
            continue;

        case ST_DOCTYPE_NAME:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) { t->state = ST_AFTER_DOCTYPE_NAME; continue; }
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->force_quirks = 1; t->state = ST_EOF_ONLY; return deliver(t, out); }
            if (c == 0) { cb_putcp(&t->name, REPLACEMENT); continue; }
            cb_putc(&t->name, lc(c));
            continue;

        case ST_AFTER_DOCTYPE_NAME: {
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) continue;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->force_quirks = 1; t->state = ST_EOF_ONLY; return deliver(t, out); }
            int r = lookahead(t, m, "PUBLIC", 6, 1);
            if (r == R_NEED) return R_NEED;
            if (r) { t->pos = m + 6; t->state = ST_AFTER_DT_PUB_KW; continue; }
            r = lookahead(t, m, "SYSTEM", 6, 1);
            if (r == R_NEED) return R_NEED;
            if (r) { t->pos = m + 6; t->state = ST_AFTER_DT_SYS_KW; continue; }
            t->force_quirks = 1;
            t->pos = m;
            t->state = ST_BOGUS_DOCTYPE;
            continue;
        }

        case ST_AFTER_DT_PUB_KW:
        case ST_BEFORE_DT_PUB_ID:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) {
                if (t->state == ST_AFTER_DT_PUB_KW) t->state = ST_BEFORE_DT_PUB_ID;
                continue;
            }
            if (c == '"') { t->has_pubid = 1; cb_reset(&t->pubid); t->state = ST_DT_PUB_ID_DQ; continue; }
            if (c == '\'') { t->has_pubid = 1; cb_reset(&t->pubid); t->state = ST_DT_PUB_ID_SQ; continue; }
            t->force_quirks = 1;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            t->pos = m;
            t->state = ST_BOGUS_DOCTYPE;
            continue;

        case ST_DT_PUB_ID_DQ:
        case ST_DT_PUB_ID_SQ: {
            int quote = t->state == ST_DT_PUB_ID_DQ ? '"' : '\'';
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == quote) { t->state = ST_AFTER_DT_PUB_ID; continue; }
            if (c == 0) { cb_putcp(&t->pubid, REPLACEMENT); continue; }
            if (c == '>') { t->force_quirks = 1; t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->force_quirks = 1; t->state = ST_EOF_ONLY; return deliver(t, out); }
            cb_putc(&t->pubid, c);
            continue;
        }

        case ST_AFTER_DT_PUB_ID:
        case ST_BETWEEN_DT_PUB_SYS:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) {
                if (t->state == ST_AFTER_DT_PUB_ID) t->state = ST_BETWEEN_DT_PUB_SYS;
                continue;
            }
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == '"') { t->has_sysid = 1; cb_reset(&t->sysid); t->state = ST_DT_SYS_ID_DQ; continue; }
            if (c == '\'') { t->has_sysid = 1; cb_reset(&t->sysid); t->state = ST_DT_SYS_ID_SQ; continue; }
            t->force_quirks = 1;
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            t->pos = m;
            t->state = ST_BOGUS_DOCTYPE;
            continue;

        case ST_AFTER_DT_SYS_KW:
        case ST_BEFORE_DT_SYS_ID:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) {
                if (t->state == ST_AFTER_DT_SYS_KW) t->state = ST_BEFORE_DT_SYS_ID;
                continue;
            }
            if (c == '"') { t->has_sysid = 1; cb_reset(&t->sysid); t->state = ST_DT_SYS_ID_DQ; continue; }
            if (c == '\'') { t->has_sysid = 1; cb_reset(&t->sysid); t->state = ST_DT_SYS_ID_SQ; continue; }
            t->force_quirks = 1;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            t->pos = m;
            t->state = ST_BOGUS_DOCTYPE;
            continue;

        case ST_DT_SYS_ID_DQ:
        case ST_DT_SYS_ID_SQ: {
            int quote = t->state == ST_DT_SYS_ID_DQ ? '"' : '\'';
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == quote) { t->state = ST_AFTER_DT_SYS_ID; continue; }
            if (c == 0) { cb_putcp(&t->sysid, REPLACEMENT); continue; }
            if (c == '>') { t->force_quirks = 1; t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->force_quirks = 1; t->state = ST_EOF_ONLY; return deliver(t, out); }
            cb_putc(&t->sysid, c);
            continue;
        }

        case ST_AFTER_DT_SYS_ID:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (is_ws(c)) continue;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->force_quirks = 1; t->state = ST_EOF_ONLY; return deliver(t, out); }
            t->pos = m;                   /* junk after the system id does NOT
                                           * force quirks -- only an unparsable
                                           * identifier does */
            t->state = ST_BOGUS_DOCTYPE;
            continue;

        case ST_BOGUS_DOCTYPE:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == '>') { t->state = HTML_STATE_DATA; return deliver(t, out); }
            if (c == C_EOF) { t->state = ST_EOF_ONLY; return deliver(t, out); }
            continue;                     /* everything else is discarded */

        /* ================================================== CDATA ======= */
        case HTML_STATE_CDATA_SECTION:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == C_EOF) return emit_eof(t, out);
            if (c == ']') { t->state = ST_CDATA_BRACKET; continue; }
            emit_ch(t, c);                /* NUL survives inside CDATA */
            continue;

        case ST_CDATA_BRACKET:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == ']') { t->state = ST_CDATA_END; continue; }
            emit_ch(t, ']');
            t->pos = m;
            t->state = HTML_STATE_CDATA_SECTION;
            continue;

        case ST_CDATA_END:
            c = nextc(t);
            if (c == C_NEED) return R_NEED;
            if (c == ']') { emit_ch(t, ']'); continue; }
            if (c == '>') { t->state = HTML_STATE_DATA; continue; }
            emit_str(t, "]]", 2);
            t->pos = m;
            t->state = HTML_STATE_CDATA_SECTION;
            continue;

        default:
            /* unreachable; fail closed rather than spin */
            return emit_eof(t, out);
        }
    }
}

/* ------------------------------------------------------------- public --- */

void html_tok_init(struct html_tokenizer *t, const char *src, size_t len, int eof)
{
    memset(t, 0, sizeof *t);
    t->buf = src; t->len = len; t->eof = eof;
    t->state = HTML_STATE_DATA;
    t->return_state = HTML_STATE_DATA;
    t->cur_type = CUR_NONE;
}

void html_tok_free(struct html_tokenizer *t)
{
    free(t->name.p); free(t->comment.p); free(t->pubid.p);
    free(t->sysid.p); free(t->chars.p); free(t->temp.p); free(t->attrbuf.p);
    free(t->attrs); free(t->oattrs);
    memset(t, 0, sizeof *t);
    t->cur_type = CUR_NONE;
}

void html_tok_set_state(struct html_tokenizer *t, int state)
{
    if (state >= 0 && state < HTML_STATE__PUBLIC) t->state = state;
}

void html_tok_set_last_start_tag(struct html_tokenizer *t, const char *name, uint32_t len)
{
    if (len > sizeof t->last_start_tag) len = sizeof t->last_start_tag;
    memcpy(t->last_start_tag, name, len);
    t->last_start_len = len;
}

void html_tok_set_foreign(struct html_tokenizer *t, int foreign) { t->in_foreign = foreign; }

void html_tok_feed(struct html_tokenizer *t, const char *src, size_t len, int eof)
{
    t->buf = src; t->len = len; t->eof = eof;
}

int html_tok_next(struct html_tokenizer *t, struct html_token *out)
{
    if (t->has_pending) {
        *out = t->pending;
        t->has_pending = 0;
        cb_reset(&t->chars);
        return 1;
    }
    if (t->finished) return -1;

    cb_reset(&t->chars);

    /* rule 3: remember where this token began.  If we run out of input before
     * it is complete we rewind to exactly here, drop everything partial, and
     * let the caller re-run us with more bytes.  Nothing in the machine has to
     * be resumable, which is why there is no "half a doctype" case anywhere. */
    size_t start_pos = t->pos;
    int start_state = t->state;

    int r = run(t, out);
    if (r == R_NEED) {
        t->pos = start_pos;
        t->state = start_state;
        t->cur_type = CUR_NONE;
        t->nattr = 0;
        cb_reset(&t->name); cb_reset(&t->comment); cb_reset(&t->pubid);
        cb_reset(&t->sysid); cb_reset(&t->chars); cb_reset(&t->temp);
        cb_reset(&t->attrbuf);
        t->self_closing = t->force_quirks = 0;
        t->has_name = t->has_pubid = t->has_sysid = 0;
        return 0;
    }
    return r;
}
