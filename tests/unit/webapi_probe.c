/* webapi_probe -- measure which JavaScript globals real pages reach for and do
 * not find, so the Web API surface is EXTENDED FROM DATA rather than guessed.
 *
 *     make probe-webapi                  # the whole corpus, ranked
 *     build/webapi_probe tests/fixtures/webapi/bing        # one site
 *     build/webapi_probe --errors <dir>  # channel 2 only (see below)
 *
 * WHY A PROBE AND NOT A LIST OF WEB APIS
 * A hand-written list of "the APIs a browser needs" is a list of the APIs the
 * author remembers. The CSS line measured `var()` at 101,904 occurrences across
 * 18 sites and found that the two things its author had guessed were hot
 * accounted for 4% and 0.1%. This is the same instrument for the JS side: load
 * the pages that actually fail, record every global lookup that misses, rank by
 * how many PAGES need each name, and implement down that list.
 *
 * TWO CHANNELS, AND THE DIFFERENCE BETWEEN THEM IS THE POINT
 *
 *   Channel 1 (the histogram) runs each script inside `with (probeScope)`,
 *   where probeScope is a Proxy whose `has` trap answers true for everything
 *   and records the names the real global object does not have. Answering true
 *   is what SUPPRESSES the ReferenceError, so the script keeps running past the
 *   first miss and reports the second, third and thirtieth. That is the only
 *   way to get a histogram at all: the browser today sees exactly one miss per
 *   script, because the first one ends it.
 *
 *   Channel 2 (the ground truth) runs the same scripts UNWRAPPED, exactly as
 *   c/apps/browser/browser.c does, and records the error each one actually dies
 *   on. This is what the user sees. It is also the acceptance check: when a
 *   name from channel 1 is implemented, the channel-2 error for that page must
 *   change (or go away), and if it does not, the implementation did not matter.
 *
 * KNOWN DISTORTION OF CHANNEL 1, STATED RATHER THAN HIDDEN
 * `with (o) { let x = 1 }` makes x block-scoped, so a top-level `let`/`const`/
 * `class` in script A is not visible to script B under channel 1 -- which can
 * manufacture a miss that the real browser would not have. Channel 2 has no
 * such distortion, so any name that matters is cross-checked there before it is
 * implemented. Names that appear ONLY in channel 1 are marked `~` in the table.
 *
 * The corpus is committed bytes (tests/fixtures/webapi/, see capture.py): no
 * network, no DNS, no TLS, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_webapi.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* The Rust staticlib's png_register calls back into the image registry. The
 * probe decodes no images, so the registry is a sink -- linking c/lib/image/img.c
 * would drag in every codec for nothing. */
void img_register(void *detect, void *decode) { (void)detect; (void)decode; }
void img_register_anim(void *detect, void *decode, void *anim)
{ (void)detect; (void)decode; (void)anim; }

/* ---- the miss table ----------------------------------------------------
 * Flat and linear-scanned. A page reaches a few hundred distinct names; a hash
 * table would cost more code than it saves, and the table is printed sorted
 * anyway. */
#define NAMEMAX 64
#define MISSMAX 2048
#define SITEMAX 32

struct miss {
    char name[NAMEMAX];
    long refs;                       /* total lookups across the corpus */
    unsigned long sites;             /* bitmask: which fixtures reached for it */
    unsigned long sites_c2;          /* ... and which died on it unwrapped */
    long refs_deep;                  /* --deep only: reach past the first TypeError */
    unsigned long sites_deep;
};
static struct miss g_miss[MISSMAX];
static int g_nmiss;
static int g_site;                   /* index of the fixture being probed */
static const char *g_sitename[SITEMAX];
static int g_nsite;
static int g_recording;              /* 0 off, 1 channel-1 plain, 2 channel-1 --deep */
static int g_deep;

static struct miss *miss_find(const char *n)
{
    for (int i = 0; i < g_nmiss; i++) if (!strcmp(g_miss[i].name, n)) return &g_miss[i];
    if (g_nmiss >= MISSMAX) return 0;
    struct miss *m = &g_miss[g_nmiss++];
    int i = 0; while (n[i] && i < NAMEMAX - 1) { m->name[i] = n[i]; i++; }
    m->name[i] = 0;
    return m;
}

static void miss_note(const char *n)
{
    if (!g_recording) return;
    struct miss *m = miss_find(n);
    if (!m) return;
    if (g_recording == 2) { m->refs_deep++; m->sites_deep |= 1ul << g_site; return; }
    m->refs++;
    m->sites |= 1ul << g_site;
}

static void miss_note_c2(const char *n)
{
    struct miss *m = miss_find(n);
    if (!m) return;
    m->sites_c2 |= 1ul << g_site;
}

static int popcnt(unsigned long v) { int n = 0; while (v) { n += (int)(v & 1); v >>= 1; } return n; }

/* ---- the recorder, called from the Proxy's has() trap ------------------- */
static JSValue js_probe_miss(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { miss_note(s); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* The scope proxy. `has` is what a `with` statement consults for an unqualified
 * identifier; answering true for a name the global object lacks is what stops
 * the ReferenceError and lets the script keep going.
 *
 * Symbol keys are passed straight through: Symbol.unscopables is asked for on
 * every single lookup, and a proxy that claimed to have it would break `with`
 * itself. */
/* Channel 3: property misses on the platform objects we DO provide.
 *
 * The first run of this probe found 17 missing globals across 7 real pages and
 * almost all of them were the pages' OWN names -- `_w`, `RLQ`, `_N_E` -- i.e.
 * names a page defines in one script and reads in another, missing only because
 * the defining script had already died. The globals were never the shortage.
 * What the pages actually die on is `performance.timing.navigationStart`,
 * `document.currentScript`, `navigator.clipboard`: properties of objects that
 * EXIST and are incomplete. A probe that only watched the global object would
 * have reported "we are nearly done" and been wrong.
 *
 * So each platform object is replaced by a recording Proxy. Existing properties
 * are forwarded (methods bound to the real target, because js_dom.c's C
 * functions read an opaque pointer off `this` and a Proxy is not it); missing
 * ones are recorded as `object.property` and read back as undefined.
 *
 * `window` is a proxy over globalThis rather than globalThis itself -- there is
 * no way to replace the global object -- so `window.foo` is recorded while bare
 * `foo` goes through the scope proxy above. The cost is that `window ===
 * globalThis` is false under the probe. Stated, not hidden: it is a measurement
 * artefact and no assertion depends on it. */
static const char *PROBE_MEMBERS =
"(function () {\n"
"  var wrap = function (label, obj) {\n"
"    if (!obj || (typeof obj !== 'object' && typeof obj !== 'function')) return obj;\n"
"    return new Proxy(obj, {\n"
"      get: function (t, k, r) {\n"
"        if (typeof k === 'symbol') return Reflect.get(t, k, t);\n"
"        if (k in t) {\n"
"          var v; try { v = Reflect.get(t, k, t); } catch (e) { return undefined; }\n"
"          return typeof v === 'function' ? v.bind(t) : v;\n"
"        }\n"
"        __probeMiss(label + '.' + k);\n"
"        return undefined;\n"
"      },\n"
"      has: function (t, k) { return Reflect.has(t, k); },\n"
"      set: function (t, k, v) { try { t[k] = v; } catch (e) {} return true; }\n"
"    });\n"
"  };\n"
"  var names = ['navigator', 'performance', 'screen', 'history', 'location',\n"
"               'localStorage', 'sessionStorage', 'crypto', 'document'];\n"
"  for (var i = 0; i < names.length; i++) {\n"
"    var n = names[i];\n"
"    try { if (globalThis[n]) globalThis[n] = wrap(n, globalThis[n]); } catch (e) {}\n"
"  }\n"
"  try { globalThis.window = wrap('window', globalThis); } catch (e) {}\n"
"  try { globalThis.self = globalThis.window; } catch (e) {}\n"
"})();\n";

static const char *PROBE_SHIM =
"globalThis.__probeScope = new Proxy(globalThis, {\n"
"  has: function (t, k) {\n"
"    if (typeof k !== 'string') return false;\n"
"    if (k in t) return true;\n"
"    __probeMiss(k);\n"
"    return true;\n"
"  },\n"
"  get: function (t, k) {\n"
"    if (typeof k !== 'string') return t[k];\n"
"    if (k in t) return t[k];\n"
"    return globalThis.__probeStub ? globalThis.__probeStub(k) : undefined;\n"
"  },\n"
"  set: function (t, k, v) { t[k] = v; return true; },\n"
"  deleteProperty: function (t, k) { delete t[k]; return true; }\n"
"});\n";

/* --deep. A missing global normally reads back as `undefined`, and the very
 * next line -- `_G.foo`, `RLQ.push(...)` -- is then a TypeError that ends the
 * script and the measurement with it. That is why the plain run finds so few
 * names: it is not that pages want little, it is that they stop.
 *
 * The stub is a Proxy that is callable, indexable, constructible and coerces to
 * a string/number, so the script keeps running and keeps reaching. What it
 * cannot do is tell the truth about behaviour -- a page that branches on a
 * stub's VALUE takes a path it would not take in a browser. So --deep is
 * reported in its own column and never used alone to justify implementing
 * something: the PAGES column from the plain run, and channel 2, are the
 * evidence. --deep says what a page would ask for NEXT. */
static const char *PROBE_STUB =
"(function () {\n"
"  var mk = function (name) {\n"
"    var f = function () { return mk(name + '()'); };\n"
"    f.__stub = name;\n"
"    return new Proxy(f, {\n"
"      get: function (t, k) {\n"
"        if (k === Symbol.toPrimitive) return function () { return 0; };\n"
"        if (k === 'toString' || k === 'valueOf') return function () { return ''; };\n"
"        if (k === Symbol.iterator) return function () { return [][Symbol.iterator](); };\n"
"        if (k === 'prototype' || k === '__stub') return t[k];\n"
"        if (typeof k === 'symbol') return undefined;\n"
"        return mk(name + '.' + k);\n"
"      },\n"
"      set: function () { return true; },\n"
"      has: function () { return true; },\n"
"      apply: function () { return mk(name + '()'); },\n"
"      construct: function () { return mk('new ' + name); }\n"
"    });\n"
"  };\n"
"  globalThis.__probeStub = mk;\n"
"})();\n";

/* ---- the fixture ------------------------------------------------------- */
struct script { char *data; int len; int module; char url[256]; };
#define SCRMAX 64
static struct script g_scr[SCRMAX];
static int g_nscr;

struct manent { char src[512]; char file[64]; };
#define MANMAX 64
static struct manent g_man[MANMAX];
static int g_nman;

static char *slurp(const char *path, int *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = 0;
    fclose(f);
    if (out_len) *out_len = (int)got;
    return b;
}

static void load_manifest(const char *dir)
{
    g_nman = 0;
    char p[512];
    snprintf(p, sizeof p, "%s/manifest.txt", dir);
    char *m = slurp(p, 0);
    if (!m) return;
    char *line = m;
    while (*line && g_nman < MANMAX) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            snprintf(g_man[g_nman].src, sizeof g_man[g_nman].src, "%s", line);
            snprintf(g_man[g_nman].file, sizeof g_man[g_nman].file, "%s", tab + 1);
            g_nman++;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(m);
}

/* Mirrors browser.c's collect_scripts: type is a whitelist, src goes to the
 * network (here: to the manifest), inline text is reassembled from the text
 * children. A probe that collected scripts differently from the browser would
 * be measuring a page the browser never runs. */
static int is_classic_type(const char *t)
{
    if (!t || !*t) return 1;
    /* the JavaScript MIME types, loosely -- enough for the corpus */
    return strstr(t, "javascript") != 0 || !strcmp(t, "text/ecmascript");
}

static void collect(struct node *n, const char *dir)
{
    if (!n) return;
    if (n->type == N_ELEM && n->tag && !strcmp(n->tag, "script") && g_nscr < SCRMAX) {
        const char *type = dom_attr(n, "type");
        const char *src  = dom_attr(n, "src");
        int module = type && !strcmp(type, "module");
        int classic = !module && is_classic_type(type);
        if (!module && !classic) {
            /* application/json, importmap, ld+json: data, not program */
        } else if (src) {
            for (int i = 0; i < g_nman; i++)
                if (!strcmp(g_man[i].src, src)) {
                    char p[600];
                    snprintf(p, sizeof p, "%s/%s", dir, g_man[i].file);
                    int len = 0;
                    char *d = slurp(p, &len);
                    if (d) {
                        g_scr[g_nscr].data = d; g_scr[g_nscr].len = len;
                        g_scr[g_nscr].module = module;
                        snprintf(g_scr[g_nscr].url, sizeof g_scr[g_nscr].url, "%s", g_man[i].file);
                        g_nscr++;
                    }
                    break;
                }
        } else {
            int total = 0;
            for (struct node *c = n->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text) total += c->textlen;
            if (total > 0) {
                char *d = malloc((size_t)total + 1);
                if (d) {
                    int o = 0;
                    for (struct node *c = n->first_child; c; c = c->next)
                        if (c->type == N_TEXT && c->text)
                            for (int i = 0; i < c->textlen; i++) d[o++] = c->text[i];
                    d[o] = 0;
                    g_scr[g_nscr].data = d; g_scr[g_nscr].len = o;
                    g_scr[g_nscr].module = module;
                    snprintf(g_scr[g_nscr].url, sizeof g_scr[g_nscr].url, "<inline %d>", g_nscr + 1);
                    g_nscr++;
                }
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect(c, dir);
}

static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now += 4; }

/* Channel 2's report: the first line of the error, and the identifier if the
 * engine named one. QuickJS spells it `ReferenceError: 'x' is not defined`. */
static void note_c2_error(const char *msg)
{
    const char *p = strstr(msg, "is not defined");
    if (!p) return;
    const char *q = strchr(msg, '\'');
    if (!q || q > p) return;
    const char *e = strchr(q + 1, '\'');
    if (!e || e > p) return;
    char nm[NAMEMAX];
    int n = (int)(e - q - 1);
    if (n <= 0 || n >= NAMEMAX) return;
    memcpy(nm, q + 1, (size_t)n);
    nm[n] = 0;
    miss_note_c2(nm);
}

struct siteres { int nscript; int c1_ok; int c2_ok; };

static void probe_site(const char *dir, int show_errors, int show_scripts)
{
    char p[600];
    snprintf(p, sizeof p, "%s/index.html", dir);
    int hlen = 0;
    char *html = slurp(p, &hlen);
    if (!html) { printf("  (no index.html in %s)\n", dir); return; }

    load_manifest(dir);
    g_nscr = 0;
    struct node *root = dom_parse(html, hlen);
    if (root) collect(root, dir);

    char url[300];
    snprintf(p, sizeof p, "%s/SOURCE", dir);
    char *srcline = slurp(p, 0);
    url[0] = 0;
    if (srcline) {
        int i = 0;
        while (srcline[i] && srcline[i] != '\n' && i < (int)sizeof url - 1) { url[i] = srcline[i]; i++; }
        url[i] = 0;
        free(srcline);
    }
    if (!url[0]) snprintf(url, sizeof url, "http://fixture.invalid/");

    int c1_ok = 0, c2_ok = 0, nclassic = 0;
    for (int i = 0; i < g_nscr; i++) if (!g_scr[i].module) nclassic++;
    /* The header goes out BEFORE anything runs, so the per-script errors
     * printed below sit under the site they belong to. Printed after, they
     * appeared under the previous fixture's name, which is a report that lies. */
    printf("  %-11s %2d scripts (%d classic, %d module) from %d bytes of HTML\n",
           g_sitename[g_site], g_nscr, nclassic, g_nscr - nclassic, hlen);

    /* The corpus is only as honest as the collection. A script the DOM never
     * produced is a script the probe never measures AND the browser never runs,
     * and the difference between this list and the <script> tags in the bytes
     * is a bug report about the parser, not about the Web API surface. */
    if (show_scripts) {
        for (int i = 0; i < g_nscr; i++) {
            char head[70];
            int o = 0;
            for (int k = 0; k < g_scr[i].len && o < 66; k++) {
                char c = g_scr[i].data[k];
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if (o && head[o - 1] == ' ' && c == ' ') continue;
                head[o++] = c;
            }
            head[o] = 0;
            printf("     %2d %-14s %6d  %s\n", i + 1, g_scr[i].url, g_scr[i].len, head);
        }
    }

    /* ---- channel 2 first: unwrapped, exactly as the browser runs them.
     * First, because channel 1's `with` proxy leaves the global object littered
     * with whatever the scripts assigned, and the honest measurement is the one
     * taken on a clean runtime. */
    js_page_set_clock(clock_fn);
    js_page_set_location(url);
    if (js_page_open(root)) {
        for (int i = 0; i < g_nscr; i++) {
            if (g_scr[i].module) continue;           /* the loader owns module URLs */
            JSContext *ctx = js_page_ctx();
            JSValue v = JS_Eval(ctx, g_scr[i].data, (size_t)g_scr[i].len, g_scr[i].url,
                                JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                JSValue e = JS_GetException(ctx);
                const char *m = JS_ToCString(ctx, e);
                if (m) {
                    note_c2_error(m);
                    if (show_errors) printf("    [c2] %-16s %.150s\n", g_scr[i].url, m);
                    JS_FreeCString(ctx, m);
                }
                JS_FreeValue(ctx, e);
            } else c2_ok++;
            JS_FreeValue(ctx, v);
        }
        js_page_close();
    }

    /* ---- channel 1: the histogram. Once plain, and (with --deep) again with
     * the permissive stub, which is a separate column because it is a separate
     * claim -- see the comment on PROBE_STUB. */
    for (int mode = 1; mode <= (g_deep ? 2 : 1); mode++) {
        g_recording = mode;
        if (!js_page_open(root)) break;
        JSContext *ctx = js_page_ctx();
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__probeMiss",
                          JS_NewCFunction(ctx, js_probe_miss, "__probeMiss", 1));
        JS_FreeValue(ctx, g);
        if (mode == 2) {
            JSValue st = JS_Eval(ctx, PROBE_STUB, strlen(PROBE_STUB), "<probe>", JS_EVAL_TYPE_GLOBAL);
            JS_FreeValue(ctx, st);
        }
        /* Members BEFORE the scope proxy: the scope proxy closes over
         * globalThis, and swapping `window` afterwards would leave it holding
         * the unwrapped one. */
        JSValue mem = JS_Eval(ctx, PROBE_MEMBERS, strlen(PROBE_MEMBERS), "<probe>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, mem);
        JSValue s = JS_Eval(ctx, PROBE_SHIM, strlen(PROBE_SHIM), "<probe>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, s);

        for (int i = 0; i < g_nscr; i++) {
            if (g_scr[i].module) continue;
            /* `with` at TOP level, not inside a function: var and function
             * declarations then still land in the global var scope, so script A
             * declaring `function f(){}` is visible to script B, as on the web. */
            size_t n = (size_t)g_scr[i].len + 64;
            char *w = malloc(n + 8);
            if (!w) continue;
            int o = snprintf(w, n, "with (__probeScope) {\n");
            memcpy(w + o, g_scr[i].data, (size_t)g_scr[i].len);
            o += g_scr[i].len;
            w[o++] = '\n'; w[o++] = '}'; w[o] = 0;
            JSValue v = JS_Eval(ctx, w, (size_t)o, g_scr[i].url, JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(v)) { if (mode == 1) c1_ok++; }
            else {
                JSValue e = JS_GetException(ctx);
                const char *m = JS_ToCString(ctx, e);
                /* What stopped the histogram matters as much as the histogram:
                 * every one of these is a name the probe could not go past. */
                if (m && show_errors)
                    printf("    [c1%s] %-16s %.140s\n", mode == 2 ? "-deep" : "",
                           g_scr[i].url, m);
                if (m) JS_FreeCString(ctx, m);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, v);
            free(w);
        }
        js_page_close();
        g_recording = 0;
    }

    /* Modules are excluded from both denominators: js_module.c owns their URLs
     * and their loader, and counting a script the probe never ran as a failure
     * would put a number on this line that no change here can move. */
    printf("  %-11s ... classic scripts that ran clean: channel1 %d/%d, channel2 %d/%d\n",
           g_sitename[g_site], c1_ok, nclassic, c2_ok, nclassic);

    for (int i = 0; i < g_nscr; i++) free(g_scr[i].data);
    g_nscr = 0;
    if (root) dom_free(root);
    free(html);
}

static int cmp_miss(const void *a, const void *b)
{
    const struct miss *x = a, *y = b;
    /* Ranked by PAGES first and references second, deliberately. A name one
     * page reaches for ten thousand times is that page's own hot loop; a name
     * six pages reach for once each is the platform. */
    int px = popcnt(x->sites | x->sites_deep), py = popcnt(y->sites | y->sites_deep);
    if (px != py) return py - px;
    int cx = popcnt(x->sites), cy = popcnt(y->sites);
    if (cx != cy) return cy - cx;
    long rx = x->refs + x->refs_deep, ry = y->refs + y->refs_deep;
    if (rx != ry) return rx > ry ? -1 : 1;
    return strcmp(x->name, y->name);
}

int main(int argc, char **argv)
{
    int show_errors = 0, show_scripts = 0;
    const char *dirs[SITEMAX];
    int nd = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--errors")) show_errors = 1;
        else if (!strcmp(argv[i], "--deep")) g_deep = 1;
        else if (!strcmp(argv[i], "--scripts")) show_scripts = 1;
        else if (nd < SITEMAX) dirs[nd++] = argv[i];
    }
    if (!nd) { printf("usage: webapi_probe [--errors] [--deep] [--scripts] <fixture-dir>...\n"); return 2; }

    printf("== webapi_probe: global lookups that MISS, across %d fixtures ==\n\n", nd);
    static char names[SITEMAX][64];
    for (int i = 0; i < nd; i++) {
        /* $(dir ...) hands these over with a trailing slash, so the basename has
         * to be taken after trimming it -- otherwise every site is named "". */
        char t[512];
        snprintf(t, sizeof t, "%s", dirs[i]);
        int n = (int)strlen(t);
        while (n > 1 && (t[n - 1] == '/' || t[n - 1] == '\\')) t[--n] = 0;
        char *base = strrchr(t, '/');
        if (!base) base = strrchr(t, '\\');
        snprintf(names[i], sizeof names[i], "%s", base ? base + 1 : t);
        g_sitename[i] = names[i];
        g_site = i;
        g_nsite = nd;
        probe_site(t, show_errors, show_scripts);
    }

    qsort(g_miss, (size_t)g_nmiss, sizeof g_miss[0], cmp_miss);

    printf("\n%-30s %5s %6s %5s %7s %-5s %s\n",
           "GLOBAL", "PAGES", "REFS", "DEEP", "D-REFS", "CH2", "WHICH PAGES");
    printf("%-30s %5s %6s %5s %7s %-5s %s\n",
           "------------------------------", "-----", "------", "-----", "-------",
           "----", "-----------");
    for (int i = 0; i < g_nmiss; i++) {
        struct miss *m = &g_miss[i];
        char which[256]; int w = 0;
        which[0] = 0;
        for (int s = 0; s < g_nsite; s++)
            if ((m->sites | m->sites_deep) & (1ul << s))
                w += snprintf(which + w, sizeof which - (size_t)w, "%s%s%s",
                              w ? "," : "", g_sitename[s],
                              (m->sites & (1ul << s)) ? "" : "*");
        printf("%-30s %5d %6ld %5d %7ld %-5s %s\n", m->name,
               popcnt(m->sites), m->refs, popcnt(m->sites_deep), m->refs_deep,
               m->sites_c2 ? "DIES" : "-", which);
    }
    printf("\n%d distinct names missed.\n", g_nmiss);
    printf("PAGES/REFS  the plain run: what a page reaches for on its real execution path.\n");
    printf("DEEP/D-REFS --deep only: what it would reach for next, past the first TypeError.\n");
    printf("CH2=DIES    a script actually TERMINATED on this name, unwrapped, as it does today.\n");
    printf("a page marked * was seen only in the --deep run.\n");
    return 0;
}
