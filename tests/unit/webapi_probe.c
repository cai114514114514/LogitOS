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
#include <stdarg.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_webapi.h"
#include "js_module.h"
#include "bfetch.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* ---- printf, teed ------------------------------------------------------
 *
 * js_module.c is not this line's file and it reports a module's exception the
 * only way it can from where it sits: printf. That is fine for a serial log
 * and useless for an instrument, because the probe cannot subtract a message
 * it never sees. So printf is DEFINED here and forwards to stdout unchanged
 * while copying into a capture buffer, and the module pass reads the buffer
 * back to learn what actually threw.
 *
 * The Makefile fragment builds this binary with -fno-builtin-printf for the
 * reason that matters: gcc rewrites printf("%s\n", x) into puts/fputs, and a
 * rewritten call goes straight to libc and never reaches this function. A tee
 * that silently loses half its input is worse than no tee. */
static char g_cap[1 << 18];
static int  g_caplen, g_capon;

static void cap_start(void) { g_caplen = 0; g_cap[0] = 0; g_capon = 1; }
static void cap_stop(void)  { g_capon = 0; }

int printf(const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    if (g_capon)
        for (int i = 0; i < n && g_caplen < (int)sizeof g_cap - 1; i++)
            g_cap[g_caplen++] = buf[i], g_cap[g_caplen] = 0;
    fwrite(buf, 1, (size_t)n, stdout);
    return n;
}
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
static int g_json;                   /* --json: one #JSON line per exception */
static int g_site_exc[SITEMAX];      /* uncaught exceptions, per fixture */
/* --base: run every fixture as though it had been served from this URL.
 * The Chrome differential needs it -- Chrome is pointed at a local server, and
 * a diff taken with the two engines on different document URLs would count
 * every `location.hostname` branch as a disagreement. */
static char g_base_override[300];

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
struct script {
    char *data; int len; int module;
    char url[256];              /* the short label used in the report */
    char abs[600];              /* the ABSOLUTE URL: a module's identity */
};
#define SCRMAX 64
static struct script g_scr[SCRMAX];
static int g_nscr;

/* `abs` is the manifest src resolved against the document URL, and it is what
 * the module loader will ask this file for -- a module specifier is normalized
 * to an absolute URL before it reaches bfetch, so a table keyed on the raw src
 * attribute could not answer a single import. Both keys are kept: the raw one
 * is how collect() finds a <script src>, the absolute one is how the loader
 * finds an import. */
struct manent { char src[512]; char file[64]; char abs[600]; };
/* Was 64, which was one entry per <script src> and enough while the corpus was
 * documents. A module graph is the whole application: kimi's entry point pulls
 * 200 chunks, so a 64-entry routing table would answer the first few imports
 * and 404 the rest -- and the probe would report that as the page failing. */
#define MANMAX 512
static struct manent g_man[MANMAX];
static int g_nman;
static char g_pagebase[600];      /* the document's absolute URL */

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

/* ---- the fixture, served as a web site ---------------------------------
 *
 * WHY THIS IS HERE AT ALL. The probe used to contain two lines:
 *
 *     if (g_scr[i].module) continue;      // the loader owns module URLs
 *
 * -- one in each channel -- and a comment saying modules were excluded from
 * both denominators. So every number this instrument printed was a number
 * about CLASSIC scripts. MDN is four modules out of five scripts; kimi's
 * entry point is a module; every Vite/Rollup/Next build on the web ships as a
 * module graph. "bing 11/12, deepseek 31/31" said nothing whatsoever about
 * the code path modern applications actually run in.
 *
 * The reason the exclusion was there is real: js_module.c resolves a specifier
 * to an absolute URL and hands it to bfetch, and bfetch is browser_rt.c --
 * sockets, TLS, a connection pool, none of which exists in a host unit test.
 * So bfetch is implemented HERE, against the committed fixture: the manifest
 * becomes a routing table from absolute URL to local file, and `import
 * "./chunk.js"` is answered from disk exactly as it would be answered from the
 * network. Nothing else about the module path is faked -- the normalizer, the
 * loader, the linker and the evaluator are js_module.c's and QuickJS's own.
 *
 * A specifier that resolves to something the fixture does not hold is counted
 * and reported SEPARATELY as a corpus gap, never as an engine failure. That
 * distinction is the whole reason for a separate counter: capture.py walks the
 * document's <script src> attributes and stops, so a module graph's interior
 * edges are only in the fixture if someone went and got them, and a missing
 * chunk must not be able to masquerade as a bug in this browser. */
static int  g_fetch_ok, g_fetch_miss;
#define MODMISS_MAX 32
static char g_modmiss[MODMISS_MAX][256];
static int  g_nmodmiss;
#define DROPMAX 32
static char g_dropped[DROPMAX][256];      /* <script src> the manifest lacks */
static int  g_ndropped;

/* RFC 3986 relative resolution, enough of it for real bundles: absolute,
 * protocol-relative, root-relative, relative, query-only and fragment-only,
 * with dot-segment removal. browser_rt.c's bfetch_resolve does the same job
 * against the same header; this one has no network under it. */
static void strip_dots(char *path)
{
    /* Segment stack. `mark[k]` is where segment k starts in `out`, so popping a
     * ".." is a truncation rather than a backwards scan for a slash -- which is
     * the version that gets "/a//../b" wrong. */
    char out[600]; int o = 0;
    int mark[128], nm = 0;
    const char *s = path;
    if (*s == '/') out[o++] = '/';
    while (*s) {
        while (*s == '/') s++;
        const char *seg = s;
        while (*s && *s != '/') s++;
        int n = (int)(s - seg);
        int trailing = (*s == '/');
        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nm > 0) { o = mark[--nm]; }
            continue;
        }
        if (n == 0) continue;
        if (nm < 128) mark[nm++] = o;
        for (int i = 0; i < n && o < (int)sizeof out - 2; i++) out[o++] = seg[i];
        if (trailing && o < (int)sizeof out - 1) out[o++] = '/';
    }
    out[o] = 0;
    memcpy(path, out, (size_t)o + 1);
}

int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{
    if (!ref || !out || max < 2) return -1;
    const char *b = (base && base[0]) ? base : g_pagebase;
    char tmp[600];

    if (strstr(ref, "://")) { snprintf(out, max, "%s", ref); return 0; }
    if (ref[0] == '/' && ref[1] == '/') {                  /* //host/path */
        const char *c = strstr(b, "://");
        int schemelen = c ? (int)(c - b) : 5;
        snprintf(tmp, sizeof tmp, "%.*s:%s", schemelen, c ? b : "https", ref);
    } else if (ref[0] == '/') {                            /* /abs/path */
        const char *c = strstr(b, "://");
        const char *slash = c ? strchr(c + 3, '/') : 0;
        int hostlen = slash ? (int)(slash - b) : (int)strlen(b);
        snprintf(tmp, sizeof tmp, "%.*s%s", hostlen, b, ref);
    } else if (ref[0] == '?' || ref[0] == '#') {
        int keep = 0;
        while (b[keep] && b[keep] != (ref[0] == '#' ? '#' : '?')) keep++;
        snprintf(tmp, sizeof tmp, "%.*s%s", keep, b, ref);
    } else {                                               /* relative */
        const char *c = strstr(b, "://");
        const char *last = strrchr(c ? c + 3 : b, '/');
        int keep = last ? (int)(last - b) + 1 : (int)strlen(b);
        snprintf(tmp, sizeof tmp, "%.*s%s", keep, b, ref);
    }
    /* Dot segments live only in the PATH: not in scheme://host, and not in the
     * query -- "?next=../x" is a value, not a path step. */
    const char *c = strstr(tmp, "://");
    int hoff = 0;
    if (c) { const char *sl = strchr(c + 3, '/'); hoff = sl ? (int)(sl - tmp) : (int)strlen(tmp); }
    int pend = hoff;
    while (tmp[pend] && tmp[pend] != '?' && tmp[pend] != '#') pend++;
    char tail[600];
    snprintf(tail, sizeof tail, "%s", tmp + pend);
    tmp[pend] = 0;
    strip_dots(tmp + hoff);
    snprintf(out, max, "%s%s", tmp, tail);
    return 0;
}

/* The routing table lookup. Exact absolute match first; then the path, so a
 * cache-busting query the document happens to carry cannot lose a file that is
 * plainly present. */
static const char *route(const char *url)
{
    for (int i = 0; i < g_nman; i++) if (!strcmp(g_man[i].abs, url)) return g_man[i].file;
    const char *q = strchr(url, '?');
    int n = q ? (int)(q - url) : (int)strlen(url);
    for (int i = 0; i < g_nman; i++) {
        const char *aq = strchr(g_man[i].abs, '?');
        int an = aq ? (int)(aq - g_man[i].abs) : (int)strlen(g_man[i].abs);
        if (an == n && !strncmp(g_man[i].abs, url, (size_t)n)) return g_man[i].file;
    }
    return 0;
}

static char g_fixdir[512];
static char *slurp(const char *path, int *out_len);

int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{
    char abs[600];
    bfetch_resolve(0, ref, abs, sizeof abs);
    const char *file = route(abs);
    if (!file) {
        g_fetch_miss++;
        if (g_nmodmiss < MODMISS_MAX) snprintf(g_modmiss[g_nmodmiss++], 256, "%s", abs);
        return -1;
    }
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_fixdir, file);
    int len = 0;
    char *d = slurp(p, &len);
    if (!d) { g_fetch_miss++; return -1; }
    *out = (unsigned char *)d;
    *outlen = len;
    g_fetch_ok++;
    return 0;
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
            bfetch_resolve(g_pagebase, line, g_man[g_nman].abs, (int)sizeof g_man[g_nman].abs);
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
/* browser.c classifies with js_module.c's two predicates, so the probe uses the
 * same two rather than a lookalike -- a probe that disagrees with the browser
 * about what is a script is measuring a page the browser never runs. */
static void collect(struct node *n, const char *dir)
{
    if (!n) return;
    if (n->type == N_ELEM && n->tag && !strcmp(n->tag, "script") && g_nscr < SCRMAX) {
        const char *type = dom_attr(n, "type");
        const char *src  = dom_attr(n, "src");
        int module = js_module_is_module_type(type);
        int classic = !module && js_module_is_classic_type(type);
        if (!module && !classic) {
            /* application/json, importmap, ld+json: data, not program */
        } else if (!module && dom_attr(n, "nomodule")) {
            /* the fallback for an engine without modules; we are not one */
        } else if (src) {
            int found = 0;
            for (int i = 0; i < g_nman; i++)
                if (!strcmp(g_man[i].src, src)) { found = 1; break; }
            /* A <script src> the manifest does not hold used to vanish without
             * a word, and the cost of that silence was the headline number:
             * kimi's ENTRY POINT is `index-h6DE6Ow7.js`, a module that fans out
             * to the rest of the application, and it is not in the capture. The
             * probe reported "kimi: 3 scripts, all clean". It was measuring a
             * page with the application removed. */
            if (!found && g_ndropped < DROPMAX)
                snprintf(g_dropped[g_ndropped++], 256, "%s", src);
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
                        snprintf(g_scr[g_nscr].abs, sizeof g_scr[g_nscr].abs, "%s", g_man[i].abs);
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
                    /* An inline module has no URL of its own; per spec it takes
                     * the DOCUMENT's, so its relative specifiers resolve against
                     * the page. The discriminator only keeps two of them from
                     * colliding in the module map -- browser.c does the same. */
                    snprintf(g_scr[g_nscr].abs, sizeof g_scr[g_nscr].abs,
                             "%s#inline-module-%d", g_pagebase, g_nscr + 1);
                    g_nscr++;
                }
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect(c, dir);
}

static unsigned long long g_now;
static unsigned long long clock_fn(void) { return g_now += 4; }

/* THE PROBE'S OWN BLIND SPOT, FIXED AFTER THE MACHINE FOUND IT.
 *
 * The first version of this file evaluated each script and stopped. That
 * measures what a page does while it is LOADING and nothing it does
 * afterwards, and a modern page defers most of itself: bing reaches `Image`
 * and `btoa` from setTimeout callbacks, and neither name appeared anywhere in
 * this table. tests/qmp/qmp_bing.py found them on the real machine as
 * `uncaught in timer: ReferenceError`, because the machine has an event loop
 * and this did not.
 *
 * So the probe now turns the loop. The clock is the fake one above, which
 * jumps 4 ms per read, so a couple of hundred passes cover several seconds of
 * page time without anything sleeping -- and a setInterval cannot spin
 * forever, because the pass budget is fixed. */
#define LOOP_PASSES 200

/* ---- the exception ledger ----------------------------------------------
 *
 * THIS is the number the whole exercise is about, and it did not exist before.
 * The table above counts NAMES; a name is a symptom. What a user sees is an
 * uncaught exception, and until this ledger there was no per-page count of
 * them and therefore no before/after that could move for a nameable reason.
 *
 * Every entry carries where it came from, because "an exception" is four
 * different events with four different owners:
 *   script   a classic <script> died at top level
 *   module   a <script type=module> (or something it imported) died
 *   timer    a setTimeout/setInterval/rAF callback died -- invisible to any
 *            instrument that does not turn the event loop
 *   console  console.error, which is where an unhandled promise rejection
 *            surfaces (js_platform.c's onReject writes one)
 *   fetch    a module URL the fixture does not hold: a CORPUS GAP, counted
 *            apart from everything else and never as an engine failure. */
struct exc { char kind[12]; char where[64]; char msg[400]; };
#define EXCMAX 512
static struct exc g_exc[EXCMAX];
static int g_nexc;

static void exc_add(const char *kind, const char *where, const char *msg)
{
    if (g_nexc >= EXCMAX) return;
    struct exc *e = &g_exc[g_nexc++];
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->where, sizeof e->where, "%s", where ? where : "?");
    /* One line: a QuickJS exception stringifies to "Type: message" and then a
     * backtrace, and the backtrace is not part of the identity of the error. */
    int o = 0;
    for (const char *p = msg ? msg : "?"; *p && o < (int)sizeof e->msg - 1; p++) {
        if (*p == '\n' || *p == '\r') break;
        e->msg[o++] = *p;
    }
    e->msg[o] = 0;
}

/* The console, unbounded, taken through js_page.c's note sink rather than its
 * 4 KiB display buffer. kimi produces more than 4 KiB of exceptions, and a
 * count taken off a truncated buffer is a count of how big the buffer is. */
static char *g_con;
static int   g_conlen, g_concap;
static int   g_outseen;

static void con_sink(const char *frag)
{
    int n = (int)strlen(frag);
    if (g_conlen + n + 1 > g_concap) {
        int want = (g_concap ? g_concap * 2 : 1 << 16);
        while (want < g_conlen + n + 1) want *= 2;
        char *nb = realloc(g_con, (size_t)want);
        if (!nb) return;
        g_con = nb; g_concap = want;
    }
    memcpy(g_con + g_conlen, frag, (size_t)n);
    g_conlen += n;
    g_con[g_conlen] = 0;
}

/* The host build has NO NETWORK: WEBAPI_HOST leaves every entry of
 * struct webapi_net NULL, so js_webapi.c rejects each fetch with exactly this
 * message. kimi opens dozens of them at startup. They are the INSTRUMENT's
 * environment, not the page's bug and not the engine's, so they are counted in
 * their own bucket and named in the report rather than quietly dropped -- a
 * silently filtered category is how a number stops meaning anything. */
static int is_netstub(const char *m)
{ return strstr(m, "could not open a socket") != 0; }

static void drain_console(int show_errors, const char *tag)
{
    const char *out = g_con ? g_con : "";
    int n = g_conlen;
    if (g_outseen > n) g_outseen = 0;
    const char *p = out + g_outseen;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > 0 && !strncmp(p, "[exception]", 11)) {
            char m[400];
            snprintf(m, sizeof m, "%.*s", len - 11 > 0 ? len - 12 : 0, p + 12);
            exc_add("timer", tag, m);
            if (show_errors) printf("    [%s-timer] %.*s\n", tag, len, p);
        } else if (len > 0 && !strncmp(p, "[error]", 7)) {
            char m[400];
            snprintf(m, sizeof m, "%.*s", len - 7 > 0 ? len - 8 : 0, p + 8);
            exc_add(is_netstub(m) ? "netstub" : "console", tag, m);
            if (show_errors && !is_netstub(m)) printf("    [%s-console] %.*s\n", tag, len, p);
        } else if (len > 0 && !strncmp(p, "[warn]", 6)) {
            /* console.warn is NOT an uncaught exception and is not counted as
             * one. It is recorded because it is where a page reports a failure
             * it CAUGHT -- mdn's `Unable to set theme` is a try/catch around a
             * missing Element.dataset, a gap of ours that produces no exception
             * at all and would otherwise be invisible to this instrument. */
            char m[400];
            snprintf(m, sizeof m, "%.*s", len - 6 > 0 ? len - 7 : 0, p + 7);
            exc_add("warn", tag, m);
        }
        if (!nl) break;
        p = nl + 1;
    }
    g_outseen = n;
}

static void run_event_loop(int show_errors, const char *tag, int record)
{
    for (int i = 0; i < LOOP_PASSES; i++) {
        int ran = js_page_run_due();
        ran += js_page_pump();
        if (!ran && !js_page_pending()) break;
    }
    /* Timer callbacks report their exceptions through js_page.c's console
     * capture rather than as a return value, so the errors this pass produced
     * are in the note buffer. */
    if (record) drain_console(show_errors, tag);
}

/* js_module.c reports a module's exception with printf, which is right for a
 * serial log and is the only channel it has. The tee above captures it; this
 * lifts the message back out so a module failure is an entry in the ledger and
 * not just a line on the terminal. */
static void harvest_module_output(const char *where)
{
    for (const char *p = g_cap; *p; ) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        const char *hit;
        if ((hit = strstr(p, "module exception in ")) && hit < p + len) {
            const char *c = strstr(hit, ": ");
            if (c && c < p + len) exc_add("module", where, c + 2);
        } else if ((hit = strstr(p, "module rejected ")) && hit < p + len) {
            const char *c = strstr(hit, ": ");
            if (c && c < p + len) exc_add("module", where, c + 2);
        } else if (!strncmp(p, "[js] module fetch FAILED: ", 26)) {
            char m[400];
            snprintf(m, sizeof m, "%.*s", len - 26, p + 26);
            exc_add("fetch", where, m);
        }
        if (!nl) break;
        p = nl + 1;
    }
}

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

    /* THE PAGE URL IS READ FIRST, and that ordering is load-bearing now: the
     * manifest's absolute column is each src resolved against it, and the
     * module loader can only be answered from that column. Read after
     * load_manifest, as it used to be, every module URL resolves against an
     * empty base and the routing table is empty. */
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
    if (g_base_override[0]) snprintf(url, sizeof url, "%s", g_base_override);
    if (!url[0]) snprintf(url, sizeof url, "http://fixture.invalid/");
    snprintf(g_pagebase, sizeof g_pagebase, "%s", url);
    snprintf(g_fixdir, sizeof g_fixdir, "%s", dir);

    load_manifest(dir);
    g_nscr = 0;
    g_nexc = 0; g_outseen = 0;
    g_fetch_ok = g_fetch_miss = 0; g_nmodmiss = 0;
    g_ndropped = 0;
    /* js_page.c's console buffer OUTLIVES js_page_close -- it is a static, and
     * clearing it is the embedder's job. Without this line the first fixture's
     * errors are re-read under the second fixture's name: `example`, which has
     * no scripts at all, was credited with two uncaught exceptions. A report
     * that attributes one page's failures to another is worse than no report. */
    js_page_output_clear();
    g_conlen = 0; if (g_con) g_con[0] = 0;
    js_page_set_note_sink(con_sink);
    struct node *root = dom_parse(html, hlen);
    if (root) collect(root, dir);

    int c1_ok = 0, c2_ok = 0, nclassic = 0, nmod = 0, mod_ok = 0;
    for (int i = 0; i < g_nscr; i++) if (!g_scr[i].module) nclassic++;
    nmod = g_nscr - nclassic;
    /* The header goes out BEFORE anything runs, so the per-script errors
     * printed below sit under the site they belong to. Printed after, they
     * appeared under the previous fixture's name, which is a report that lies. */
    printf("  %-11s %2d scripts (%d classic, %d module) from %d bytes of HTML\n",
           g_sitename[g_site], g_nscr, nclassic, nmod, hlen);

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
    js_module_reset();
    if (js_page_open(root)) {
        /* TWO PASSES, because that is the order the spec gives and browser.c
         * follows: classic scripts in document order, then modules -- every
         * <script type=module> is implicitly `defer`. A probe that ran them
         * interleaved would report failures that come from its own ordering. */
        for (int i = 0; i < g_nscr; i++) {
            if (g_scr[i].module) continue;
            JSContext *ctx = js_page_ctx();
            JSValue v = JS_Eval(ctx, g_scr[i].data, (size_t)g_scr[i].len, g_scr[i].url,
                                JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                JSValue e = JS_GetException(ctx);
                const char *m = JS_ToCString(ctx, e);
                if (m) {
                    note_c2_error(m);
                    exc_add("script", g_scr[i].url, m);
                    if (show_errors) printf("    [c2] %-16s %.150s\n", g_scr[i].url, m);
                    JS_FreeCString(ctx, m);
                }
                JS_FreeValue(ctx, e);
            } else c2_ok++;
            JS_FreeValue(ctx, v);
        }
        /* ---- the module pass. js_module_eval is the browser's own entry
         * point, loader and all; the only thing this file supplies is the
         * bfetch under it, which reads the committed fixture instead of a
         * socket. Its diagnostics come back through the printf tee. */
        for (int i = 0; i < g_nscr; i++) {
            if (!g_scr[i].module) continue;
            cap_start();
            int ok = js_module_eval(g_scr[i].data, g_scr[i].len, g_scr[i].abs);
            cap_stop();
            harvest_module_output(g_scr[i].url);
            if (ok) mod_ok++;
            if (show_errors && !ok) printf("    [c2] %-16s module did not evaluate\n", g_scr[i].url);
            /* Turn the loop between modules: a dynamic import() settles as a
             * job, and the chunk it pulls in is code this instrument exists to
             * measure. Without this the fan-out is invisible. */
            cap_start();
            run_event_loop(show_errors, "c2", 1);
            cap_stop();
            harvest_module_output(g_scr[i].url);
        }
        cap_start();
        run_event_loop(show_errors, "c2", 1);
        cap_stop();
        harvest_module_output("<loop>");
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
        /* Modules run in channel 1 too, UNWRAPPED, and the reason has to be
         * stated because it is a hole in this channel that cannot be closed:
         * `with` is a syntax error in module code and would be meaningless
         * anyway -- a module has its own scope, so an unresolved import is a
         * link error and an unresolved free variable is a plain ReferenceError.
         * So a module contributes nothing to the bare-global histogram. It
         * still contributes everything to the MEMBER histogram, because
         * PROBE_MEMBERS replaced navigator/performance/document with recording
         * proxies on the global object, and a module reads those through the
         * same globals a classic script does. Given round one's finding -- that
         * the missing bare globals were overwhelmingly the pages' own names and
         * the real shortages were object PROPERTIES -- that is the half worth
         * having. */
        for (int i = 0; i < g_nscr; i++) {
            if (!g_scr[i].module) continue;
            cap_start();
            js_module_eval(g_scr[i].data, g_scr[i].len, g_scr[i].abs);
            run_event_loop(show_errors, mode == 2 ? "c1-deep" : "c1", 0);
            cap_stop();
        }
        /* Still recording: a global first reached from a timer counts, and is
         * exactly the class of miss the first version of this probe could not
         * see. See run_event_loop. */
        run_event_loop(show_errors, mode == 2 ? "c1-deep" : "c1", 0);
        js_page_close();
        g_recording = 0;
    }

    /* Modules are IN the denominator now. Two separate numbers, because they
     * are two separate claims: `modules` counts the top-level <script
     * type=module> tags that evaluated with no uncaught exception, while
     * `chunks` counts what the loader pulled in behind them -- a page with one
     * module tag and 134 imports is one of the first and 134 of the second, and
     * only the second says whether the graph was walked. */
    printf("  %-11s ... ran clean: c1 %d/%d classic, c2 %d/%d classic, %d/%d modules"
           "  (chunks loaded %d, missing %d)\n",
           g_sitename[g_site], c1_ok, nclassic, c2_ok, nclassic, mod_ok, nmod,
           g_fetch_ok, g_fetch_miss);
    /* A missing chunk is a hole in the CORPUS, printed as one. It is the single
     * easiest way for this instrument to lie: an import that 404s makes a page
     * look broken in a way the browser is not responsible for. */
    for (int i = 0; i < g_nmodmiss; i++)
        printf("  %-11s ... NOT IN FIXTURE (import): %s\n", g_sitename[g_site], g_modmiss[i]);
    for (int i = 0; i < g_ndropped; i++)
        printf("  %-11s ... NOT IN FIXTURE (<script src>): %s\n", g_sitename[g_site], g_dropped[i]);

    int nun = 0, nnet = 0, nwarn = 0;
    for (int i = 0; i < g_nexc; i++) {
        if (!strcmp(g_exc[i].kind, "fetch")) continue;         /* corpus gap */
        if (!strcmp(g_exc[i].kind, "netstub")) { nnet++; continue; }
        if (!strcmp(g_exc[i].kind, "warn")) { nwarn++; continue; }
        nun++;
    }
    printf("  %-11s ... UNCAUGHT: %d   (+%d from the host build's absent network,"
           " +%d console.warn)\n", g_sitename[g_site], nun, nnet, nwarn);
    g_site_exc[g_site] = nun;
    if (g_json) {
        for (int i = 0; i < g_nexc; i++) {
            printf("#JSON\t%s\t%s\t%s\t", g_sitename[g_site], g_exc[i].kind, g_exc[i].where);
            for (const char *s = g_exc[i].msg; *s; s++) printf("%c", *s == '\t' ? ' ' : *s);
            printf("\n");
        }
    }

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
        else if (!strcmp(argv[i], "--json")) g_json = 1;
        else if (!strncmp(argv[i], "--base=", 7))
            snprintf(g_base_override, sizeof g_base_override, "%s", argv[i] + 7);
        else if (nd < SITEMAX) dirs[nd++] = argv[i];
    }
    if (!nd) {
        printf("usage: webapi_probe [--errors] [--deep] [--scripts] [--json] "
               "[--base=URL] <fixture-dir>...\n");
        return 2;
    }

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
    printf("\nUNCAUGHT EXCEPTIONS PER PAGE (the number that matters -- a name is\n"
           "a symptom, this is what a user gets):\n");
    int total = 0;
    for (int s = 0; s < g_nsite; s++) {
        printf("  %-12s %d\n", g_sitename[s], g_site_exc[s]);
        total += g_site_exc[s];
    }
    printf("  %-12s %d\n", "TOTAL", total);

    printf("\n%d distinct names missed.\n", g_nmiss);
    printf("PAGES/REFS  the plain run: what a page reaches for on its real execution path.\n");
    printf("DEEP/D-REFS --deep only: what it would reach for next, past the first TypeError.\n");
    printf("CH2=DIES    a script actually TERMINATED on this name, unwrapped, as it does today.\n");
    printf("a page marked * was seen only in the --deep run.\n");
    return 0;
}
