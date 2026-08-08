/* url_test.c -- the WHATWG URL parser measured against the corpus's OWN data.
 *
 * third_party/wpt/url/resources/urltestdata.json is a machine-readable table of
 * (input, base) -> the eleven components, and setters_tests.json is the same
 * for the setters. url-constructor.any.js and url-setters.any.js are thin
 * loops over exactly these two files, so driving them directly measures the
 * same thing the corpus does -- in a second, with no runner, no testharness
 * and no virtual clock. That matters here beyond convenience: the WPT runner
 * currently cannot finish a 267 KB fetch, so url-constructor.any.js is one red
 * line standing for 1,004 cases that never execute. This file executes them.
 *
 * It is NOT a replacement for the corpus run and does not pretend to be: the
 * two numbers are printed separately and the report says which is which.
 * What this file cannot see is everything about the JS surface -- that a
 * getter is on the prototype, that the constructor throws a TypeError rather
 * than returning null, that searchParams is live. Those are what url/ in the
 * corpus is for.
 *
 * THE GATE IS A RATCHET, the same shape tests/wpt.mk uses: a committed list of
 * case ids that are expected to fail. An unexpected failure is red. A case
 * that starts passing is reported so the list can be re-cut, but is not red --
 * a green build must not become the thing that stops you fixing a bug. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_url.h"

/* ===================================================================== *
 *  a small JSON reader -- enough for these three files
 * ===================================================================== */

typedef struct jv jv;
struct jv {
    int type;               /* 0 null 1 bool 2 num 3 str 4 arr 5 obj */
    int b;
    double num;
    char *s;                /* str: UTF-8 */
    int slen;               /* ... and its LENGTH: a corpus input may contain
                             * U+0000 (13 constructor cases and one setter
                             * case do), and strlen would silently truncate
                             * exactly the inputs that test how the parser
                             * handles it. */
    jv **kids; char **keys; int n, cap;
};

static void jv_free(jv *v)
{
    if (!v) return;
    free(v->s);
    for (int i = 0; i < v->n; i++) { jv_free(v->kids[i]); if (v->keys) free(v->keys[i]); }
    free(v->kids); free(v->keys);
    free(v);
}

static jv *jv_new(int t) { jv *v = calloc(1, sizeof(jv)); v->type = t; return v; }

static void jv_push(jv *p, const char *key, jv *c)
{
    if (p->n + 1 > p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->kids = realloc(p->kids, sizeof(jv *) * p->cap);
        p->keys = realloc(p->keys, sizeof(char *) * p->cap);
    }
    p->kids[p->n] = c;
    p->keys[p->n] = key ? strdup(key) : NULL;
    p->n++;
}

static const char *J;

static void jskip(void) { while (*J == ' ' || *J == '\t' || *J == '\n' || *J == '\r') J++; }

static void putcp(char **out, int *n, int *cap, unsigned cp)
{
    char t[4]; int k = 0;
    if (cp < 0x80) t[k++] = (char)cp;
    else if (cp < 0x800) { t[k++] = (char)(0xC0 | (cp >> 6)); t[k++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        t[k++] = (char)(0xE0 | (cp >> 12));
        t[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[k++] = (char)(0x80 | (cp & 0x3F));
    } else {
        t[k++] = (char)(0xF0 | (cp >> 18));
        t[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[k++] = (char)(0x80 | (cp & 0x3F));
    }
    if (*n + k + 1 > *cap) { *cap = (*cap ? *cap * 2 : 32) + k; *out = realloc(*out, *cap); }
    memcpy(*out + *n, t, k);
    *n += k;
    (*out)[*n] = 0;
}

static int g_slen;

static char *jstring(void)
{
    J++;  /* opening quote */
    char *out = malloc(1); int n = 0, cap = 1; out[0] = 0;
    while (*J && *J != '"') {
        if (*J == '\\') {
            J++;
            switch (*J) {
            case 'n': putcp(&out, &n, &cap, '\n'); J++; break;
            case 't': putcp(&out, &n, &cap, '\t'); J++; break;
            case 'r': putcp(&out, &n, &cap, '\r'); J++; break;
            case 'b': putcp(&out, &n, &cap, '\b'); J++; break;
            case 'f': putcp(&out, &n, &cap, '\f'); J++; break;
            case 'u': {
                J++;
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    int c = *J++;
                    v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
                }
                if (v >= 0xD800 && v <= 0xDBFF && J[0] == '\\' && J[1] == 'u') {
                    unsigned lo = 0;
                    const char *save = J;
                    J += 2;
                    for (int i = 0; i < 4; i++) {
                        int c = *J++;
                        lo = lo * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    else { J = save; v = 0xFFFD; }
                } else if (v >= 0xD800 && v <= 0xDFFF) {
                    /* A lone surrogate is not a scalar value. A JS string can
                     * hold one; UTF-8 encoding it -- which is what the URL
                     * parser does with its input -- yields U+FFFD. */
                    v = 0xFFFD;
                }
                putcp(&out, &n, &cap, v);
                break; }
            default: putcp(&out, &n, &cap, (unsigned char)*J); J++; break;
            }
        } else {
            /* A raw byte, NOT a code point: the file is already UTF-8 and a
             * non-ASCII byte must pass through verbatim. Re-encoding it as a
             * code point is Latin-1 mojibake, which is exactly what the first
             * run of this file produced. */
            if (n + 2 > cap) { cap = cap ? cap * 2 : 32; out = realloc(out, cap); }
            out[n++] = *J++;
            out[n] = 0;
        }
    }
    J++;  /* closing quote */
    g_slen = n;
    return out;
}

static jv *jparse(void)
{
    jskip();
    if (*J == '{') {
        J++;
        jv *o = jv_new(5);
        jskip();
        if (*J == '}') { J++; return o; }
        for (;;) {
            jskip();
            char *k = jstring();
            jskip(); J++;   /* ':' */
            jv *v = jparse();
            jv_push(o, k, v);
            free(k);
            jskip();
            if (*J == ',') { J++; continue; }
            if (*J == '}') { J++; break; }
            break;
        }
        return o;
    }
    if (*J == '[') {
        J++;
        jv *a = jv_new(4);
        jskip();
        if (*J == ']') { J++; return a; }
        for (;;) {
            jv *v = jparse();
            jv_push(a, NULL, v);
            jskip();
            if (*J == ',') { J++; continue; }
            if (*J == ']') { J++; break; }
            break;
        }
        return a;
    }
    if (*J == '"') { jv *s = jv_new(3); s->s = jstring(); s->slen = g_slen; return s; }
    if (!strncmp(J, "true", 4)) { J += 4; jv *v = jv_new(1); v->b = 1; return v; }
    if (!strncmp(J, "false", 5)) { J += 5; jv *v = jv_new(1); v->b = 0; return v; }
    if (!strncmp(J, "null", 4)) { J += 4; return jv_new(0); }
    { jv *v = jv_new(2); char *e; v->num = strtod(J, &e); J = e; return v; }
}

static jv *jget(jv *o, const char *k)
{
    if (!o || o->type != 5) return NULL;
    for (int i = 0; i < o->n; i++) if (o->keys[i] && !strcmp(o->keys[i], k)) return o->kids[i];
    return NULL;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    return b;
}

/* ===================================================================== *
 *  the ratchet
 * ===================================================================== */

static char **g_expect;
static int g_nexpect;
static int g_verbose;

static void load_expect(const char *path)
{
    char *b = slurp(path);
    if (!b) return;
    char *p = b;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        while (*p == ' ') p++;
        int l = (int)strlen(p);
        while (l > 0 && (p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
        if (l && p[0] != '#') {
            g_expect = realloc(g_expect, sizeof(char *) * (g_nexpect + 1));
            g_expect[g_nexpect++] = strdup(p);
        }
        if (!e) break;
        p = e + 1;
    }
    free(b);
}

static int is_expected(const char *id)
{
    for (int i = 0; i < g_nexpect; i++) if (!strcmp(g_expect[i], id)) return 1;
    return 0;
}

static int g_pass, g_fail, g_unexpected, g_newpass;
static char g_seen_pass[65536];   /* index into the expected list, marked */

static void record(const char *id, int ok, const char *detail)
{
    int exp = is_expected(id);
    if (ok) {
        g_pass++;
        if (exp) {
            g_newpass++;
            if (g_verbose > 1) printf("  NEWPASS %s\n", id);
        }
    } else {
        g_fail++;
        if (!exp) {
            g_unexpected++;
            if (g_verbose && g_unexpected <= g_verbose)
                printf("  FAIL %s: %s\n", id, detail ? detail : "");
        }
    }
}

/* ===================================================================== *
 *  the constructor corpus
 * ===================================================================== */

static const char *COMPNAME[] = {
    "href", "protocol", "username", "password", "host", "hostname",
    "port", "pathname", "search", "hash", "origin"
};

static char g_detail[2048];

static int check_components(urlrec *u, jv *e, const char *skip_origin)
{
    for (int c = 0; c < URLC__N; c++) {
        jv *want = jget(e, COMPNAME[c]);
        if (!want || want->type != 3) continue;
        if (c == URLC_ORIGIN && skip_origin) continue;
        char *got = url_get(u, c);
        if (strcmp(got, want->s)) {
            snprintf(g_detail, sizeof(g_detail), "%s: got \"%s\" want \"%s\"",
                     COMPNAME[c], got, want->s);
            free(got);
            return 0;
        }
        free(got);
    }
    return 1;
}

static void run_constructor(const char *root)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/url/resources/urltestdata.json", root);
    char *txt = slurp(path);
    if (!txt) { printf("url_test: %s not found -- constructor corpus skipped\n", path); return; }
    J = txt;
    jv *arr = jparse();
    int idx = 0, tot = 0, ok = 0;
    for (int i = 0; i < arr->n; i++) {
        jv *e = arr->kids[i];
        if (e->type != 5) continue;
        jv *in = jget(e, "input");
        if (!in || in->type != 3) continue;
        char id[64];
        snprintf(id, sizeof(id), "ctor:%d", idx++);
        jv *bv = jget(e, "base");
        urlrec *base = NULL;
        int base_ok = 1;
        if (bv && bv->type == 3) {
            base = url_parse_w(bv->s, bv->slen, NULL);
            if (!base) base_ok = 0;
        }
        tot++;
        if (!base_ok) {
            snprintf(g_detail, sizeof(g_detail), "base \"%s\" did not parse", bv->s);
            record(id, 0, g_detail);
            continue;
        }
        urlrec *u = url_parse_w(in->s, in->slen, base);
        jv *fail = jget(e, "failure");
        int good;
        if (fail && fail->type == 1 && fail->b) {
            good = (u == NULL);
            if (!good) {
                char *h = url_get(u, URLC_HREF);
                snprintf(g_detail, sizeof(g_detail), "expected failure, got \"%s\"", h);
                free(h);
            }
        } else if (!u) {
            good = 0;
            snprintf(g_detail, sizeof(g_detail), "unexpected failure on \"%s\"", in->s);
        } else {
            good = check_components(u, e, NULL);
        }
        if (good) ok++;
        record(id, good, g_detail);
        url_free_w(u);
        url_free_w(base);
    }
    printf("  constructor      %4d/%-4d  (%s/url/resources/urltestdata.json)\n", ok, tot, root);
    jv_free(arr);
    free(txt);
}

/* ===================================================================== *
 *  the setter corpus
 * ===================================================================== */

static int comp_of(const char *name)
{
    for (int i = 0; i < URLC__N; i++) if (!strcmp(COMPNAME[i], name)) return i;
    return -1;
}

static void run_setters(const char *root)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/url/resources/setters_tests.json", root);
    char *txt = slurp(path);
    if (!txt) { printf("url_test: %s not found -- setter corpus skipped\n", path); return; }
    J = txt;
    jv *obj = jparse();
    int tot = 0, ok = 0;
    for (int a = 0; a < obj->n; a++) {
        const char *attr = obj->keys[a];
        if (!attr || !strcmp(attr, "comment")) continue;
        int comp = comp_of(attr);
        jv *cases = obj->kids[a];
        if (comp < 0 || cases->type != 4) continue;
        for (int i = 0; i < cases->n; i++) {
            jv *c = cases->kids[i];
            jv *href = jget(c, "href"), *nv = jget(c, "new_value"), *exp = jget(c, "expected");
            if (!href || !nv || !exp) continue;
            char id[80];
            snprintf(id, sizeof(id), "set:%s:%d", attr, i);
            tot++;
            urlrec *u = url_parse_w(href->s, href->slen, NULL);
            if (!u) {
                snprintf(g_detail, sizeof(g_detail), "base href \"%s\" did not parse", href->s);
                record(id, 0, g_detail);
                continue;
            }
            url_set(u, comp, nv->s, nv->slen);
            int good = check_components(u, exp, NULL);
            if (good) ok++;
            record(id, good, g_detail);
            url_free_w(u);
        }
    }
    printf("  setters          %4d/%-4d  (%s/url/resources/setters_tests.json)\n", ok, tot, root);
    jv_free(obj);
    free(txt);
}

/* ===================================================================== *
 *  IdnaTestV2 -- reported SEPARATELY, because it measures the one part of
 *  this file that is knowingly incomplete (see the IDNA section in js_url.c).
 *  Folding it into the headline would hide exactly the gap the report should
 *  make visible.
 * ===================================================================== */

static void run_idna(const char *root)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/url/resources/IdnaTestV2.json", root);
    char *txt = slurp(path);
    if (!txt) return;
    J = txt;
    jv *arr = jparse();
    int tot = 0, ok = 0;
    for (int i = 0; i < arr->n; i++) {
        jv *e = arr->kids[i];
        if (e->type != 5) continue;
        jv *in = jget(e, "input"), *out = jget(e, "output");
        if (!in || in->type != 3 || !out) continue;
        tot++;
        char *host = url_host_parse(in->s, in->slen, 0);
        int good;
        if (out->type == 0 || (out->type == 1 && !out->b)) good = (host == NULL);
        else if (out->type == 3) good = host && !strcmp(host, out->s);
        else { tot--; free(host); continue; }
        if (good) ok++;
        free(host);
    }
    printf("  IdnaTestV2       %4d/%-4d  (host parsing only; NOT gated -- see js_url.c)\n", ok, tot);
    jv_free(arr);
    free(txt);
}

/* ===================================================================== *
 *  URLSearchParams -- the C-visible half. The live link to a URL and the
 *  iterator protocol are JS-visible only and live in the corpus run.
 * ===================================================================== */

static void expect_str(const char *id, const char *got, const char *want)
{
    int ok = got && want && !strcmp(got, want);
    if (!ok) snprintf(g_detail, sizeof(g_detail), "got \"%s\" want \"%s\"", got ? got : "(null)", want);
    record(id, ok, g_detail);
}

static void run_usp(void)
{
    usplist l;
    usp_init(&l);
    int tot = 0, ok0 = g_pass;

    struct { const char *in, *out; } round[] = {
        { "a=b&c=d", "a=b&c=d" },
        { "a=b&c=d&", "a=b&c=d" },
        { "&a=b&&c=d&", "a=b&c=d" },
        { "a", "a=" },
        { "a=", "a=" },
        { "=b", "=b" },
        { "a=b&a=c", "a=b&a=c" },
        { "a+b=c+d", "a+b=c+d" },
        { "a=b%20c", "a=b+c" },
        { "%FE%FF", "%EF%BF%BD%EF%BF%BD=" },     /* invalid UTF-8 -> U+FFFD */
        { "%C2%A3=x", "%C2%A3=x" },
        { "a=b&c=d&e", "a=b&c=d&e=" },
        { "a%3Db=c", "a%3Db=c" },
        { "&&&&", "" },
        { "=", "=" },
        { "a=b#c", "a=b%23c" },
        { "~a=~b", "%7Ea=%7Eb" },   /* the urlencoded serializer keeps only *-._ and alnum */
        { "!'()=!'()", "%21%27%28%29=%21%27%28%29" },
    };
    for (unsigned i = 0; i < sizeof(round) / sizeof(round[0]); i++) {
        char id[64];
        snprintf(id, sizeof(id), "usp:round:%u", i);
        usp_parse(&l, round[i].in, -1);
        char *s = usp_serialize(&l);
        expect_str(id, s, round[i].out);
        free(s);
        tot++;
    }

    /* the '+' in a NAME is a space, and a literal '+' must survive the trip */
    usp_parse(&l, "a+b=c", -1);
    expect_str("usp:plus:name", l.n == 1 ? l.v[0].name : "(none)", "a b");
    usp_parse(&l, "a%2Bb=c", -1);
    expect_str("usp:plus:literal", l.n == 1 ? l.v[0].name : "(none)", "a+b");
    tot += 2;

    /* sort is stable and orders by UTF-16 code units -- an astral character
     * sorts BELOW U+FFFD because its lead surrogate does. */
    usp_parse(&l, "\xEF\xBF\xBD=x&\xF0\x9F\x8C\x88=y&a=1&a=0", -1);
    usp_sort(&l);
    {
        char *s = usp_serialize(&l);
        expect_str("usp:sort:astral", s, "a=1&a=0&%F0%9F%8C%88=y&%EF%BF%BD=x");
        free(s);
        tot++;
    }
    usp_parse(&l, "z=1&a=2&z=3&a=4", -1);
    usp_sort(&l);
    { char *s = usp_serialize(&l); expect_str("usp:sort:stable", s, "a=2&a=4&z=1&z=3"); free(s); tot++; }

    usp_clear(&l);
    free(l.v);
    printf("  URLSearchParams  %4d/%-4d  (host-side; the live URL link is JS-visible)\n",
           g_pass - ok0, tot);
}

/* ===================================================================== *
 *  percent-encoding sets -- invisible on an ordinary URL, wrong on the corpus
 * ===================================================================== */

static void run_pct(void)
{
    int tot = 0, ok0 = g_pass;
    struct { const char *in; int set; const char *out; } t[] = {
        { "a\x1F" "b~", PCT_C0, "a%1Fb~" },
        { " \"<>`", PCT_FRAGMENT, "%20%22%3C%3E%60" },
        { " \"#<>'", PCT_QUERY, "%20%22%23%3C%3E'" },
        { " \"#<>'", PCT_SPECIAL_QUERY, "%20%22%23%3C%3E%27" },
        { "?`{}", PCT_PATH, "%3F%60%7B%7D" },
        { "/:;=@[\\]^|", PCT_USERINFO, "%2F%3A%3B%3D%40%5B%5C%5D%5E%7C" },
        { "$%&+,", PCT_COMPONENT, "%24%25%26%2B%2C" },
        { "!'()~", PCT_FORM, "%21%27%28%29%7E" },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char id[64];
        snprintf(id, sizeof(id), "pct:%u", i);
        char *e = url_percent_encode(t[i].in, -1, t[i].set);
        expect_str(id, e, t[i].out);
        free(e);
        tot++;
    }
    /* the query set DIFFERS between a special and a non-special scheme, which
     * is the one that is invisible until the corpus asks */
    urlrec *a = url_parse_w("http://x/?'", -1, NULL);
    urlrec *b = url_parse_w("nonspecial://x/?'", -1, NULL);
    { char *s = url_get(a, URLC_SEARCH); expect_str("pct:query:special", s, "?%27"); free(s); tot++; }
    { char *s = url_get(b, URLC_SEARCH); expect_str("pct:query:nonspecial", s, "?'"); free(s); tot++; }
    url_free_w(a); url_free_w(b);
    printf("  encoding sets    %4d/%-4d\n", g_pass - ok0, tot);
}

int main(int argc, char **argv)
{
    const char *root = getenv("WPT_ROOT");
    if (!root) root = "third_party/wpt";
    const char *expect = getenv("URL_EXPECT");
    if (!expect) expect = "tests/unit/url_expected_fail.txt";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-v", 2)) g_verbose = argv[i][2] ? atoi(argv[i] + 2) : 20;
        else if (!strcmp(argv[i], "-n")) g_verbose = 2;
    }
    load_expect(expect);

    printf("url_test: the WHATWG URL Standard against the corpus's own data\n");
    run_constructor(root);
    run_setters(root);
    run_usp();
    run_pct();
    run_idna(root);

    int total = g_pass + g_fail;
    printf("  ------------------------------------------------------------\n");
    printf("  gated total      %4d/%-4d  (%.1f%%)   unexpected failures: %d",
           g_pass, total, total ? 100.0 * g_pass / total : 0.0, g_unexpected);
    if (g_newpass) printf(", newly passing: %d (re-cut the list)", g_newpass);
    printf("\n");
    if (g_unexpected) {
        printf("url_test: FAILED -- %d case(s) not in %s\n", g_unexpected, expect);
        return 1;
    }
    printf("url_test: ok\n");
    (void)g_seen_pass;
    return 0;
}
