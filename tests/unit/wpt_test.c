/* tests/unit/wpt_test.c -- run Web Platform Tests against our DOM and Web-API
 * layer and report a pass rate.
 *
 *     make test-wpt              per-subset counts + the total
 *     make test-wpt V=20         also dump the first 20 unexpected failures
 *     make test-wpt STRICT=1     exit non-zero on a regression
 *
 * WHY THIS EXISTS. The HTML parser had html5lib-tests and became a spec
 * implementation with a number attached. The DOM and the Web APIs had nothing:
 * every defect in them was found by loading a real site and reading a stack,
 * which is unbounded (sites are infinite) and unordered (fixing one does not
 * predict the next). A corpus turns "some site is broken somewhere" into "these
 * N behaviours are wrong, ranked".
 *
 * WPT tests are HTML files that load /resources/testharness.js and report
 * through it. This runner is the wptserve+wptrunner half, in ~700 lines and
 * host-side:
 *
 *   - a FILE RESOLVER that maps the server paths a test uses ("/resources/
 *     testharness.js", "support/foo.js", "/common/get-host-info.sub.js") onto
 *     the vendored tree. No network, no server.
 *   - a WRAPPER GENERATOR for the .any.js / .window.js formats, which upstream
 *     only exist as generated .html files served by wptserve. The `// META:`
 *     header lines (script=, title=, global=) are honoured.
 *   - a COLLECTOR: testharness.js's own add_completion_callback, so the
 *     subtest names and messages come from the harness, not from parsing text.
 *
 * SUBTESTS ARE THE UNIT, not files. A WPT file holds anywhere from 1 to 400
 * test() calls; counting files would make "Node.prototype exists" worth as much
 * as 300 range assertions, and would hide the exact thing the ranking is for.
 * A file that throws before the harness completes is reported as one HARNESS
 * failure with its exception, which is a different and more serious category.
 *
 * ------------------------------------------------------------------------
 * THE RATCHET
 * ------------------------------------------------------------------------
 * Identical in shape to tests/unit/html5lib_expected_fail.txt, deliberately:
 * tests/unit/wpt_expected_fail.txt lists every subtest that fails today as
 * "<test path>::<subtest name>". The runner diffs against it and reports NEW
 * FAILURES and NEWLY PASSING separately, so a change that fixes twelve and
 * breaks one reads as exactly that instead of as "+11". --strict exits
 * non-zero when there are new failures.
 *
 * ------------------------------------------------------------------------
 * THE CORPUS IS NOT REQUIRED TO BE PRESENT
 * ------------------------------------------------------------------------
 * WPT_ROOT (argv[1], or $WPT_ROOT) can point at any checkout. If the directory
 * is absent the runner says so and exits 0 -- deleting the data must not delete
 * the capability, and a build that cannot find a corpus is not a regression in
 * the code under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }

/* layout.c is linked (see tests/wpt.mk for why that was a decision and not an
 * accident), and it asks the embedder for two things the browser answers with
 * syscalls: how wide a run of text is, and the bytes of a subresource.
 *
 * text_measure is a MONOSPACE APPROXIMATION here, and that is a stated limit
 * rather than a bug: the real one rasterises a TrueType face through
 * SYS_TEXT_MEASURE, which a host process cannot call. Every geometry assertion
 * in the corpus that depends on real glyph advances is therefore measuring this
 * approximation -- but every geometry assertion that depends on the box model,
 * which is nearly all of them, is measuring layout.c. The alternative was
 * linking no layout at all, and then every one of them fails on a 0x0 box.
 *
 * res_fetch returns -1: a subresource is not on the disk under a corpus path in
 * any form layout can use, and an image that fails to load has defined layout
 * behaviour (its intrinsic size is 0 unless width/height say otherwise), so
 * this is a real answer rather than a stub that lies. */
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *u, unsigned char **b, int *l)
{ (void)u; (void)b; (void)l; return -1; }

/* js_module.c -- the REAL module loader -- is in this link rather than
 * reimplemented, so `<script type=module>` and dynamic import are resolved and
 * linked by the code the browser ships. All it needs under it is a fetch, and
 * here that fetch is the corpus on disk. Defined further down, next to the path
 * resolver they share. */
int bfetch_resolve(const char *base, const char *ref, char *out, int max);
int bfetch_sync(const char *ref, unsigned char **out, int *outlen);

/* ------------------------------------------------------------ options -- */
static const char *g_root;          /* WPT checkout root */
static int   g_verbose, g_vmax = 10, g_writebl, g_strict, g_listonly, g_dump, g_progress, g_no_lifecycle;
static const char *g_blpath = "tests/unit/wpt_expected_fail.txt";
static const char *g_only;          /* substring filter on the test path */
/* The ranked-cause report needs the MESSAGE, which the baseline deliberately
 * does not carry (a baseline entry has to be stable across a message reword).
 * --report writes a TSV of every result with its message for tools/wpt_rank.py. */
static const char *g_report;
static FILE *g_repf;

/* ------------------------------------------------------------ helpers -- */
static char *xread(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return 0; }
    b[n] = 0; fclose(f);
    if (len) *len = n;
    return b;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A growable byte buffer -- the concatenated script source for one test. */
struct buf { char *p; size_t n, cap; };
static void bput(struct buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        while (b->n + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 65536;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bputs(struct buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bfree(struct buf *b) { free(b->p); b->p = 0; b->n = b->cap = 0; }

/* ------------------------------------------------------- path resolve -- */
/* Map a URL as it appears in a test onto a file in the checkout.
 *   "/resources/testharness.js"  -> <root>/resources/testharness.js
 *   "support/x.js"               -> <dir-of-test>/support/x.js
 *   "../resources/y.js"          -> normalised against the test's directory
 * Query strings and fragments are dropped: wptserve serves the same file. */
static void norm_path(char *p)
{
    /* collapse "a/./b" and "a/b/../c" in place */
    char *out = p, *seg = p;
    char *segs[64]; int ns = 0;
    for (;;) {
        char *slash = strchr(seg, '/');
        size_t l = slash ? (size_t)(slash - seg) : strlen(seg);
        if (l == 1 && seg[0] == '.') { /* skip */ }
        else if (l == 2 && seg[0] == '.' && seg[1] == '.') { if (ns) ns--; }
        else if (l || ns == 0) { if (ns < 64) { segs[ns] = seg; segs[ns] = seg; ns++; }
                                 segs[ns - 1] = seg; }
        if (!slash) break;
        *slash = 0;
        seg = slash + 1;
    }
    out = p;
    for (int i = 0; i < ns; i++) {
        size_t l = strlen(segs[i]);
        if (i) *out++ = '/';
        memmove(out, segs[i], l);
        out += l;
    }
    *out = 0;
}

static char *resolve_url(const char *url, const char *testdir, char *out, size_t n)
{
    char tmp[1024];
    /* strip query/fragment */
    size_t l = strcspn(url, "?#");
    if (l >= sizeof tmp) return 0;
    memcpy(tmp, url, l); tmp[l] = 0;
    if (!tmp[0]) return 0;
    if (strstr(tmp, "://")) return 0;            /* absolute: no network here */
    if (tmp[0] == '/') snprintf(out, n, "%s%s", g_root, tmp);
    else               snprintf(out, n, "%s/%s", testdir, tmp);
    norm_path(out);
    return out;
}

/* The module loader's fetch, served from the checkout. `g_testdir` is the
 * directory of the file being run; a module specifier is resolved against it
 * exactly as a relative URL would be, and a "/..." one against the root. */
static const char *g_testdir = "";

int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{
    /* Strip the synthetic http://web-platform.test:8000 origin off `base` so
     * the same code path resolves both a URL and a bare path. */
    const char *b = base ? base : "";
    const char *p = strstr(b, "://");
    if (p) { p = strchr(p + 3, '/'); b = p ? p : "/"; }
    char dir[1024];
    if (ref && ref[0] == '/') snprintf(dir, sizeof dir, "%s%s", g_root, ref);
    else {
        char bd[1024];
        snprintf(bd, sizeof bd, "%s", b[0] ? b : "/");
        char *s = strrchr(bd, '/');
        if (s) s[1] = 0; else bd[0] = 0;
        if (bd[0] == '/') snprintf(dir, sizeof dir, "%s%s%s", g_root, bd, ref ? ref : "");
        else              snprintf(dir, sizeof dir, "%s/%s", g_testdir, ref ? ref : "");
    }
    size_t q = strcspn(dir, "?#");
    dir[q] = 0;
    norm_path(dir);
    snprintf(out, (size_t)max, "%s", dir);
    return 0;
}

int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{
    char path[1024];
    bfetch_resolve(0, ref, path, sizeof path);
    long n = 0;
    char *s = xread(path, &n);
    if (!s) return -1;
    *out = (unsigned char *)s;
    *outlen = (int)n;
    return 0;
}

/* --------------------------------------------------- script extraction -- */
/* Walk the parsed document in tree order and append every <script>'s source.
 * A src= script is read off disk; a missing one is recorded as a comment so a
 * failure caused by an unfetchable dependency is visible in the source dump
 * rather than as an unexplained ReferenceError. */
/* Scripts already in the stream, by resolved path. testharness.js is seeded
 * into this set because the runner loads it itself before anything else, and
 * every WPT file also asks for it by <script src>.
 *
 * That double-load is not a cosmetic duplicate. testharness.js is an IIFE that
 * builds a fresh `Tests` object and rebinds `test`, `async_test` and
 * `add_completion_callback` to it. Running it twice leaves the runner's
 * completion callback attached to the FIRST instance while every test() the
 * page makes goes to the second -- so the file reports 0 subtests and a
 * TIMEOUT, with no exception anywhere to explain it. That is exactly what this
 * runner did until this set existed. */
struct incl { char **p; int n, cap; };
static struct incl g_incl;
static int incl_seen(const char *path)
{
    for (int i = 0; i < g_incl.n; i++) if (!strcmp(g_incl.p[i], path)) return 1;
    if (g_incl.n == g_incl.cap) {
        g_incl.cap = g_incl.cap ? g_incl.cap * 2 : 32;
        g_incl.p = realloc(g_incl.p, (size_t)g_incl.cap * sizeof *g_incl.p);
    }
    g_incl.p[g_incl.n++] = strdup(path);
    return 0;
}
static void incl_reset(void)
{
    for (int i = 0; i < g_incl.n; i++) free(g_incl.p[i]);
    g_incl.n = 0;
}

/* ONE PROGRAM PER <script>, not one concatenated blob.
 *
 * This runner started out concatenating every script in the document into a
 * single JS_Eval. That is wrong in the way that matters most here: a browser
 * runs each <script> as its own program, so a SyntaxError or an early throw in
 * one kills that script and nothing else. Concatenated, one bad script takes
 * the whole file down, and the file reports as "harness never completed" with
 * no subtests -- which is indistinguishable from a DOM that cannot do anything.
 * 185 of 304 harness failures in the first dom/ run were that artefact.
 *
 * Keeping them separate also makes the line numbers in an exception's stack
 * refer to the SCRIPT the corpus actually ships, so a failure names a file and
 * a line somebody can open. */
struct script { char *name; char *src; long len; int module; };
struct sctx {
    struct script *s; int n, cap;
    const char *testdir; int missing; int modules;
    /* The file asked for /resources/testdriver.js: it drives the page with
     * SYNTHETIC input -- click, touch, wheel, scroll, key -- injected by the
     * test runner, not by anything in the page. Nothing here can send those, so
     * such a file registers its tests and then waits forever, or registers none
     * at all. Recorded rather than inferred, because it is the one signal that
     * says "unreachable by implementing a Web API" without any guessing. */
    int needs_driver;
};

static void sc_add(struct sctx *c, const char *name, char *src, long len, int module)
{
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 16;
                          c->s = realloc(c->s, (size_t)c->cap * sizeof *c->s); }
    c->s[c->n].name = strdup(name);
    c->s[c->n].src = src;
    c->s[c->n].len = len;
    c->s[c->n].module = module;
    c->n++;
}

static void collect_scripts(struct node *n, struct sctx *c)
{
    if (!n) return;
    if (n->type == N_ELEM && n->tag_id == TAG_SCRIPT) {
        const char *type = dom_attr(n, "type");
        const char *src  = dom_attr(n, "src");
        /* A type that is not a JavaScript MIME type is a DATA BLOCK, not a
         * script -- WPT uses <script type="text/plain"> and friends as fixture
         * payloads, and evaluating one is how a runner invents failures a
         * browser would never see. */
        int module = type && !strcasecmp(type, "module");
        int classic = !type || !*type ||
                      !strcasecmp(type, "text/javascript") ||
                      !strcasecmp(type, "application/javascript");
        if (module) c->modules++;
        if (classic || module) {
            if (src && *src) {
                char path[1024];
                if (strstr(src, "testdriver")) c->needs_driver = 1;
                if (resolve_url(src, c->testdir, path, sizeof path)) {
                    if (incl_seen(path)) goto kids;
                    long len = 0;
                    char *s = xread(path, &len);
                    if (s) sc_add(c, src, s, len, module);
                    else c->missing++;
                } else { c->missing++; }
            } else {
                for (struct node *t = n->first_child; t; t = t->next)
                    if (t->type == N_TEXT && t->text) {
                        char nm[64];
                        snprintf(nm, sizeof nm, "<inline %d>", c->n);
                        char *s = malloc((size_t)t->textlen + 1);
                        memcpy(s, t->text, (size_t)t->textlen);
                        s[t->textlen] = 0;
                        sc_add(c, nm, s, t->textlen, module);
                    }
            }
        }
    }
kids:
    for (struct node *k = n->first_child; k; k = k->next) collect_scripts(k, c);
}

/* --------------------------------------------------- .any.js wrappers --- */
/* Upstream ships bare .js for these; wptserve generates the HTML. `// META:`
 * lines at the top of the file name extra scripts to load first. */
static void meta_scripts(const char *src, struct sctx *c)
{
    const char *p = src;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (l < 2 || p[0] != '/' || p[1] != '/') break;      /* header ends */
        const char *m = "// META: script=";
        size_t ml = strlen(m);
        if (l > ml && !strncmp(p, m, ml)) {
            if (strstr(p, "testdriver")) c->needs_driver = 1;
            char url[512];
            size_t ul = l - ml;
            if (ul < sizeof url) {
                memcpy(url, p + ml, ul); url[ul] = 0;
                while (ul && (url[ul-1] == '\r' || url[ul-1] == ' ')) url[--ul] = 0;
                char path[1024];
                if (resolve_url(url, c->testdir, path, sizeof path) && !incl_seen(path)) {
                    long n = 0; char *s = xread(path, &n);
                    if (s) sc_add(c, url, s, n, 0); else c->missing++;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

/* ------------------------------------------------------------ results -- */
/* The STACK is carried alongside the message because the message alone is not
 * a work order. QuickJS reports a failed call as "not a function" with no
 * callee name, and 586 subtests in one dom/ run collapsed into that one
 * useless bucket; the stack's top frame names the script and the line, which
 * is a thing somebody can open. */
struct res { char **id; char **msg; char **stack; int *status; int n, cap; };

static void res_add(struct res *r, const char *id, const char *msg,
                    const char *stack, int status)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 256;
        r->id = realloc(r->id, (size_t)r->cap * sizeof *r->id);
        r->msg = realloc(r->msg, (size_t)r->cap * sizeof *r->msg);
        r->stack = realloc(r->stack, (size_t)r->cap * sizeof *r->stack);
        r->status = realloc(r->status, (size_t)r->cap * sizeof *r->status);
    }
    r->id[r->n] = strdup(id);
    r->msg[r->n] = strdup(msg ? msg : "");
    r->stack[r->n] = strdup(stack ? stack : "");
    r->status[r->n] = status;
    r->n++;
}

/* -------------------------------------------------------- the baseline -- */
struct baseline { char **id; int n, cap; };
static void bl_add(struct baseline *b, const char *id)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 512;
        b->id = realloc(b->id, (size_t)b->cap * sizeof *b->id);
    }
    b->id[b->n++] = strdup(id);
}
/* The baseline can hold thousands of entries and every subtest asks it a
 * question, so a linear scan is O(n*m) and shows up as seconds. Hash it. */
#define BLH 8192
struct blhash { struct baseline *b; int *head, *next; };
static unsigned hstr(const char *s)
{ unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static void blh_build(struct blhash *h, struct baseline *b)
{
    h->b = b;
    h->head = malloc(BLH * sizeof(int));
    for (int i = 0; i < BLH; i++) h->head[i] = -1;
    h->next = malloc((size_t)(b->n ? b->n : 1) * sizeof(int));
    for (int i = 0; i < b->n; i++) {
        unsigned k = hstr(b->id[i]) & (BLH - 1);
        h->next[i] = h->head[k]; h->head[k] = i;
    }
}
static int blh_exact(const struct blhash *h, const char *id)
{
    for (int i = h->head[hstr(id) & (BLH - 1)]; i >= 0; i = h->next[i])
        if (!strcmp(h->b->id[i], id)) return 1;
    return 0;
}

/* "<path>::*" -- every subtest in this file fails today.
 *
 * html5lib's baseline lists every failing case by name and that is right for a
 * 1818-case corpus. This one has 148,269 failing subtests: written out one per
 * line it is a 16 MB file, which is not a thing anyone diffs, and it is 5 MB of
 * churn for a change that fixes one method. The wildcard collapses only the
 * files where NOTHING passes -- where a per-subtest list carries no information
 * a single line does not -- and keeps the exact per-subtest entries for every
 * file with a mixed result, which is where the ratchet actually earns its
 * keep. A file going from all-red to mixed shows up as the wildcard being
 * replaced by real names, which is exactly the event worth seeing. */
static int blh_has(const struct blhash *h, const char *id)
{
    if (blh_exact(h, id)) return 1;
    const char *sep = strstr(id, "::");
    if (!sep) return 0;
    char wild[2048];
    size_t n = (size_t)(sep - id);
    if (n + 4 > sizeof wild) return 0;
    memcpy(wild, id, n);
    memcpy(wild + n, "::*", 4);
    return blh_exact(h, wild);
}
static void blh_free(struct blhash *h) { free(h->head); free(h->next); }

/* A baseline entry is one LINE, and a WPT subtest name is not required to be
 * one line. css/css-syntax and css/css-values name subtests after the
 * declaration block under test, newlines and all:
 *
 *     "@media (min-width: 1px) {\n  div { color: red }\n} should be valid"
 *
 * Written raw, that entry becomes three lines, none of which is a valid key.
 * On the next run the real entry reads as a NEW FAILURE and the orphan
 * fragments read as NEWLY PASSING -- the ratchet failing in both directions at
 * once, which is worse than no ratchet because it is a ratchet that lies. 206
 * entries in the first baseline were exactly this.
 *
 * So the name is escaped on the way out and unescaped on the way in, and
 * --write-baseline asserts the round trip: it re-reads the file it just wrote
 * and requires the entry count to equal the failure count it reported. The
 * escaping is only how that assertion is passed. */
static void bl_escape(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:   fputc(*s, f);     break;
        }
    }
}

static void bl_unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p != '\\') { *o++ = *p; continue; }
        switch (*++p) {
        case 'n':  *o++ = '\n'; break;
        case 'r':  *o++ = '\r'; break;
        case 't':  *o++ = '\t'; break;
        case '\\': *o++ = '\\'; break;
        case 0:    *o++ = '\\'; p--; break;
        default:   *o++ = '\\'; *o++ = *p; break;
        }
    }
    *o = 0;
}

static void bl_load(struct baseline *b, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    /* Subtest names are long: WPT's reflection suite names a case after the
     * value it set, and css/css-values after a whole declaration. 4 KiB
     * truncated some of them, and a truncated key never matches either. */
    static char line[65536];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#') continue;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!*s) continue;
        bl_unescape(s);
        bl_add(b, s);
    }
    fclose(f);
}

/* ------------------------------------------------- the runner harness --- */
static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now; }

static long long g_interrupt_budget;
static long long g_budget = 10000;
static int interrupt_cb(JSRuntime *rt, void *opaque)
{ (void)rt; (void)opaque; return --g_interrupt_budget <= 0; }

/* The prologue installed BEFORE testharness.js. It supplies the two things the
 * harness reads out of its environment that a non-browser embedder has to
 * provide, and nothing else -- every gap that is not one of these is a real
 * finding and must stay visible.
 *
 *   __wpt_results   where the completion callback deposits the verdict
 *   __wpt_hookup    run after testharness.js loads: registers the callback and
 *                   turns off the harness's own timeout (we drive the clock)
 */
static const char *PROLOGUE =
"globalThis.__wpt = { results: [], status: -1, message: '', done: false };\n";

static const char *HOOKUP =
"(function(){\n"
"  if (typeof add_completion_callback !== 'function') { __wpt.harness_missing = 1; return; }\n"
/* OUTPUT OFF, which is what wptrunner does through testharnessreport.js.
 * testharness.js otherwise renders every result into a #log table when it
 * completes, from a completion callback it registered before ours. On this DOM
 * that renderer throws ("not a function"), and testharness's forEach lets the
 * exception escape -- so OUR callback, next in the list, never runs and the
 * file reports zero subtests. dom/nodes/Element-classlist.html went from 1420
 * subtests to 0 for exactly that reason, and it looked like the load event
 * being wrong rather than the reporter being in the way. */
"  try { setup({ output: false }); } catch (e) { __wpt.setup_error = String(e); }\n"
"  add_completion_callback(function(tests, status){\n"
"    __wpt.done = true;\n"
"    __wpt.status = status ? status.status : -1;\n"
"    __wpt.message = status && status.message ? String(status.message) : '';\n"
"    for (var i = 0; i < tests.length; i++) {\n"
"      __wpt.results.push([String(tests[i].name), tests[i].status,\n"
"                          tests[i].message == null ? '' : String(tests[i].message),\n"
"                          tests[i].stack == null ? '' : String(tests[i].stack)]);\n"
"    }\n"
"  });\n"
"})();\n";

/* Run one test file. Returns 1 if the harness completed, 0 if it never did
 * (which is itself the result: a HARNESS failure). */
/* A file that produced no results did so for one of TWO reasons, and they are
 * not the same finding:
 *
 *   DIED         it threw -- a missing global, a missing method, an abort
 *                partway. Fixable by implementing something, and the ranking
 *                should send someone at it.
 *   NEVER STARTED it loaded cleanly, registered zero test() calls, and is
 *                waiting for an input event nobody will send. These need
 *                testdriver.js -- synthetic click, touch, wheel, scroll -- which
 *                this runner does not provide. They are a missing capability in
 *                the HARNESS, not a defect in the browser, and no amount of DOM
 *                or CSS work moves them.
 *
 * Measured in dom/events: 64 of 70 zero-result files are the second kind. Rolled
 * together they would put a large, immovable number at the top of a work order
 * and send people at something that cannot move. They also must not share a
 * ratchet token, because a category-2 entry can never be "fixed" by the lines
 * reading the list.
 *
 * `threw` is the discriminator and it is mechanical: any uncaught exception in
 * any of the file's programs sets it. `needs_driver` is the stronger signal
 * where it is available -- the file asked for /resources/testdriver.js by name. */
struct outcome {
    int completed, harness_missing, threw, needs_driver, onload_attr;
    char why[512], stack[512];
};

/* Non-NULL replaces the file's bytes: the self-check runs an in-memory case
 * through this exact path so the thing it proves is the runner, not a fixture
 * that happens to sit beside it. */
static const char *g_override;

static void run_one(const char *relpath, struct res *out, struct outcome *oc)
{
    memset(oc, 0, sizeof *oc);

    char full[1024];
    snprintf(full, sizeof full, "%s/%s", g_root, relpath);
    char testdir[1024];
    snprintf(testdir, sizeof testdir, "%s", full);
    char *slash = strrchr(testdir, '/');
    if (slash) *slash = 0;

    g_testdir = testdir;
    long flen = 0;
    char *fsrc = g_override ? strdup(g_override) : xread(full, &flen);
    if (g_override) flen = (long)strlen(g_override);
    if (!fsrc) { snprintf(oc->why, sizeof oc->why, "cannot read %s", full); return; }

    /* The harness itself, always first and as its own program. */
    char hpath[1024];
    snprintf(hpath, sizeof hpath, "%s/resources/testharness.js", g_root);
    long hl = 0; char *h = xread(hpath, &hl);
    if (!h) { snprintf(oc->why, sizeof oc->why, "no %s", hpath); free(fsrc); return; }
    incl_reset();
    incl_seen(hpath);          /* the page's own <script src> for it is a no-op */
    char rpath[1024];
    snprintf(rpath, sizeof rpath, "%s/resources/testharnessreport.js", g_root);
    incl_seen(rpath);          /* the wptrunner reporter: needs a wptrunner */

    struct node *root = 0;
    struct sctx c; memset(&c, 0, sizeof c);
    c.testdir = testdir;
    size_t l = strlen(relpath);
    int isjs = (l > 3 && !strcmp(relpath + l - 3, ".js"));

    if (isjs) {
        /* .any.js / .window.js: a synthetic document, then META scripts, then
         * the file. The document is the one wptserve's template produces. */
        static const char *DOC =
            "<!doctype html><meta charset=utf-8><title>wpt</title><div id=log></div>";
        root = dom_parse(DOC, (int)strlen(DOC));
        meta_scripts(fsrc, &c);
        sc_add(&c, relpath, fsrc, flen, 0);       /* takes ownership of fsrc */
        fsrc = 0;
    } else {
        root = dom_parse(fsrc, (int)flen);
        if (!root) { snprintf(oc->why, sizeof oc->why, "dom_parse returned NULL");
                     free(fsrc); free(h); return; }
        collect_scripts(root, &c);
        free(fsrc); fsrc = 0;
    }
    if (!root) { snprintf(oc->why, sizeof oc->why, "no document"); free(h); return; }

    g_now = 0;
    js_page_set_clock(clock_fn);
    char url[1200];
    snprintf(url, sizeof url, "http://web-platform.test:8000/%s", relpath);
    js_page_set_location(url);
    if (!js_page_open(root)) {
        snprintf(oc->why, sizeof oc->why, "js_page_open failed");
        dom_free(root); free(h); return;
    }
    JSContext *ctx = js_page_ctx();

    /* A watchdog, because a conformance corpus contains loops that a
     * non-conformant engine never leaves. dom/events/Event-timestamp-safe-
     * resolution.html spins until performance.now() advances past a bound, and
     * this runner drives a VIRTUAL clock that only moves between turns -- so
     * that loop is infinite here by construction, and with no interrupt handler
     * the whole suite hangs on one file and the runner looks broken rather than
     * the browser. Tripping the budget produces "InternalError: interrupted",
     * which is a visible, groupable cause instead of a hang.
     *
     * The unit is HANDLER CALLS, not opcodes: QuickJS polls interrupts once per
     * JS_INTERRUPT_COUNTER_INIT (10000) opcodes, so this is ~100M opcodes --
     * two orders of magnitude more than the heaviest honest test here needs,
     * and about a second when something really is looping. Getting this wrong
     * in the other direction is what a 40-million budget did: 400 billion
     * opcodes, i.e. never. */
    g_interrupt_budget = g_budget;
    JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_cb, 0);

#ifdef WPT_NEGCTL
    /* THE NEGATIVE CONTROL. An assertion nobody has watched fail is not a
     * known-failing assertion, and the assertion this whole file exists to
     * make is "the ratchet goes red when a DOM capability that works today
     * stops working". Nothing in a passing run demonstrates that.
     *
     * So: remove ONE method the corpus measures -- document.createElement,
     * which exists and works today -- and require `--strict` to exit non-zero
     * and to name the subtests that regressed. It is deliberately a single
     * named method rather than a wholesale break: a control that removes half
     * the DOM would go red even if the diff logic were wrong, and would prove
     * nothing about the ratchet.
     *
     * This lives in the RUNNER, not in js_dom.c, on purpose. The instrument
     * and the repairs are separable, and a control that requires editing the
     * thing under test cannot be run before the first fix exists. */
    {
        static const char *NEG =
            "delete document.createElement;"
            "if (typeof document.createElement === 'function')"
            "  document.createElement = undefined;"
            "typeof document.createElement;";
        JSValue nv = JS_Eval(ctx, NEG, strlen(NEG), "<negctl>", JS_EVAL_TYPE_GLOBAL);
        if (g_dump) {
            const char *t = JS_ToCString(ctx, nv);
            printf("  [negctl] document.createElement is now %s\n", t ? t : "?");
            if (t) JS_FreeCString(ctx, t);
        }
        JS_FreeValue(ctx, nv);
    }
#endif

    /* Programs, in document order: the prologue, testharness.js, the hookup,
     * then each <script>. One JS_Eval each, exactly as a browser runs them --
     * so a throw in one script leaves the rest of the file running, and the
     * exception's stack refers to the script the corpus ships rather than to a
     * line in a 400 KiB concatenation. */
#define EVAL(SRC, LEN, NAME) do {                                              \
        JSValue v_ = JS_Eval(ctx, (SRC), (size_t)(LEN), (NAME), JS_EVAL_TYPE_GLOBAL); \
        if (JS_IsException(v_)) {                                              \
            oc->threw = 1;                                                     \
            JSValue e_ = JS_GetException(ctx);                                 \
            const char *m_ = JS_ToCString(ctx, e_);                            \
            if (!oc->why[0])                                                   \
                snprintf(oc->why, sizeof oc->why, "uncaught in %s: %s",        \
                         (NAME), m_ ? m_ : "?");                               \
            if (m_) JS_FreeCString(ctx, m_);                                   \
            JSValue st_ = JS_GetPropertyStr(ctx, e_, "stack");                 \
            const char *s_ = JS_IsUndefined(st_) ? 0 : JS_ToCString(ctx, st_); \
            if (s_) {                                                          \
                if (!oc->stack[0]) snprintf(oc->stack, sizeof oc->stack, "%s", s_); \
                if (g_verbose) printf("      stack: %.300s\n", s_);            \
                JS_FreeCString(ctx, s_);                                       \
            }                                                                  \
            JS_FreeValue(ctx, st_);                                            \
            JS_FreeValue(ctx, e_);                                             \
        }                                                                      \
        JS_FreeValue(ctx, v_);                                                 \
    } while (0)

    EVAL(PROLOGUE, strlen(PROLOGUE), "<wpt prologue>");
    EVAL(h, hl, "/resources/testharness.js");
    free(h);
    EVAL(HOOKUP, strlen(HOOKUP), "<wpt hookup>");
    for (int i = 0; i < c.n; i++) {
        if (c.s[i].module) {
            /* A module is its own program with its own scope, and `import` is a
             * syntax error outside one -- evaluating a <script type=module> as
             * a classic script produced "SyntaxError: expecting '('" and killed
             * the file. js_module.c (the browser's real loader) is in this link
             * and resolves specifiers through bfetch_sync above, so the module
             * graph is the one the browser would build. */
            JSValue m = JS_Eval(ctx, c.s[i].src, (size_t)c.s[i].len, c.s[i].name,
                                JS_EVAL_TYPE_MODULE);
            if (JS_IsException(m)) {
                oc->threw = 1;
                JSValue e = JS_GetException(ctx);
                const char *ms = JS_ToCString(ctx, e);
                if (!oc->why[0])
                    snprintf(oc->why, sizeof oc->why, "uncaught in module %s: %s",
                             c.s[i].name, ms ? ms : "?");
                if (ms) JS_FreeCString(ctx, ms);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, m);
        } else {
            js_page_begin_script(c.s[i].name);
            EVAL(c.s[i].src, c.s[i].len, c.s[i].name);
            js_page_end_script();
        }
        free(c.s[i].name); free(c.s[i].src);
    }
    free(c.s);
    oc->needs_driver = c.needs_driver;
    /* Does the document start its own tests from a `<body onload=...>` content
     * attribute? 846 of the 1153 files that register nothing do, and that is
     * NOT a missing harness capability -- per HTML, `onload` on <body> sets the
     * handler on WINDOW, and a browser that never compiles the attribute into a
     * function leaves those pages inert. Isolated and reproduced: the attribute
     * is in the DOM, `typeof window.onload` and `typeof document.body.onload`
     * are both "object". Distinguishing this from the testdriver files is the
     * difference between a work item and an impossibility. */
    {
        struct node *b = root && root->doc ? dom_doc_body(root->doc) : 0;
        const char *ol = b ? dom_attr(b, "onload") : 0;
        oc->onload_attr = ol && *ol;
    }
    if (c.missing && !oc->why[0])
        snprintf(oc->why, sizeof oc->why,
                 "%d referenced script(s) could not be resolved in the checkout",
                 c.missing);

    /* The lifecycle events, exactly as browser.c fires them after a page's
     * scripts have run. This is not optional decoration: testharness.js's
     * WindowTestEnvironment does not set `all_loaded` until the load event, and
     * Tests.all_done() will not complete without it -- so with no load event
     * EVERY file finishes only by its own 10-second timeout, reports TIMEOUT,
     * and the rate measures the runner instead of the browser. 483 files never
     * completed at all before this call existed. */
    if (!g_no_lifecycle) {
        /* Dispatched from JS, not through js_dom_dispatch(), and that is the
         * whole point: testharness.js registers its load hook with
         * `on_event(window, 'load', ...)`, i.e. on the GLOBAL, and
         * js_dom_dispatch delivers to the document. Firing on the document
         * changed nothing at all -- A/B'd, 81/308 both ways -- which is how
         * this was found. browser.c has the same shape and the same gap. */
        /* ONE DISPATCH PER EVENT. The first version called dispatchEvent AND
         * then target['on'+t] by hand, "in case" the on-property was not wired
         * to the listener list -- and it dispatched `load` at document and
         * again at globalThis. js_dom.c makes window and document the SAME
         * EventTarget, so the net effect was window.onload running FOUR times
         * and addEventListener('load') twice, against browser.c's single call.
         *
         * That inflates a rate in a way that is invisible in the percentage,
         * because numerator and denominator inflate together: css/css-align
         * went 3,308 -> 9,296 subtests on nothing but this. Worse, it forced
         * the shipping <body onload> implementation to carry a fired-already
         * guard on all 19 window handler types just to survive the harness --
         * and that guard makes a repeating handler like <body onscroll> fire
         * once for three scrolls, so a real page updates once and looks frozen.
         * A test harness that changes the product to accommodate its own bug is
         * the worst outcome available here.
         *
         * So: dispatchEvent only, and the on-property only as a FALLBACK when
         * the target has no dispatchEvent at all. The self-check counts. */
        static const char *LIFECYCLE =
            "(function(){\n"
            "  function fire(t, target, bub) {\n"
            "    try {\n"
            "      var e = new Event(t, { bubbles: !!bub });\n"
            "      if (target && typeof target.dispatchEvent === 'function') {\n"
            "        target.dispatchEvent(e);\n"
            "      } else {\n"
            "        var h = target ? target['on' + t] : null;\n"
            "        if (typeof h === 'function') h.call(target, e);\n"
            "      }\n"
            "    } catch (x) { __wpt.lifecycle_error = String(x); }\n"
            "  }\n"
            "  fire('DOMContentLoaded', globalThis.document, 1);\n"
            "  fire('readystatechange', globalThis.document, 0);\n"
            /* `load` goes to the GLOBAL only. Dispatching at document as well
             * would double it, because they are one EventTarget here. */
#ifdef WPT_DOUBLE_FIRE
            /* The control for the once-only assertion, kept rather than done by
             * hand: this restores exactly what the runner did before -- a second
             * dispatch at document, which is the same EventTarget as the global.
             * `make test-wpt-fire-negctl` requires the self-check to FAIL with
             * it. Without a control this assertion is one I merely believe. */
            "  fire('load', globalThis.document, 0);\n"
#endif
            "  fire('load', globalThis, 0);\n"
            "})();\n";
        EVAL(LIFECYCLE, strlen(LIFECYCLE), "<wpt lifecycle>");
        js_page_pump();
    }

    /* Drain: work first, THEN time.
     *
     * THE RULE, and getting it backwards was a systematic measurement error:
     * the virtual clock is only allowed to move when nothing else can make
     * progress. The first version jumped straight to the next TIMER deadline
     * every pass. A transfer in flight has no entry in that queue -- http1.c
     * moves 4096 bytes per js_webapi_pump() and schedules nothing -- so the
     * very first jump landed on testharness's own 10-second timeout and the
     * fetch lost the race after ONE pump. Observable and exact: 540 bytes
     * passed, 267 KB timed out, nothing else different.
     *
     * The bias that produces is the dangerous part. It is not noise: the
     * BIGGER a test's resources, the more likely it failed for a reason that
     * has nothing to do with this browser. url/url-constructor.any.js is one
     * red line in the report and 1,004 URL cases that never executed -- a URL
     * parser reading 21.7% because it was never asked.
     *
     * So: pump the network and run due timers; drain microtasks; and if
     * anything at all ran, go round again WITHOUT touching the clock. Only a
     * pass where nothing ran is allowed to advance it, and then only to the
     * next real deadline. That is also what a browser does -- the network is
     * concurrent with the clock, not scheduled by it. */
    js_page_pump();
    for (int i = 0; i < 200000; i++) {
        JSValue d = JS_Eval(ctx, "__wpt.done", 10, "<drain>", JS_EVAL_TYPE_GLOBAL);
        int done = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
        if (done) break;

        int ran = js_page_run_due();     /* steps in-flight fetches FIRST */
        ran += js_page_pump();
        if (ran) {
            if (g_interrupt_budget <= 0) break;
            continue;                    /* progress: the clock stays put */
        }
        if (!js_page_pending()) break;   /* nothing running, nothing scheduled */

        long long due = js_page_next_due();
        if (due > (long long)g_now) g_now = (unsigned long long)due;
        else g_now += 1;                 /* no timer, only a transfer: nudge */
        if (g_now > 30000) break;
        if (g_interrupt_budget <= 0) {           /* watchdog tripped: stop */
            if (!oc->why[0]) snprintf(oc->why, sizeof oc->why,
                                      "watchdog: script did not stop running");
            break;
        }
    }

    /* Pull the results out field by field: no JSON round trip, no escaping
     * questions, and the subtest name arrives as the bytes the harness holds. */
    if (g_dump) {
        static const char *DS =
            "(function(){try{return 'done='+__wpt.done+' status='+__wpt.status+"
            "' hm='+!!__wpt.harness_missing+' n='+__wpt.results.length+"
            "' msg='+__wpt.message+' probe='+__wpt.probe+' lferr='+__wpt.lifecycle_error;}catch(e){return 'no __wpt: '+e;}})()";
        JSValue s = JS_Eval(ctx, DS, strlen(DS), "<dump>", JS_EVAL_TYPE_GLOBAL);
        const char *m = JS_ToCString(ctx, s);
        printf("  [dump] %s  why=%s\n", m ? m : "?", oc->why[0] ? oc->why : "-");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, s);
        const char *o = js_page_output();
        if (o && *o) printf("  [console] %.1200s\n", o);
    }

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue w = JS_GetPropertyStr(ctx, g, "__wpt");
    if (JS_IsObject(w)) {
        JSValue hm = JS_GetPropertyStr(ctx, w, "harness_missing");
        oc->harness_missing = JS_ToBool(ctx, hm);
        JS_FreeValue(ctx, hm);
        JSValue dn = JS_GetPropertyStr(ctx, w, "done");
        oc->completed = JS_ToBool(ctx, dn);
        JS_FreeValue(ctx, dn);
        if (!oc->completed && !oc->why[0]) {
            JSValue st = JS_GetPropertyStr(ctx, w, "status");
            int32_t si = -1; JS_ToInt32(ctx, &si, st); JS_FreeValue(ctx, st);
            snprintf(oc->why, sizeof oc->why,
                     oc->harness_missing ? "testharness.js did not install"
                                         : "harness never completed");
        }
        JSValue arr = JS_GetPropertyStr(ctx, w, "results");
        if (JS_IsArray(ctx, arr)) {
            JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
            uint32_t n = 0; JS_ToUint32(ctx, &n, lenv); JS_FreeValue(ctx, lenv);
            for (uint32_t i = 0; i < n; i++) {
                JSValue row = JS_GetPropertyUint32(ctx, arr, i);
                JSValue nm = JS_GetPropertyUint32(ctx, row, 0);
                JSValue stv = JS_GetPropertyUint32(ctx, row, 1);
                JSValue msv = JS_GetPropertyUint32(ctx, row, 2);
                JSValue skv = JS_GetPropertyUint32(ctx, row, 3);
                const char *nm_s = JS_ToCString(ctx, nm);
                const char *ms_s = JS_ToCString(ctx, msv);
                const char *sk_s = JS_ToCString(ctx, skv);
                int32_t st = 0; JS_ToInt32(ctx, &st, stv);
                res_add(out, nm_s ? nm_s : "?", ms_s, sk_s, st);
                if (nm_s) JS_FreeCString(ctx, nm_s);
                if (ms_s) JS_FreeCString(ctx, ms_s);
                if (sk_s) JS_FreeCString(ctx, sk_s);
                JS_FreeValue(ctx, nm); JS_FreeValue(ctx, stv);
                JS_FreeValue(ctx, msv); JS_FreeValue(ctx, skv);
                JS_FreeValue(ctx, row);
            }
        }
        JS_FreeValue(ctx, arr);
    }
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, g);

    js_page_close();
    dom_free(root);
}

/* ------------------------------------------------ what kind of test is it -- */
/* WPT holds three kinds of file and only one of them can run here.
 *
 *   K_HARNESS  loads /resources/testharness.js and reports through it. This
 *              runner's whole subject.
 *   K_REFTEST  carries <link rel=match|mismatch>: it is judged by RENDERING it
 *              and comparing pixels against a reference document. There are
 *              17,023 of these in css/ alone and none of them can run without a
 *              reftest harness we do not have.
 *   K_NEITHER  a reference document, a manual test, a support page.
 *
 * Counting a reftest as a failure inflates the denominator until the rate means
 * nothing; counting one as a pass is worse. They are reported as NOT RUN, in
 * their own column, so the rate is over the files that were actually judged. */
enum { K_HARNESS, K_REFTEST, K_NEITHER };

static int classify(const char *relpath)
{
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", g_root, relpath);
    size_t l = strlen(relpath);
    if (l > 3 && !strcmp(relpath + l - 3, ".js")) return K_HARNESS;  /* .any.js etc */

    long n = 0;
    char *s = xread(full, &n);
    if (!s) return K_NEITHER;
    /* Look at the head only: the <link rel> and the harness <script> are both
     * in the document's first couple of KiB, and some of these files are large. */
    if (n > 8192) s[8192] = 0;
    int kind = K_NEITHER;
    if (strstr(s, "testharness.js")) kind = K_HARNESS;
    else if (strstr(s, "rel=\"match\"") || strstr(s, "rel='match'") ||
             strstr(s, "rel=match")     || strstr(s, "rel=\"mismatch\"") ||
             strstr(s, "rel='mismatch'")|| strstr(s, "rel=mismatch"))
        kind = K_REFTEST;
    free(s);
    return kind;
}

/* ------------------------------------------------------- test discovery -- */
/* A WPT test file is one whose name says so. Everything else in the tree is
 * support: helpers, .headers, .ini, resources, manual tests. */
static int is_test_file(const char *name)
{
    size_t l = strlen(name);
    if (strstr(name, "-manual.")) return 0;
    if (strstr(name, ".tentative.")) return 0;
    if (l > 8 && !strcmp(name + l - 8, ".any.js")) return 1;   /* unreachable, kept for clarity */
    if (l > 7 && !strcmp(name + l - 7, ".any.js")) return 1;
    if (l > 10 && !strcmp(name + l - 10, ".window.js")) return 1;
    if (l > 5 && !strcmp(name + l - 5, ".html")) {
        /* .any.html and friends are GENERATED by wptserve from the .js; if both
         * are present in a checkout the .js is the source of truth. Upstream
         * only ships the .js, so this is belt and braces. */
        if (strstr(name, ".any.")) return 0;
        if (strstr(name, ".window.")) return 0;
        if (strstr(name, ".worker.")) return 0;
        return 1;
    }
    return 0;
}

struct list { char **p; int n, cap; };
static void l_add(struct list *l, const char *s)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256;
                          l->p = realloc(l->p, (size_t)l->cap * sizeof *l->p); }
    l->p[l->n++] = strdup(s);
}
static int cmpstr(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* Directories that hold support material rather than tests, or that cannot run
 * without a server / a real browser. Skipping them is a statement about the
 * runner's reach, so each name is one the report has to mention. */
static int skip_dir(const char *name)
{
    static const char *skip[] = {
        "resources", "support", "META.yml", "tools", "chromium",
        "webidl2", "test262", ".git", "idlharness", 0
    };
    for (int i = 0; skip[i]; i++) if (!strcmp(name, skip[i])) return 1;
    return 0;
}

static void walk(const char *root, const char *rel, struct list *out)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", rel, de->d_name);
        char abspath[1200];
        snprintf(abspath, sizeof abspath, "%s/%s", root, child);
        if (is_dir(abspath)) {
            if (skip_dir(de->d_name)) continue;
            walk(root, child, out);
        } else if (is_test_file(de->d_name)) {
            if (g_only && !strstr(child, g_only)) continue;
            l_add(out, child);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------- the self-check -- */
/* Finding number one, kept as an assertion: does testharness.js RUN here?
 *
 * Every number this file prints rests on it. If the harness stops installing,
 * test-wpt does not go red case by case -- it collapses to 0/0 and reads as a
 * catastrophe in the DOM rather than a broken runner. Worse, "the harness
 * completed with zero subtests" is silent by construction: no exception, no
 * failing assertion, just an empty result set. This ran into exactly that (see
 * the note on `struct incl`) and it cost an hour.
 *
 * The case below goes through run_one unchanged, so what it proves is the
 * runner: testharness.js loads, test()/assert_equals report, an async_test
 * resolves through the timer queue on the virtual clock, and a deliberately
 * failing assertion is reported as FAIL rather than swallowed. */
static const char *SELFCHECK =
"<!doctype html><meta charset=utf-8><title>runner self-check</title>\n"
"<script src=\"/resources/testharness.js\"></script>\n"
"<script src=\"/resources/testharnessreport.js\"></script>\n"
"<div id=log></div>\n"
"<script>\n"
"test(function(){ assert_equals(1+1, 2); }, 'sync test reports PASS');\n"
"test(function(){ assert_equals(1, 2, 'deliberate'); }, 'a failing assert reports FAIL');\n"
"async_test(function(t){ setTimeout(t.step_func_done(function(){\n"
"    assert_true(true); }), 50); }, 'async_test resolves through the timer queue');\n"
"promise_test(function(){ return Promise.resolve().then(function(){\n"
"    assert_equals('a', 'a'); }); }, 'promise_test resolves through microtasks');\n"
/* THE LOAD EVENT FIRES EXACTLY ONCE, asserted rather than assumed.
 *
 * The runner used to dispatch it four times over -- dispatchEvent plus a manual
 * on-property call, at document and again at the global, which js_dom.c makes
 * the same EventTarget. Nothing failed. The rate went UP, because a doubled
 * handler doubles numerator and denominator together, and the only visible sign
 * was a subtest count that had grown for no reason anyone could name. It also
 * pushed a fired-already guard into the shipping browser, which broke repeating
 * handlers like <body onscroll>.
 *
 * A miscount that makes the product look better is the one a suite must be
 * able to catch by itself, so it is counted here on both channels. */
"var __lc = 0, __ll = 0;\n"
"addEventListener('load', function(){ __lc++; });\n"
"onload = function(){ __ll++; };\n"
"async_test(function(t){ setTimeout(t.step_func_done(function(){\n"
"    assert_equals(__lc, 1, 'addEventListener(load) ran ' + __lc + ' times');\n"
"    assert_equals(__ll, 1, 'window.onload ran ' + __ll + ' times');\n"
"  }), 10); }, 'the load event fires exactly once, on both channels');\n"
"</script>\n";

static int selfcheck(void)
{
    struct res r = { 0, 0, 0, 0, 0, 0 };
    struct outcome oc;
    g_override = SELFCHECK;
    run_one("_selfcheck/selfcheck.html", &r, &oc);
    g_override = 0;

    int bad = 0;
    printf("wpt self-check: does testharness.js run in this engine?\n");
    if (!oc.completed) {
        printf("  FAIL: the harness never completed -- %s\n", oc.why);
        return 1;
    }
    printf("  ok  : testharness.js loaded and the completion callback fired\n");
    if (r.n == 0) {
        printf("  FAIL: the harness completed with ZERO subtests. Every rate this\n"
               "        runner prints would be 0/0 and would look like a clean run.\n");
        return 1;
    }
    struct { const char *name; int want; } want[] = {
        { "sync test reports PASS", 0 },
        { "a failing assert reports FAIL", 1 },
        { "async_test resolves through the timer queue", 0 },
        { "promise_test resolves through microtasks", 0 },
        { "the load event fires exactly once, on both channels", 0 },
    };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        int found = -1;
        for (int j = 0; j < r.n; j++) if (!strcmp(r.id[j], want[i].name)) found = r.status[j];
        if (found == want[i].want) printf("  ok  : %s\n", want[i].name);
        else {
            bad = 1;
            printf("  FAIL: %s -- wanted status %d, got %d\n",
                   want[i].name, want[i].want, found);
            for (int j = 0; j < r.n; j++)
                if (!strcmp(r.id[j], want[i].name) && r.msg[j][0])
                    printf("        %.200s\n", r.msg[j]);
        }
    }
    for (int i = 0; i < r.n; i++) { free(r.id[i]); free(r.msg[i]); free(r.stack[i]); }
    free(r.id); free(r.msg); free(r.stack); free(r.status);
    printf(bad ? "wpt self-check: FAILED\n" : "wpt self-check: ok\n");
    return bad;
}

/* --------------------------------------------------------------- main --- */
/* The default subsets: the same list tools/wpt_fetch.sh vendors, in the order
 * the report reads best. Absent ones are reported as absent rather than
 * skipped silently -- a subset that quietly disappears is a rate that quietly
 * changes meaning. */
static const char *SUBSETS[] = { "dom", "html/dom", "html/semantics",
                                 "encoding", "url", "console", "css", 0 };

int main(int argc, char **argv)
{
    g_root = getenv("WPT_ROOT");
    struct list subsets = { 0, 0, 0 };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { g_verbose = 1;
            if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9') g_vmax = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) g_blpath = argv[++i];
        else if (!strcmp(argv[i], "--write-baseline")) g_writebl = 1;
        else if (!strcmp(argv[i], "--strict")) g_strict = 1;
        else if (!strcmp(argv[i], "--list")) g_listonly = 1;
        else if (!strcmp(argv[i], "--dump")) g_dump = 1;
        else if (!strcmp(argv[i], "--progress")) g_progress = 1;
        else if (!strcmp(argv[i], "--no-lifecycle")) g_no_lifecycle = 1;
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) g_budget = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) g_report = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) g_root = argv[++i];
        else if (!strcmp(argv[i], "--subset") && i + 1 < argc) l_add(&subsets, argv[++i]);
        else if (argv[i][0] != '-' && !g_root) g_root = argv[i];
        else if (argv[i][0] != '-') l_add(&subsets, argv[i]);
    }

    if (!g_root) g_root = "third_party/wpt";
    if (!is_dir(g_root)) {
        printf("wpt: corpus not present at %s -- nothing to measure.\n", g_root);
        printf("wpt: this is not a failure. Point WPT_ROOT at a checkout, or run\n"
               "     `make wpt-fetch` to vendor the subsets.\n");
        return 0;
    }
    char hp[1024];
    snprintf(hp, sizeof hp, "%s/resources/testharness.js", g_root);
    if (!xread(hp, 0)) {
        printf("wpt: %s has no resources/testharness.js -- not a WPT checkout.\n", g_root);
        return 2;
    }

    if (subsets.n == 1 && !strcmp(subsets.p[0], "_selfcheck")) return selfcheck();
    if (g_report) {
        g_repf = fopen(g_report, "wb");
        if (!g_repf) { fprintf(stderr, "cannot write %s\n", g_report); return 2; }
        fprintf(g_repf, "#status\tpath\tsubtest\tmessage\tstack\n");
    }
    if (!subsets.n) for (int i = 0; SUBSETS[i]; i++) l_add(&subsets, SUBSETS[i]);

    struct baseline expected = { 0, 0, 0 };
    bl_load(&expected, g_blpath);
    struct blhash eh; blh_build(&eh, &expected);
    struct baseline failures = { 0, 0, 0 };

    long tot_pass = 0, tot_fail = 0, tot_files = 0, tot_broken = 0;
    long tot_ref = 0, tot_other = 0, tot_never = 0, tot_driver = 0, tot_onload = 0;
    int shown = 0;

    for (int si = 0; si < subsets.n; si++) {
        char sd[1024];
        snprintf(sd, sizeof sd, "%s/%s", g_root, subsets.p[si]);
        if (!is_dir(sd)) { printf("  %-24s (absent)\n", subsets.p[si]); continue; }

        struct list files = { 0, 0, 0 };
        walk(g_root, subsets.p[si], &files);
        qsort(files.p, (size_t)files.n, sizeof *files.p, cmpstr);

        long spass = 0, sfail = 0, sbroken = 0, sref = 0, sother = 0;
        long snever = 0, sdriver = 0, sonload = 0;
        for (int fi = 0; fi < files.n; fi++) {
            if (g_listonly) { printf("%s\n", files.p[fi]); continue; }
            int kind = classify(files.p[fi]);
            if (kind == K_REFTEST) { sref++; tot_ref++;
                if (g_repf) fprintf(g_repf, "REFTEST\t%s\t[NOT RUN]\tno reftest harness: judged by pixels against <link rel=match>\n", files.p[fi]);
                continue; }
            if (kind == K_NEITHER) { sother++; tot_other++;
                if (g_repf) fprintf(g_repf, "NOHARNESS\t%s\t[NOT RUN]\tnot a testharness test (reference, manual or support page)\n", files.p[fi]);
                continue; }
            if (g_progress) fprintf(stderr, "\r%-100.100s", files.p[fi]), fflush(stderr);
            struct res r = { 0, 0, 0, 0, 0, 0 };
            struct outcome oc;
            run_one(files.p[fi], &r, &oc);
            tot_files++;

            /* Zero subtests counts as a harness failure, not as a clean file.
             * A run that produces no results at all is the one failure mode
             * that is invisible in a rate: 0/0 is 100%. */
            /* Per-file failure ids, held back until the file is done so an
             * all-red file can be collapsed to one "<path>::*" line. */
            struct baseline ffail = { 0, 0, 0 };
            long fpass = 0;

            if (!oc.completed || r.n == 0) {
                /* DIED vs NEVER STARTED -- see `struct outcome`. A file that
                 * threw is category 1 and belongs in the work order. A file
                 * that ran clean and registered nothing is category 2: it is
                 * waiting on synthetic input, and no DOM or CSS work moves it. */
                int never_started = (!oc.threw && r.n == 0);
                char id[2048];
                const char *w;
                if (never_started) {
                    snever++; tot_never++;
                    snprintf(id, sizeof id, "%s::[NOTRUN]", files.p[fi]);
                    w = oc.onload_attr
                        ? "registered no tests: starts from <body onload=...>, and an"
                          " event-handler CONTENT ATTRIBUTE is never compiled into a"
                          " handler (window.onload stays null)"
                        : oc.needs_driver
                        ? "registered no tests: needs testdriver.js (synthetic input)"
                        : "registered no tests and raised nothing (waiting on an event)";
                } else {
                    sbroken++; tot_broken++;
                    snprintf(id, sizeof id, "%s::[HARNESS]", files.p[fi]);
                    w = oc.why[0] ? oc.why : "stopped part-way with no message";
                }
                bl_add(&ffail, id);
                if (g_repf) {
                    fprintf(g_repf, "%s\t%s\t%s\t",
                            never_started ? "NOTSTARTED" : "HARNESS", files.p[fi],
                            never_started ? "[NOTRUN]" : "[HARNESS]");
                    for (const char *s = w; *s; s++) fputc(*s == '\t' || *s == '\n' ? ' ' : *s, g_repf);
                    fputc('\t', g_repf);
                    for (const char *s = oc.stack; *s; s++) fputc(*s == '\t' || *s == '\n' ? ' ' : *s, g_repf);
                    fputc('\n', g_repf);
                }
                if (never_started && oc.onload_attr) { sonload++; tot_onload++; }
                else if (oc.needs_driver) { sdriver++; tot_driver++; }
                if (g_verbose && shown < g_vmax && !blh_has(&eh, id) && !never_started) {
                    shown++;
                    printf("  HARNESS %s\n     %s\n", files.p[fi], w);
                }
            }
            for (int i = 0; i < r.n; i++) {
                /* testharness status: 0 PASS, 1 FAIL, 2 TIMEOUT, 3 NOTRUN,
                 * 4 PRECONDITION_FAILED. Only 0 counts. */
                char id[2048];
                snprintf(id, sizeof id, "%s::%s", files.p[fi], r.id[i]);
                if (g_repf) {
                    static const char *SN[] = { "PASS", "FAIL", "TIMEOUT",
                                                "NOTRUN", "PRECONDITION_FAILED" };
                    int s = r.status[i];
                    fprintf(g_repf, "%s\t%s\t", (s >= 0 && s < 5) ? SN[s] : "?", files.p[fi]);
                    for (const char *p = r.id[i]; *p; p++) fputc(*p == '\t' || *p == '\n' ? ' ' : *p, g_repf);
                    fputc('\t', g_repf);
                    for (const char *p = r.msg[i]; *p; p++) fputc(*p == '\t' || *p == '\n' ? ' ' : *p, g_repf);
                    fputc('\t', g_repf);
                    for (const char *p = r.stack[i]; *p; p++) fputc(*p == '\t' || *p == '\n' ? ' ' : *p, g_repf);
                    fputc('\n', g_repf);
                }
                if (r.status[i] == 0) { spass++; tot_pass++; fpass++; }
                else {
                    sfail++; tot_fail++;
                    bl_add(&ffail, id);
                    if (g_verbose && shown < g_vmax && !blh_has(&eh, id)) {
                        shown++;
                        printf("  FAIL %s\n     %.240s\n", id, r.msg[i]);
                    }
                }
                free(r.id[i]); free(r.msg[i]); free(r.stack[i]);
            }
            free(r.id); free(r.msg); free(r.stack); free(r.status);

            if (ffail.n && fpass == 0) {
                char wild[2048];
                snprintf(wild, sizeof wild, "%s::*", files.p[fi]);
                bl_add(&failures, wild);
            } else {
                for (int i = 0; i < ffail.n; i++) bl_add(&failures, ffail.id[i]);
            }
            for (int i = 0; i < ffail.n; i++) free(ffail.id[i]);
            free(ffail.id);
        }
        if (!g_listonly) {
            if (g_progress) fprintf(stderr, "\r%100s\r", "");
            printf("  %-22s %6ld/%-6ld subtests | %4ld files | died %ld,"
                   " never started %ld (%ld <body onload>, %ld testdriver) | not run:"
                   " %ld reftest, %ld other\n",
                   subsets.p[si], spass, spass + sfail,
                   (long)files.n - sref - sother, sbroken, snever, sonload,
                   sdriver, sref, sother);
        }
        for (int i = 0; i < files.n; i++) free(files.p[i]);
        free(files.p);
    }
    if (g_listonly) return 0;

    long tot = tot_pass + tot_fail;
    printf("\nWPT: %ld/%ld subtests passed (%.1f%%) over %ld harness files.\n",
           tot_pass, tot, tot ? 100.0 * (double)tot_pass / (double)tot : 0.0,
           tot_files);
    printf("     %ld files DIED -- threw part-way. Fixable, and the work order.\n"
           "     %ld files NEVER STARTED -- loaded clean, registered no test().\n"
           "        Split, because the two halves are not the same finding:\n"
           "          %ld start from <body onload=...>. An event-handler CONTENT\n"
           "            ATTRIBUTE is never compiled into a handler here: the\n"
           "            attribute is in the DOM and window.onload stays null, so\n"
           "            the page is inert. ONE browser behaviour, and FIXABLE.\n"
           "          %ld want testdriver.js -- synthetic click/touch/wheel/scroll\n"
           "            only a test driver can send. A missing capability in the\n"
           "            HARNESS, not a browser defect; no DOM or CSS work moves\n"
           "            them.\n"
           "        Both tokened ::[NOTRUN] so neither pollutes the ratchet.\n",
           tot_broken, tot_never, tot_onload, tot_driver);
    printf("     NOT RUN: %ld reftests -- judged by pixels against a reference render,\n"
           "        and there is no reftest harness here, so LAYOUT correctness is\n"
           "        essentially UNMEASURED by the rate above. %ld other files.\n",
           tot_ref, tot_other);

    struct blhash fh; blh_build(&fh, &failures);
    int newfail = 0, newpass = 0;
    for (int i = 0; i < failures.n; i++)
        if (!blh_has(&eh, failures.id[i])) {
            if (newfail < 20) printf("  NEW FAILURE: %.200s\n", failures.id[i]);
            newfail++;
        }
    for (int i = 0; i < expected.n; i++)
        if (!blh_has(&fh, expected.id[i])) newpass++;

    if (expected.n)
        printf("baseline %s: %d expected failures; %d new, %d newly passing\n",
               g_blpath, expected.n, newfail, newpass);
    else
        printf("baseline %s: not found (run with --write-baseline to create it)\n", g_blpath);

    if (g_writebl) {
        FILE *f = fopen(g_blpath, "wb");
        if (f) {
            fprintf(f,
                "# WPT subtests that fail today, as \"<path>::<subtest name>\".\n"
                "#\n"
                "# \"<path>::*\" means EVERY subtest in that file fails -- the file is\n"
                "# collapsed to one line because a per-subtest list of an all-red file\n"
                "# carries no information a single line does not, and the uncollapsed\n"
                "# form of this corpus is a 16 MB file nobody diffs. Files with a MIXED\n"
                "# result keep their exact per-subtest entries, which is where a ratchet\n"
                "# earns its keep: a regression there names the subtest.\n"
                "#\n"
                "# \"::[HARNESS]\" means the file never completed the harness at all -- a\n"
                "# worse category than a failing assertion, and the one to read first.\n"
                "#\n"
                "# A subtest name may contain a newline (css/css-syntax names cases after\n"
                "# the declaration block under test); those are escaped as \\n here and\n"
                "# unescaped on read, so one entry is always exactly one line.\n"
                "#\n"
                "# Regenerate with `make wpt-baseline`. Shrinking this file is the point;\n"
                "# growing it needs a reason written next to the new entries.\n");
            for (int i = 0; i < failures.n; i++) {
                bl_escape(f, failures.id[i]);
                fputc('\n', f);
            }
            fclose(f);
            printf("wrote %d entries to %s\n", failures.n, g_blpath);

            /* THE ROUND-TRIP ASSERTION. Escaping is only the means; this is the
             * check. Re-read what was just written and require it to hold the
             * same entries -- a baseline that does not read back as it was
             * written is a ratchet that reports permanent phantom failures on
             * one side and phantom fixes on the other. */
            struct baseline back = { 0, 0, 0 };
            bl_load(&back, g_blpath);
            int bad = (back.n != failures.n);
            if (!bad) {
                struct blhash bh; blh_build(&bh, &back);
                for (int i = 0; i < failures.n; i++)
                    if (!blh_exact(&bh, failures.id[i])) { bad = 1;
                        printf("  round trip lost: %.120s\n", failures.id[i]);
                        break; }
                blh_free(&bh);
            }
            if (bad) {
                fprintf(stderr, "wpt: BASELINE ROUND TRIP FAILED -- wrote %d entries,"
                                " read back %d. The ratchet would lie; refusing.\n",
                        failures.n, back.n);
                return 2;
            }
            printf("baseline round trip: ok (%d entries in, %d out)\n",
                   failures.n, back.n);
        } else { fprintf(stderr, "cannot write %s\n", g_blpath); return 2; }
    }

    blh_free(&eh); blh_free(&fh);
    return (g_strict && newfail) ? 1 : 0;
}
