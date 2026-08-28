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
#include "js_stall.h"
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
/* WEAK: tests/unit/rust_host_shim.c supplies the same two stubs and is in this
 * link too. Which file wins is somebody else's Makefile line, and a strong
 * definition here turned a working link into `multiple definition of
 * img_register` the moment that line changed. */
__attribute__((__weak__)) void img_register(void *detect, void *decode)
{ (void)detect; (void)decode; }
__attribute__((__weak__)) void img_register_anim(void *detect, void *decode, void *anim)
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
    int  inl;                   /* inline: no src attribute */
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
/* The directory that URL path `/` names. Equal to g_fixdir unless --docroot
 * moved it up to the corpus root; see fs_map. */
static char g_fsroot[512];
static char g_docroot[512];       /* --docroot=DIR, empty when not given */
static char *slurp(const char *path, int *out_len);

/* ---- the fixture as a DIRECTORY TREE, when the manifest cannot answer ----
 *
 * WHY A SECOND WAY IN. capture.py flattens a captured site: every <script src>
 * becomes `s001.js` in one directory and manifest.txt is the only thing that
 * knows which URL that was. A corpus that was never captured has no manifest
 * and needs none, because its layout IS the routing table -- build/jsfb ships
 * `index.html` beside `src/Main.js`, and `<script src="src/Main.js">` is a path
 * on disk relative to the document. With a manifest as the only door, every one
 * of those pages probed as "0 scripts", which is the failure mode this file's
 * own header calls the worst one: an instrument that measures a page with the
 * application removed and reports it clean.
 *
 * IT IS A FALLBACK, NOT A REPLACEMENT, and the order is load-bearing. The
 * manifest is consulted first and wins, so no captured fixture changes meaning:
 * this runs only where the old code had already given up.
 *
 * THE RULE IS ONE LINE: the URL's PATH is a path under g_fsroot, the directory
 * that URL `/` names. Without --docroot that is the fixture directory itself
 * and the document sits at the root of its own origin, so `src/Main.js` and
 * `/src/Main.js` are the same file -- which is the whole of what a captured
 * fixture ever needed. With --docroot it is the corpus root, the document gets
 * the URL it is really served from, and a ROOT-RELATIVE src resolves the way a
 * browser resolves it. That second case is not hypothetical: laminar's entire
 * application is `<script src="/frameworks/keyed/laminar/bundled-dist/assets/
 * index-84d215d4.js">`, and against the fixture directory alone it is
 * unresolvable -- the probe read the page as having no scripts at all.
 *
 * THREE REFUSALS, because a file that opens is not the same as a file that was
 * asked for:
 *   - a different ORIGIN never maps. `https://cdn.example/x.js` on a page from
 *     `https://www.bing.com/` is not `<fixture>/x.js`, and answering it from the
 *     fixture would invent a same-origin script the browser would never have.
 *   - a path that climbs out of g_fsroot is refused outright rather than
 *     clamped, so `../../etc` cannot read a neighbouring fixture and be counted
 *     as this one's. bfetch_resolve has already removed dot segments, so what
 *     is left here is the residue that escaped the root, which is a refusal.
 *   - the file must actually OPEN. If it does not, this returns 0 and the caller
 *     falls through to the NOT IN FIXTURE line exactly as before. An
 *     unresolvable script stays NAMED; that property is why we know what to fix.
 *
 * Returns 1 and writes the on-disk path; 0 means "not mine, say so out loud". */
static const char *url_path(const char *u, int *hostlen)
{
    const char *c = strstr(u, "://");
    if (!c) { if (hostlen) *hostlen = 0; return u; }
    const char *sl = strchr(c + 3, '/');
    if (hostlen) *hostlen = sl ? (int)(sl - u) : (int)strlen(u);
    return sl ? sl : "/";
}

static int fs_map(const char *url, char *out, int max)
{
    if (!g_fsroot[0] || !url || !url[0]) return 0;
    int uh = 0, bh = 0;
    const char *up = url_path(url, &uh);
    (void)url_path(g_pagebase, &bh);
    if (uh != bh || (uh && strncmp(url, g_pagebase, (size_t)uh))) return 0;

    if (up[0] != '/') return 0;
    const char *rel = up + 1;

    int n = 0;                                  /* the query is not a path */
    while (rel[n] && rel[n] != '?' && rel[n] != '#') n++;
    char relbuf[600];
    if (n <= 0 || n >= (int)sizeof relbuf) return 0;
    memcpy(relbuf, rel, (size_t)n);
    relbuf[n] = 0;
    if (relbuf[0] == '/') return 0;
    if (!strncmp(relbuf, "..", 2) && (relbuf[2] == 0 || relbuf[2] == '/')) return 0;
    if (strstr(relbuf, "/../") || strstr(relbuf, "//")) return 0;

    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_fsroot, relbuf);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    snprintf(out, max, "%s", p);
    return 1;
}

int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{
    char abs[600];
    bfetch_resolve(0, ref, abs, sizeof abs);
    const char *file = route(abs);
    char p[1200];
    if (file) snprintf(p, sizeof p, "%s/%s", g_fixdir, file);
    else if (!fs_map(abs, p, sizeof p)) {
        g_fetch_miss++;
        if (g_nmodmiss < MODMISS_MAX) snprintf(g_modmiss[g_nmodmiss++], 256, "%s", abs);
        return -1;
    }
    int len = 0;
    char *d = slurp(p, &len);
    /* A manifest entry naming a file that is not there used to increment the
     * miss counter and vanish. Same rule as everywhere else in this file: it
     * gets named. */
    if (!d) {
        g_fetch_miss++;
        if (g_nmodmiss < MODMISS_MAX) snprintf(g_modmiss[g_nmodmiss++], 256, "%s", abs);
        return -1;
    }
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
            int mi = -1;
            for (int i = 0; i < g_nman; i++)
                if (!strcmp(g_man[i].src, src)) { mi = i; break; }
            /* The manifest first and the directory second -- see fs_map. A
             * captured fixture answers from the manifest and nothing about it
             * moves; a fixture that never went through capture.py has its src
             * attributes as paths on disk and is answered from there. */
            char p[1200], abs[600];
            p[0] = 0;
            if (mi >= 0) {
                snprintf(p, sizeof p, "%s/%s", dir, g_man[mi].file);
                snprintf(abs, sizeof abs, "%s", g_man[mi].abs);
            } else {
                bfetch_resolve(g_pagebase, src, abs, sizeof abs);
                if (!fs_map(abs, p, sizeof p)) p[0] = 0;
            }
            int len = 0;
            char *d = p[0] ? slurp(p, &len) : 0;
            if (d) {
                g_scr[g_nscr].data = d; g_scr[g_nscr].len = len;
                g_scr[g_nscr].module = module;
                snprintf(g_scr[g_nscr].url, sizeof g_scr[g_nscr].url, "%s",
                         mi >= 0 ? g_man[mi].file : src);
                snprintf(g_scr[g_nscr].abs, sizeof g_scr[g_nscr].abs, "%s", abs);
                g_nscr++;
            }
            /* A <script src> nothing could answer used to vanish without a
             * word, and the cost of that silence was the headline number:
             * kimi's ENTRY POINT is `index-h6DE6Ow7.js`, a module that fans out
             * to the rest of the application, and it is not in the capture. The
             * probe reported "kimi: 3 scripts, all clean". It was measuring a
             * page with the application removed. Both doors have now been tried,
             * so this is still the same claim: NOBODY here holds this file. */
            else if (g_ndropped < DROPMAX)
                snprintf(g_dropped[g_ndropped++], 256, "%s", src);
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
                    g_scr[g_nscr].inl = 1;
                    g_nscr++;
                }
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) collect(c, dir);
}

static unsigned long long g_now;
/* A GETTER, NOT A TICKER -- every other host harness that calls
 * js_page_set_clock (webapi_platform_test.c, worker_test.c, webapi_idb_test.c,
 * js_dom_test.c, wpt_test.c...) makes clock_fn return g_now unchanged and
 * advances g_now by hand from a `tick()` called BETWEEN synchronous evals.
 * This file used to be the one exception: `return g_now += 4` advanced the
 * clock as a side effect of being READ, and js_page.c's slice watchdog reads
 * the clock from INSIDE the QuickJS interrupt handler -- which fires every
 * JS_INTERRUPT_COUNTER_INIT=10,000 bytecodes, including in the middle of one
 * synchronous script/handler that never returns to this file's own loop.
 * js_page.c documents the invariant this broke, in its own words: "the clock
 * ... is frozen for the whole of a synchronous eval, and a time-only watchdog
 * provably never fires there." Under the old clock_fn it was NOT frozen: a
 * single click handler that ran 11,250+ interrupt checks (=112.5M bytecodes,
 * not the documented 45,000 REAL ms or 2e10-bytecode fuel backstop) tripped
 * the wall-time rail purely because it kept asking the fake clock what time it
 * was. That is what made js-framework-benchmark's `runlots` (10,000 rows, one
 * synchronous commit) throw `InternalError: interrupted` on 22 independent
 * implementations -- see tests/jsfb/BASELINE and CLAUDE.md's 2026-08-29 entry.
 * A profile of one of them (`keyed/laminar`, via `sample` on this host binary)
 * showed no single hot function and no quadratic: total handler-visible work
 * for 10,000 rows was a few hundred thousand DOM calls, nowhere near what
 * either rail is supposed to require. The apparatus was the bug (CLAUDE.md
 * rule 1), not the DOM or the interpreter. */
static unsigned long long clock_fn(void) { return g_now; }

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
 * So the probe now turns the loop. run_event_loop() below ticks the fake
 * clock 4 ms per PASS (not per read -- see clock_fn's comment), so a couple
 * of hundred passes cover most of a second of page time without anything
 * sleeping -- and a setInterval cannot spin forever, because the pass budget
 * is fixed. */
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

/* ---- --stall: what is the page still WAITING for at settle? --------------
 *
 * 0 = off, 1 = armed (js_stall_arm before the first script), 2 = bare (the
 * native census only, which cannot perturb what it measures).
 *
 * TWO MODES BECAUSE THE ARMED HALF HAS A KNOWN OBSERVER EFFECT and this is
 * where it is measured rather than asserted: arming attaches a settlement
 * handler to every fetch promise, which registers a reaction and therefore
 * raises `pending_awaited` by exactly the number of fetches in flight. Running
 * the same fixture both ways and diffing the census IS the control -- if the
 * bare and armed censuses differ by anything other than that, one of them is
 * wrong. See tests/stall.mk. */
static int g_stall;

static void stall_dump(const char *site)
{
    char buf[8192];
    JSContext *ctx = js_page_ctx();
    if (!g_stall || !ctx) return;
    int n = js_stall_report(ctx, buf, (int)sizeof buf);
    if (n <= 0) return;
    printf("  %-11s ... STALL %s | blocked=%s\n", site,
           g_stall == 1 ? "armed" : "bare",
           js_stall_blocked(ctx) ? "YES" : "no");
    for (const char *p = buf; *p; ) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > 0) printf("  %-11s ...   %.*s\n", site, len, p);
        if (!nl) break;
        p = nl + 1;
    }
}

static void run_event_loop(int show_errors, const char *tag, int record)
{
    for (int i = 0; i < LOOP_PASSES; i++) {
        g_now += 4;                    /* the only place the fake clock moves */
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

/* ---- --drive: a loaded page is not a USED page -------------------------
 *
 * Everything above measures a page up to the settle point: scripts evaluated,
 * modules linked, the loop turned until nothing more is due. That is loading.
 * An application does its real work AFTER that, when somebody presses
 * something -- and a page whose whole render path is behind a click is scored
 * "ran clean, 0 uncaught" by every line above while doing nothing at all. The
 * `_paint` reader in tests/fixtures/frameworks closed half of that gap by
 * reading the DOM back; this closes the other half by producing the INPUT.
 *
 * TWO PIECES, AND BOTH ARE DELIBERATELY CONTENT-FREE. Nothing here knows what
 * a corpus is or what it should assert:
 *
 *   --drive FILE   evaluate FILE as a classic script in the page's own context
 *                  at the settle point, in CHANNEL 2 ONLY. Channel 2 is the
 *                  unwrapped path the browser really takes; running a driver
 *                  under channel 1's `with (Proxy)` scope would measure the
 *                  probe's own suppression machinery, and it would also print
 *                  every result line twice.
 *   __probeClick(el)  dispatch a click AT el.
 *
 * WHY __probeClick IS NATIVE AND WHY IT IS NOT `el.click()`.
 * `el.click()` exists (js_semantics.c:997) and is a JS shim WE wrote: it
 * constructs one untrusted `click` event and runs the activation steps. A user
 * pressing a mouse button goes somewhere else entirely -- browser.c:3492/3540/
 * 3550 raises TRUSTED mousedown, mouseup and click through js_dom_dispatch(),
 * in that order, with detail=1 and buttons set. A driver built on click()
 * would be this instrument measuring another thing this line also wrote, and
 * would silently miss every handler bound to mousedown. So the primitive is
 * the browser's own C entry point, called with browser.c's own initialiser,
 * and the loop is turned afterwards because a real click is followed by a
 * frame. What it does NOT do is browser.c's control_activate() -- the default
 * action of a checkbox or a submit -- which is stated because it is a
 * limitation of the driver rather than of the engine.
 *
 * The return value is the one js_dom_dispatch gives: 1 if the default action
 * survived, 0 if a listener called preventDefault(). */
static char       *g_drive_src;
static int         g_drive_len;
static const char *g_drive_path;

static JSValue js_probe_click(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_FALSE;
    struct node *n = js_dom_node_from(argv[0]);
    if (!n) return JS_FALSE;
    struct js_event_init ji = { 0 };
    ji.bubbles = 1; ji.cancelable = 1; ji.detail = 1;
    ji.button = 0; ji.buttons = 1;
    js_dom_dispatch(n, "mousedown", &ji);
    ji.buttons = 0;
    js_dom_dispatch(n, "mouseup", &ji);
    int go = js_dom_dispatch(n, "click", &ji);
    /* A click is followed by a frame: timers, microtasks and the promise a
     * framework schedules its render on. Without this the driver would read
     * the DOM back before the application had touched it and report every
     * asynchronous renderer as broken. */
    run_event_loop(0, "drive", 1);
    return JS_NewBool(ctx, go);
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
    /* --docroot: the document is given the URL it is REALLY served from, which
     * is its path under the corpus root. Everything else follows from that --
     * a root-relative src resolves against the same root a browser would use,
     * and `/css/currentStyle.css` is the file the corpus actually ships. Only
     * used when no SOURCE and no --base said otherwise, because those are two
     * deliberate statements about the document's identity and this is a
     * default. */
    int under_docroot = 0;
    if (!url[0] && g_docroot[0]) {
        size_t rl = strlen(g_docroot);
        while (rl && g_docroot[rl - 1] == '/') rl--;
        if (!strncmp(dir, g_docroot, rl) && (dir[rl] == '/' || dir[rl] == 0)) {
            const char *rel = dir + rl;
            while (*rel == '/') rel++;
            snprintf(url, sizeof url, "http://fixture.invalid/%s%s",
                     rel, (*rel && rel[strlen(rel) - 1] == '/') ? "" : "/");
            under_docroot = 1;
        } else {
            /* A fixture outside the docroot is NAMED, not silently given the
             * root's URL: a page probed at the wrong URL resolves every
             * absolute src to the wrong file and reports the result as ours. */
            printf("  (%s is not under --docroot %s; using its own directory)\n",
                   dir, g_docroot);
        }
    }
    if (!url[0]) snprintf(url, sizeof url, "http://fixture.invalid/");
    snprintf(g_pagebase, sizeof g_pagebase, "%s", url);
    snprintf(g_fixdir, sizeof g_fixdir, "%s", dir);
    /* g_fsroot is what URL `/` names: the corpus root when one was given and
     * this fixture is under it, the fixture's own directory otherwise. */
    snprintf(g_fsroot, sizeof g_fsroot, "%s", under_docroot ? g_docroot : dir);

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
        /* BEFORE the first script: a tracker installed afterwards would miss
         * every fetch the page starts on its first line, which on an
         * application page is most of them. */
        if (g_stall == 1) js_stall_arm(js_page_ctx());
        /* TWO PASSES, because that is the order the spec gives and browser.c
         * follows: classic scripts in document order, then modules -- every
         * <script type=module> is implicitly `defer`. A probe that ran them
         * interleaved would report failures that come from its own ordering. */
        for (int i = 0; i < g_nscr; i++) {
            if (g_scr[i].module) continue;
            JSContext *ctx = js_page_ctx();
            /* The browser reaches a classic script through js_page_eval, which
             * sets document.currentScript around it. This file evaluates the
             * script itself so it can keep the exception OBJECT rather than the
             * printed message, so it has to do that part too -- without it the
             * probe measured currentScript as null on every script of every
             * page and would have reported a working feature as absent. */
            /* An INLINE script must be paired by position, not by URL: its
             * `abs` is the document's URL with a discriminator, which looks
             * like an external script's and matched nothing. */
            js_page_begin_script(g_scr[i].inl || !g_scr[i].abs[0]
                                 ? g_scr[i].url : g_scr[i].abs);
            JSValue v = JS_Eval(ctx, g_scr[i].data, (size_t)g_scr[i].len, g_scr[i].url,
                                JS_EVAL_TYPE_GLOBAL);
            js_page_end_script();
            if (JS_IsException(v)) {
                JSValue e = JS_GetException(ctx);
                const char *m = JS_ToCString(ctx, e);
                if (m) {
                    note_c2_error(m);
                    exc_add("script", g_scr[i].url, m);
                    if (show_errors) printf("    [c2] %-16s %.150s\n", g_scr[i].url, m);
                    JS_FreeCString(ctx, m);
                }
                /* THE STACK, not just the message.
                 *
                 * A timer exception has carried its stack since js_page.c
                 * started printing one; a top-level script exception did not,
                 * and that asymmetry is the difference between a diagnosis and
                 * a guess. baidu's whole page collapses from ONE throw --
                 * `TypeError: not a function` inside 144 KB of minified jQuery
                 * 1.10.2 -- and every other line of its report is the cascade.
                 * A message with no frames does not say which call, and 143 KB
                 * of 2013 minified JavaScript does not yield to reading. */
                if (show_errors && JS_IsError(ctx, e)) {
                    JSValue st = JS_GetPropertyStr(ctx, e, "stack");
                    const char *s = JS_ToCString(ctx, st);
                    if (s && s[0]) {
                        for (const char *p = s; *p; ) {
                            const char *nl = strchr(p, '\n');
                            int len = nl ? (int)(nl - p) : (int)strlen(p);
                            if (len > 0) printf("         %.*s\n", len, p);
                            if (!nl) break;
                            p = nl + 1;
                        }
                    }
                    if (s) JS_FreeCString(ctx, s);
                    JS_FreeValue(ctx, st);
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
        /* THE LIFECYCLE EVENTS, and they are here because their ABSENCE was
         * the instrument's first false positive.
         *
         * The stall report on the first run said `DOMContentLoaded listeners=1
         * dispatched=0 NEVER-DISPATCHED` on a page, which reads as the exact
         * silent stall this whole instrument exists to find -- and it was the
         * HARNESS. browser.c dispatches both events at the document root once
         * the scripts have run (browser.c, just after `load done:`); this probe
         * is a script runner and dispatched neither, so every page that defers
         * its work to `addEventListener('load', ...)` was reported as parked on
         * a gun that never fired. It is the browser's own sequence, copied
         * exactly -- bubbles on the first, off on the second -- because a
         * settle point that differs from the browser's measures a browser
         * nobody runs.
         *
         * Only under --stall: channel 2's existing numbers are a published
         * baseline and must not move because a diagnostic was added. */
        /* --drive JOINS --stall ON THIS BLOCK, and it is not a convenience.
         * An application that binds its buttons inside
         * `addEventListener('DOMContentLoaded', ...)` has bound nothing at the
         * settle point, so a driver that clicked without firing the lifecycle
         * would report every one of those as dead -- a harness result wearing
         * an engine result's clothes, which is this tree's first rule. The
         * published channel-2 numbers still cannot move, because a plain run
         * passes neither flag. */
        if (g_stall || g_drive_src) {
            struct js_event_init li;
            memset(&li, 0, sizeof li);
            li.bubbles = 1;
            js_dom_dispatch(js_dom_root(), "DOMContentLoaded", &li);
            li.bubbles = 0;
            js_dom_dispatch(js_dom_root(), "load", &li);
            /* pageshow JOINS this block for the same reason load did: browser.c
             * now fires it immediately after `load`, unconditionally, on every
             * page (see browser.c's load_once). It is NOT the bfcache-restore
             * event some page authors assume -- HTML requires it on every load
             * -- so a page whose bootstrap is `addEventListener('pageshow', ..)`
             * instead of `'load'` is exactly the DOMContentLoaded-vs-load trap
             * this comment already tells the story of, one event later.
             *
             * pagehide, deliberately, does NOT join it. browser.c only ever
             * fires pagehide when NAVIGATING AWAY from a live document (see
             * load_once) -- closing the browser outright (EV_CLOSE) does not
             * fire it either, which is the same asymmetry a real desktop
             * browser has between "back/forward or a typed URL" and "quit".
             * This probe's per-site teardown is the latter shape: one process,
             * one site, then js_page_close() -- there is no second document
             * being navigated INTO. Dispatching pagehide here would be firing
             * an event for a navigation that never happened, which is exactly
             * the fabrication rule 3 forbids; a page that registers pagehide
             * and never gets it in this harness is reporting the harness's
             * shape correctly, not a bug. */
            js_dom_dispatch(js_dom_root(), "pageshow", &li);
            cap_start();
            run_event_loop(show_errors, "c2", 1);
            cap_stop();
            harvest_module_output("<lifecycle>");
        }
        /* THE SETTLE POINT. run_event_loop has just returned because nothing
         * ran and nothing is due -- which is exactly the browser's definition
         * of a page that has finished loading. Whatever is outstanding here is
         * outstanding forever. */
        stall_dump(g_sitename[g_site]);
        /* ---- the driver, at the settle point and in channel 2 only.
         * Its exceptions are the PAGE's ledger entries under `drive`, not a
         * separate bucket, because a driver that pressed a button and got a
         * TypeError has found exactly the thing this instrument exists to
         * count. A throw in the driver's own body is reported and named as
         * such -- an instrument that cannot say "the fault was mine" is the
         * one that reports its own bugs as the machine's. */
        if (g_drive_src && js_page_ctx()) {
            JSContext *ctx = js_page_ctx();
            JSValue g = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, g, "__probeClick",
                              JS_NewCFunction(ctx, js_probe_click, "__probeClick", 1));
            JS_FreeValue(ctx, g);
            cap_start();
            js_page_begin_script("<drive>");
            JSValue v = JS_Eval(ctx, g_drive_src, (size_t)g_drive_len,
                                g_drive_path ? g_drive_path : "<drive>",
                                JS_EVAL_TYPE_GLOBAL);
            js_page_end_script();
            if (JS_IsException(v)) {
                JSValue e = JS_GetException(ctx);
                const char *m = JS_ToCString(ctx, e);
                if (m) {
                    exc_add("drive", "<drive>", m);
                    printf("  %-11s ... DRIVER THREW: %.200s\n", g_sitename[g_site], m);
                    JS_FreeCString(ctx, m);
                }
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, v);
            run_event_loop(show_errors, "drive", 1);
            cap_stop();
            harvest_module_output("<drive>");
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
        else if (!strcmp(argv[i], "--stall")) g_stall = 1;
        else if (!strcmp(argv[i], "--stall-bare")) g_stall = 2;
        /* --drive FILE. A file that cannot be read is a REFUSAL, not a silent
         * plain run: the caller asked for a driven measurement and would
         * otherwise get an undriven one under the driven one's name. */
        else if (!strcmp(argv[i], "--drive") && i + 1 < argc) {
            g_drive_path = argv[++i];
            g_drive_src = slurp(g_drive_path, &g_drive_len);
            if (!g_drive_src) {
                fprintf(stderr, "webapi_probe: --drive %s: cannot read\n", g_drive_path);
                return 2;
            }
        }
        else if (!strncmp(argv[i], "--base=", 7))
            snprintf(g_base_override, sizeof g_base_override, "%s", argv[i] + 7);
        /* --docroot=DIR: the directory that URL `/` names, for a corpus whose
         * documents sit in subdirectories of one served tree. See probe_site
         * and fs_map. Without it every fixture is the root of its own origin,
         * which is what a captured fixture is and what this always did. */
        else if (!strncmp(argv[i], "--docroot=", 10))
            snprintf(g_docroot, sizeof g_docroot, "%s", argv[i] + 10);
        else if (nd < SITEMAX) dirs[nd++] = argv[i];
    }
    if (!nd) {
        printf("usage: webapi_probe [--errors] [--deep] [--scripts] [--json] "
               "[--stall|--stall-bare] [--base=URL] [--docroot=DIR] "
               "[--drive FILE] [NAME=]<fixture-dir>...\n");
        return 2;
    }

    printf("== webapi_probe: global lookups that MISS, across %d fixtures ==\n\n", nd);
    static char names[SITEMAX][64];
    for (int i = 0; i < nd; i++) {
        /* NAME=PATH. The label defaults to the directory's basename, which is
         * ambiguous the moment two fixtures' documents live in identically
         * named subdirectories -- js-framework-benchmark has two whose
         * documents are both `bundled-dist/`, and a report with two rows called
         * `bundled-dist` is a report that lies about which one failed. A caller
         * that knows the real name says it.
         *
         * THE RULE, so it can be worked around rather than guessed at: an
         * argument is read as NAME=PATH when it contains an `=` and does NOT
         * begin with `/`, `.` or `~`. A directory whose own name contains an
         * `=` is therefore passed as `./that=dir` or by absolute path, and is
         * then a directory again. */
        const char *arg = dirs[i], *eq = strchr(dirs[i], '=');
        const char *label = 0;
        static char lbuf[SITEMAX][64];
        if (eq && eq != dirs[i] && eq[1]
            && dirs[i][0] != '/' && dirs[i][0] != '.' && dirs[i][0] != '~') {
            int ln = (int)(eq - dirs[i]);
            if (ln > (int)sizeof lbuf[i] - 1) ln = (int)sizeof lbuf[i] - 1;
            memcpy(lbuf[i], dirs[i], (size_t)ln);
            lbuf[i][ln] = 0;
            label = lbuf[i];
            arg = eq + 1;
        }
        /* $(dir ...) hands these over with a trailing slash, so the basename has
         * to be taken after trimming it -- otherwise every site is named "". */
        char t[512];
        snprintf(t, sizeof t, "%s", arg);
        int n = (int)strlen(t);
        while (n > 1 && (t[n - 1] == '/' || t[n - 1] == '\\')) t[--n] = 0;
        char *base = strrchr(t, '/');
        if (!base) base = strrchr(t, '\\');
        snprintf(names[i], sizeof names[i], "%s", label ? label : (base ? base + 1 : t));
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
