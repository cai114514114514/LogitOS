/* reftest -- the WPT reftest runner.
 *
 * A reftest is a page carrying <link rel="match" href="...">. Both pages render
 * and the test passes when the two renderings are IDENTICAL, pixel for pixel.
 * The elegance is that both sides go through the same engine, so antialiasing,
 * glyph rasterization and rounding cancel: a correct engine matches exactly even
 * if its text looks nothing like Chrome's. Exact comparison is therefore the
 * DEFAULT here, not a tolerance -- `fuzzy` is honoured where the corpus asks for
 * it, and the report separates the tests that pass only because of it, because
 * that number is how you find out whether a rate is real.
 *
 * The model is tests/unit/h264 and the Makefile comment that goes with it:
 * bit-exactness is the bar, and `make test-h264-diff` prints per-case wrong-byte
 * totals because "the first mismatch moved" says nothing. So every failing test
 * here carries a WRONG-PIXEL COUNT you can watch shrink, and --diffdir writes
 * the three images (test, reference, amplified difference) for any test you want
 * to look at.
 *
 * WHAT CODE THIS SHARES WITH browser.aex: see tests/unit/refhost/logit.h, which
 * states it in full. The short version is that the pipeline, the rasterizer, the
 * shaper and text_measure are the real linked code, and what is not shared is
 * wm.c's six-line syscall cases, the window chrome, and which font file the
 * loader is pointed at.
 *
 * CRASH ISOLATION IS NOT OPTIONAL. 24,612 tests against a layout engine that has
 * never seen most of them will find a way to segfault, and a runner that dies on
 * test 300 measures nothing. Every test renders in a FORKED CHILD under an
 * alarm, so a crash and a hang each become a recorded cause with a count, which
 * is a finding rather than an outage.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

#include "refhost.h"
#include "refrender.h"
#include "refmanifest.h"

/* ------------------------------------------------------------- verdicts -- */
enum {
    V_EXACT = 0,   /* identical, no tolerance used */
    V_FUZZY,       /* matched only within the page's own fuzzy annotation */
    V_FAIL,        /* rendered, and differs */
    V_SKIP,        /* flags say no static pixel comparison can judge it */
    V_NOREF,       /* the reference file is not in this checkout */
    V_ERR,         /* the test or the reference would not render */
    V_CRASH,       /* the child died on a signal */
    V_TIMEOUT,     /* the child ran past the alarm */
    V_NVERD
};
static const char *vname[V_NVERD] = {
    "exact", "fuzzy", "fail", "skip", "noref", "err", "crash", "timeout"
};

/* What comes back from the child, through a pipe. Fixed size, no pointers. */
struct verdict {
    int  code;
    long diffpx;      /* pixels differing, best over the candidate references */
    int  maxdelta;    /* largest per-channel delta on that best reference */
    long total;       /* w*h, so a percentage can be printed */
    int  used_fuzz;   /* the fuzzy bound that let it pass, or -1 */
    /* INK: pixels in the test rendering that differ from its own background.
     * This exists because of what negative control 2 found -- see the comment
     * above opt_nocss. A pass with no ink on either side is two blank pages
     * agreeing, which is not evidence of anything, and it must be counted
     * apart from the rate or the rate is a lie told with real arithmetic. */
    long ink, ref_ink;
    /* NON-DISCRIMINATING: this test also passes with its own author CSS
     * removed. See the comment above opt_nocss. A pass that survives having the
     * stylesheet deleted is not evidence that the cascade or layout is right --
     * it is a test whose two sides our engine renders identically for reasons
     * that have nothing to do with what the test is about. This is the per-test
     * form of negative control 2, run on every passing test, and it is the
     * difference between a pass rate and a pass rate worth quoting. */
    int  nondiscriminating;
    /* 1 when every reference on this page is rel=match. Needed by the
     * always-equal control: see the match_fail counter in main(). */
    int  all_match;
    char cause[64];   /* classification, filled by the parent for failures */
};

/* --------------------------------------------------------------- config -- */
static const char *opt_root = "third_party/wpt";
static const char *opt_filter = "css/";
static const char *opt_diffdir;
static const char *opt_manifest;
static const char *opt_ahem = ".cache/ahem/Ahem.ttf";
static const char *opt_ui, *opt_mono;
static int opt_limit = 0;
static int opt_verbose = 0;
static int opt_always_equal = 0;   /* negative control 1 */
/* NEGATIVE CONTROL 2, and it took two tries to get right -- the first version is
 * kept because what it found is worth more than the control was.
 *
 *   opt_nocss == 1  withhold the author CSS from BOTH sides.
 *   opt_nocss == 2  withhold it from the TEST ONLY.
 *
 * The brief asked for "CSS ignored entirely, nearly everything must fail", and
 * the obvious reading is mode 1. Mode 1 does not fail. It scores HIGHER than a
 * normal run -- 80.33% against 38.33% on the first 300 -- because stripping the
 * stylesheet off both pages degrades them to the SAME unstyled HTML, and two
 * pages that both render as nearly nothing are pixel-identical. That is not a
 * broken control; it is the control working, and reporting that a large part of
 * the apparent pass rate is two blank pages agreeing with each other.
 *
 * So mode 2 is the actual control (perturb ONE side; the rate must collapse),
 * mode 1 is retained as a diagnostic, and the vacuity it exposed is now measured
 * on every ordinary run through struct verdict's `ink`. A reftest suite over an
 * engine that renders nothing is the exact analogue of the always-equal
 * comparator, arriving by a different road, and nothing in the WPT design
 * catches it -- real browsers do not have the failure mode, so the upstream
 * harness never needed the check. */
static int opt_nocss = 0;
static int opt_timeout = 20;
static int opt_tentative = 0;      /* include .tentative. in the headline rate */
static int opt_realfont = 0;

/* ---------------------------------------------------- cause classification -
 * A ranked failure table is worth more than the pass rate, because it decides
 * the order of the layout work. Two groupings, deliberately:
 *
 *   the WPT directory   completely objective, no judgement, no heuristic
 *   a feature probe     actionable, and a heuristic, and labelled as one
 *
 * The feature probe reads the TEST SOURCE, in order, and reports the first
 * marker it finds. Order matters: a grid test that also uses a float is a grid
 * test. This is not a claim about the root cause of any single failure -- it is
 * a way to sort 20,000 failures into buckets a person can act on. */
static const struct { const char *marker; const char *name; } causes[] = {
    { "display: grid",        "grid" },
    { "display:grid",         "grid" },
    { "display: inline-grid", "grid" },
    { "display:inline-grid",  "grid" },
    { "grid-template",        "grid" },
    { "display: flex",        "flex" },
    { "display:flex",         "flex" },
    { "display: inline-flex", "flex" },
    { "display:inline-flex",  "flex" },
    { "flex-direction",       "flex" },
    { "writing-mode",         "writing-mode" },
    { "direction: rtl",       "bidi/rtl" },
    { "direction:rtl",        "bidi/rtl" },
    { "position: absolute",   "abspos" },
    { "position:absolute",    "abspos" },
    { "position: fixed",      "abspos" },
    { "position:fixed",       "abspos" },
    { "float:",               "float" },
    { "float: ",              "float" },
    { "display: table",       "table" },
    { "display:table",        "table" },
    { "<table",               "table" },
    { "transform:",           "transform" },
    { "transform: ",          "transform" },
    { "column-count",         "multicol" },
    { "column-width",         "multicol" },
    { "line-height",          "line-height" },
    { "vertical-align",       "vertical-align" },
    { "font-family",          "font" },
    { "border-radius",        "border-radius" },
    { "background-image",     "background-image" },
    { "linear-gradient",      "gradient" },
    { "overflow:",            "overflow" },
    { "overflow: ",           "overflow" },
    { "z-index",              "z-index" },
    { "::before",             "pseudo-element" },
    { "::after",              "pseudo-element" },
    { ":before",              "pseudo-element" },
    { ":after",               "pseudo-element" },
};

static void classify(const char *root, const char *rel, char *out, int outmax)
{
    char full[RM_PATHMAX * 2];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    FILE *f = fopen(full, "rb");
    if (!f) { snprintf(out, (size_t)outmax, "unreadable"); return; }
    static char buf[512 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0; fclose(f);
    /* lowercase in place so the markers can be written once */
    for (size_t i = 0; i < n; i++)
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] + 32);
    for (unsigned k = 0; k < sizeof causes / sizeof causes[0]; k++)
        if (strstr(buf, causes[k].marker)) {
            snprintf(out, (size_t)outmax, "%s", causes[k].name);
            return;
        }
    snprintf(out, (size_t)outmax, "block/inline");
}

/* The objective grouping: the WPT spec directory, e.g. "css/css-grid". */
static void specdir(const char *rel, char *out, int outmax)
{
    const char *s = rel, *slash1 = strchr(s, '/');
    if (!slash1) { snprintf(out, (size_t)outmax, "%s", rel); return; }
    const char *slash2 = strchr(slash1 + 1, '/');
    int n = slash2 ? (int)(slash2 - s) : (int)strlen(s);
    if (n >= outmax) n = outmax - 1;
    memcpy(out, s, (size_t)n); out[n] = 0;
}

/* ------------------------------------------------------- the comparison -- */
/* Render `rel` and hand back a private copy of the pixels: rr_render returns
 * the ONE shared surface, so rendering the reference would otherwise overwrite
 * the test that is about to be compared against it. That bug reports every test
 * as a perfect match, which is exactly the shape of the failure the always-equal
 * control is built to catch -- so it is worth the malloc. */
/* Pixels differing from the page's own background (taken as the top-left pixel,
 * which is background by construction unless the page paints the full viewport).
 * A crude measure on purpose: it only has to separate "drew something" from
 * "drew nothing", and anything cleverer would be a second renderer to be wrong
 * in a second way. */
static long count_ink(const uint32_t *px, long n)
{
    uint32_t bg = px[0] & 0xFFFFFFu;
    long k = 0;
    for (long i = 0; i < n; i++) if ((px[i] & 0xFFFFFFu) != bg) k++;
    return k;
}

static uint32_t *render_copy(const char *rel, int w, int h, int nocss)
{
    uint32_t *px = rr_render(opt_root, rel, w, h, nocss, 0);
    if (!px) return 0;
    size_t n = (size_t)w * h * 4;
    uint32_t *cp = (uint32_t *)malloc(n);
    if (!cp) return 0;
    memcpy(cp, px, n);
    return cp;
}

/* THE COMPARATOR. `opt_always_equal` is negative control #1 and it lives here,
 * one branch from the real answer, because that is where a comparator that
 * cannot fail would really live. The suite must go GREEN when it is on. */
static long compare(const uint32_t *a, const uint32_t *b, int w, int h, int *maxd)
{
    if (opt_always_equal) { *maxd = 0; return 0; }
    return refhost_cmp(a, b, w, h, maxd);
}

static void write_diff(const char *rel, const uint32_t *t, const uint32_t *r,
                       int w, int h)
{
    if (!opt_diffdir) return;
    char safe[RM_PATHMAX], path[RM_PATHMAX * 2];
    snprintf(safe, sizeof safe, "%s", rel);
    for (char *p = safe; *p; p++) if (*p == '/') *p = '_';
    mkdir(opt_diffdir, 0755);
    snprintf(path, sizeof path, "%s/%s.test.png", opt_diffdir, safe);
    refhost_png(path, t, w, h);
    snprintf(path, sizeof path, "%s/%s.ref.png", opt_diffdir, safe);
    refhost_png(path, r, w, h);
    uint32_t *d = (uint32_t *)malloc((size_t)w * h * 4);
    if (d) {
        refhost_diffimg(t, r, d, w * h);
        snprintf(path, sizeof path, "%s/%s.diff.png", opt_diffdir, safe);
        refhost_png(path, d, w, h);
        free(d);
    }
}

/* Judge one test. Runs INSIDE the forked child. */
static void judge(const struct rm_test *t, struct verdict *v)
{
    int w = RR_VIEW_W, h = RR_VIEW_H;
    memset(v, 0, sizeof *v);
    v->total = (long)w * h;
    v->used_fuzz = -1;
    v->diffpx = v->total;

    v->all_match = 1;
    for (int i = 0; i < t->nrefs; i++) if (t->refs[i].mismatch) v->all_match = 0;

    int unjudgeable = t->flags & (RM_F_INTERACT | RM_F_ANIMATED | RM_F_PAGED |
                                  RM_F_SPEECH | RM_F_HTTP | RM_F_USERSTYLE | RM_F_ASIS);
    if (unjudgeable) { v->code = V_SKIP; return; }

    /* mode 2 strips CSS from the test only; mode 1 from both. */
    int css_test = opt_nocss ? 1 : 0;
    int css_ref  = (opt_nocss == 1) ? 1 : 0;

    uint32_t *tp = render_copy(t->path, w, h, css_test);
    if (!tp) { v->code = V_ERR; return; }
    v->ink = count_ink(tp, w * h);

    int best = V_FAIL; long bestdiff = v->total; int bestmax = 255, bestfuzz = -1;
    uint32_t *bestref = 0;
    int any_ref = 0;

    for (int i = 0; i < t->nrefs; i++) {
        const struct rm_ref *r = &t->refs[i];
        uint32_t *rp = render_copy(r->path, w, h, css_ref);
        if (!rp) continue;
        any_ref = 1;
        long rink = count_ink(rp, w * h);
        int md = 0;
        long d = compare(tp, rp, w, h, &md);

        int code;
        if (r->mismatch) {
            /* rel=mismatch: the two must DIFFER. Note that always-equal makes
             * every mismatch test fail, which is the correct and useful
             * behaviour -- the control proves the comparator is load-bearing in
             * both directions, and the report says how many of each there are. */
            code = (d > 0) ? V_EXACT : V_FAIL;
        } else if (d == 0) {
            code = V_EXACT;
        } else if (r->fuzz_maxdiff >= 0 &&
                   md <= r->fuzz_maxdiff && d <= r->fuzz_maxpixels) {
            code = V_FUZZY;
        } else {
            code = V_FAIL;
        }

        /* Any one reference matching is a pass; keep the best outcome and, for
         * the diff artefact, the reference that came closest. */
        if (code < best || (code == best && d < bestdiff)) {
            best = code; bestdiff = d; bestmax = md;
            bestfuzz = (code == V_FUZZY) ? r->fuzz_maxdiff : -1;
            v->ref_ink = rink;
            free(bestref); bestref = rp; rp = 0;
        }
        free(rp);
        if (best == V_EXACT) break;
    }

    if (!any_ref) { v->code = V_NOREF; free(tp); return; }
    v->code = best; v->diffpx = bestdiff; v->maxdelta = bestmax; v->used_fuzz = bestfuzz;
    if (best == V_FAIL && bestref) write_diff(t->path, tp, bestref, w, h);

    /* The discrimination check. Only for passes, and only when CSS was in play
     * at all: re-render the TEST with its author stylesheet withheld and ask
     * whether it still matches the same reference. If it does, deleting the
     * stylesheet changed nothing our comparator can see, and the pass says
     * nothing about CSS. */
    if ((best == V_EXACT || best == V_FUZZY) && !opt_nocss && bestref && !t->refs[0].mismatch) {
        uint32_t *bare = render_copy(t->path, w, h, 1);
        if (bare) {
            int md2 = 0;
            long d2 = compare(bare, bestref, w, h, &md2);
            if (d2 == 0) v->nondiscriminating = 1;
            free(bare);
        }
    }
    free(tp); free(bestref);
}

/* ------------------------------------------------------ fork + supervise -- */
static void alarm_die(int s) { (void)s; _exit(70); }

static void run_one(const struct rm_test *t, struct verdict *out)
{
    int fd[2];
    if (pipe(fd) != 0) { memset(out, 0, sizeof *out); out->code = V_ERR; return; }
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); memset(out, 0, sizeof *out); out->code = V_ERR; return; }
    if (pid == 0) {
        close(fd[0]);
        signal(SIGALRM, alarm_die);
        alarm((unsigned)opt_timeout);
        struct verdict v;
        judge(t, &v);
        ssize_t ignored = write(fd[1], &v, sizeof v);
        (void)ignored;
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    struct verdict v; memset(&v, 0, sizeof v);
    ssize_t got = read(fd[0], &v, sizeof v);
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (got != (ssize_t)sizeof v) {
        memset(&v, 0, sizeof v);
        v.total = (long)RR_VIEW_W * RR_VIEW_H;
        v.diffpx = v.total;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 70) v.code = V_TIMEOUT;
        else v.code = V_CRASH;
    }
    *out = v;
}

/* -------------------------------------------------------------- the list -- */
struct entry { char rel[RM_PATHMAX]; };
static struct entry *list; static int nlist, caplist;

static void collect_cb(const char *rel, void *ud)
{
    (void)ud;
    if (rm_is_reference(rel)) return;
    if (nlist >= caplist) {
        caplist = caplist ? caplist * 2 : 4096;
        list = (struct entry *)realloc(list, (size_t)caplist * sizeof *list);
    }
    snprintf(list[nlist].rel, RM_PATHMAX, "%s", rel);
    nlist++;
}

static int cmp_entry(const void *a, const void *b)
{ return strcmp(((const struct entry *)a)->rel, ((const struct entry *)b)->rel); }

/* The manifest cache. Walking this checkout costs 95 seconds of pure I/O
 * against 0.6 seconds of CPU, so a runner that re-walks per invocation makes
 * bisecting a single test unusable. */
static int load_manifest(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    char line[RM_PATHMAX];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        if (opt_filter && !strstr(line, opt_filter)) continue;
        collect_cb(line, 0);
    }
    fclose(f);
    return nlist;
}

static void save_manifest(const char *path)
{
    FILE *f = fopen(path, "wb"); if (!f) return;
    for (int i = 0; i < nlist; i++) fprintf(f, "%s\n", list[i].rel);
    fclose(f);
}

/* ------------------------------------------------------------- baseline -- */
/* The ratchet, the same shape tests/wpt.mk uses: a committed list of tests
 * expected to fail. Green when nothing that passes today stops passing. The
 * rate is reported either way; only a REGRESSION is a non-zero exit. */
static char **basel; static int nbase;
static int base_has(const char *rel)
{
    int lo = 0, hi = nbase - 1;
    while (lo <= hi) { int m = (lo + hi) / 2, c = strcmp(basel[m], rel);
        if (!c) return 1; if (c < 0) lo = m + 1; else hi = m - 1; }
    return 0;
}
static int cmp_str(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void load_baseline(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return;
    char line[RM_PATHMAX];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        if (nbase >= cap) { cap = cap ? cap * 2 : 1024;
                            basel = (char **)realloc(basel, (size_t)cap * sizeof *basel); }
        basel[nbase++] = strdup(line);
    }
    fclose(f);
    qsort(basel, (size_t)nbase, sizeof *basel, cmp_str);
}

/* ------------------------------------------------------------ reporting -- */
struct bucket { char name[64]; long n; };
static struct bucket bk[512]; static int nbk;
static void bump(const char *name)
{
    for (int i = 0; i < nbk; i++) if (!strcmp(bk[i].name, name)) { bk[i].n++; return; }
    if (nbk >= 512) return;
    snprintf(bk[nbk].name, sizeof bk[nbk].name, "%s", name);
    bk[nbk].n = 1; nbk++;
}
static int cmp_bk(const void *a, const void *b)
{ long d = ((const struct bucket *)b)->n - ((const struct bucket *)a)->n;
  return d < 0 ? -1 : d > 0 ? 1 : 0; }

static struct bucket sd[512]; static int nsd;
static void bump_sd(const char *name)
{
    for (int i = 0; i < nsd; i++) if (!strcmp(sd[i].name, name)) { sd[i].n++; return; }
    if (nsd >= 512) return;
    snprintf(sd[nsd].name, sizeof sd[nsd].name, "%s", name);
    sd[nsd].n = 1; nsd++;
}

static void usage(void)
{
    printf(
"usage: reftest [options]\n"
"  --root DIR          WPT checkout           (default third_party/wpt)\n"
"  --filter SUB        only paths containing SUB (default css/)\n"
"  --one PATH          judge exactly one test, verbosely -- the bisect form\n"
"  --limit N           stop after N tests\n"
"  --manifest FILE     read the test list from FILE (written if absent)\n"
"  --baseline FILE     expected-failure list; regressions exit non-zero\n"
"  --write-baseline F  rewrite F from this run\n"
"  --diffdir DIR       write test/ref/diff PNGs for every failure\n"
"  --ahem PATH         Ahem.ttf to use as the UI font (default .cache/ahem)\n"
"  --realfont          use the SHIPPING fonts instead of Ahem\n"
"  --tentative         include .tentative. tests in the headline rate\n"
"  --always-equal      NEGATIVE CONTROL 1: comparator always reports equal\n"
"  --no-css-test       NEGATIVE CONTROL 2: withhold CSS from the TEST only\n"
"  --no-css            diagnostic: withhold CSS from BOTH sides (see the\n"
"                      comment on opt_nocss -- this one does NOT collapse)\n"
"  --timeout SEC       per-test alarm (default 20)\n"
"  -v                  list unexpected failures as they happen\n");
}

int main(int argc, char **argv)
{
    const char *one = 0, *baseline = 0, *write_base = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define ARG(n) (!strcmp(a, n) && i + 1 < argc)
        if      (ARG("--root"))           opt_root = argv[++i];
        else if (ARG("--filter"))         opt_filter = argv[++i];
        else if (ARG("--one"))            one = argv[++i];
        else if (ARG("--limit"))          opt_limit = atoi(argv[++i]);
        else if (ARG("--manifest"))       opt_manifest = argv[++i];
        else if (ARG("--baseline"))       baseline = argv[++i];
        else if (ARG("--write-baseline")) write_base = argv[++i];
        else if (ARG("--diffdir"))        opt_diffdir = argv[++i];
        else if (ARG("--ahem"))           opt_ahem = argv[++i];
        else if (ARG("--timeout"))        opt_timeout = atoi(argv[++i]);
        else if (!strcmp(a, "--realfont")) opt_realfont = 1;
        else if (!strcmp(a, "--tentative")) opt_tentative = 1;
        else if (!strcmp(a, "--always-equal")) opt_always_equal = 1;
        else if (!strcmp(a, "--no-css"))      opt_nocss = 1;
        else if (!strcmp(a, "--no-css-test")) opt_nocss = 2;
        else if (!strcmp(a, "-v"))        opt_verbose = 1;
        else if (!strcmp(a, "--all"))     opt_filter = 0;
        else { usage(); return 2; }
        #undef ARG
    }

    /* --- fonts. The Ahem answer, printed on every run, because a harness that
     * silently rendered no glyphs would pass a great many blank-vs-blank
     * comparisons and report a rate that means nothing. --- */
    if (opt_realfont) { opt_ui = "fsroot/fonts/ui.ttf"; opt_mono = "fsroot/fonts/mono.ttf"; }
    else              { opt_ui = opt_ahem;              opt_mono = opt_ahem; }
    refhost_font_map(opt_ui, opt_mono);
    if (refhost_fonts() != 0) {
        printf("reftest: FONT NOT USABLE (%s)\n", opt_ui);
        printf("  Every comparison would be blank against blank. Refusing to\n"
               "  report a rate. Fetch Ahem with `make reftest-ahem`.\n");
        return 1;
    }
    printf("font: %s  (%s)\n", opt_ui,
           opt_realfont ? "SHIPPING fonts -- Ahem-authored tests are not judged as authored"
                        : "Ahem as the UI font -- the corpus's own font");

    /* --- the single-test form, for bisecting --- */
    if (one) {
        struct rm_test t;
        if (rm_parse_file(opt_root, one, &t) <= 0) {
            printf("%s: not a reftest (no <link rel=match>)\n", one);
            return 2;
        }
        printf("test  : %s%s%s\n", t.path, t.tentative ? "  [tentative]" : "",
               t.xhtml ? "  [xhtml]" : "");
        for (int i = 0; i < t.nrefs; i++)
            printf("  %-8s %s   fuzzy=%d/%ld\n", t.refs[i].mismatch ? "mismatch" : "match",
                   t.refs[i].path, t.refs[i].fuzz_maxdiff, t.refs[i].fuzz_maxpixels);
        struct rr_stats st;
        if (rr_render(opt_root, t.path, RR_VIEW_W, RR_VIEW_H, opt_nocss, &st))
            printf("  render: %d nodes, %d css bytes, %d sheets (%d missing), "
                   "%d items, doc height %d\n",
                   st.nodes, st.css_bytes, st.sheets, st.sheets_missing, st.items, st.doc_h);
        struct verdict v;
        run_one(&t, &v);
        printf("verdict: %s   wrong pixels %ld / %ld (%.3f%%)  max channel delta %d\n",
               vname[v.code], v.diffpx, v.total, 100.0 * (double)v.diffpx / (double)v.total,
               v.maxdelta);
        if (v.code == V_FUZZY) printf("  (passed ONLY because of the page's fuzzy annotation)\n");
        if (opt_diffdir) printf("  images in %s/\n", opt_diffdir);
        return v.code == V_EXACT || v.code == V_FUZZY ? 0 : 1;
    }

    /* --- the list --- */
    if (opt_manifest && load_manifest(opt_manifest) > 0) {
        printf("manifest: %d candidates from %s\n", nlist, opt_manifest);
    } else {
        printf("walking %s ...\n", opt_root); fflush(stdout);
        rm_walk(opt_root, opt_filter, collect_cb, 0);
        qsort(list, (size_t)nlist, sizeof *list, cmp_entry);
        if (opt_manifest) { save_manifest(opt_manifest); printf("manifest written: %s\n", opt_manifest); }
        printf("walked: %d candidate files\n", nlist);
    }
    if (baseline) load_baseline(baseline);

    long count[V_NVERD]; memset(count, 0, sizeof count);
    long tent_skipped = 0, fuzz_passes = 0, regressions = 0, newpass = 0;
    long vacuous_pass = 0, nondisc_pass = 0, match_fail = 0;
    FILE *wb = write_base ? fopen(write_base, "wb") : 0;
    if (wb) fprintf(wb, "# generated by tests/unit/reftest.c --write-baseline\n"
                        "# every line is a test that FAILS today. Shrinking this file is the work.\n");
    FILE *fl = fopen("build/reftest/failures.txt", "wb");

    int judged = 0;
    for (int i = 0; i < nlist; i++) {
        struct rm_test t;
        if (rm_parse_file(opt_root, list[i].rel, &t) <= 0) continue;
        if (t.tentative && !opt_tentative) { tent_skipped++; continue; }
        if (opt_limit && judged >= opt_limit) break;
        judged++;

        struct verdict v;
        run_one(&t, &v);
        count[v.code]++;
        if (v.code == V_FUZZY) fuzz_passes++;

        int passed = (v.code == V_EXACT || v.code == V_FUZZY);
        /* A pass where NEITHER side drew anything is two blank pages agreeing.
         * Counted, and subtracted from the headline, because it is not evidence
         * that layout is right -- it is evidence that layout ran. The threshold
         * is a flat pixel count rather than a fraction: a reference that draws a
         * single 10x10 swatch is a real test, and 100 pixels is below that. */
        int vacuous = passed && v.ink < 100 && v.ref_ink < 100;
        if (vacuous) vacuous_pass++;
        if (passed && v.nondiscriminating) nondisc_pass++;
        /* The always-equal control's real assertion. A comparator stubbed to
         * equality cannot fail a rel=MATCH test -- but it must fail every
         * rel=MISMATCH test, since those demand the two renderings DIFFER. So
         * "100%" was the wrong bar and the first full-corpus run caught it at
         * 98.68%: the 558 mismatch tests were doing exactly what they should.
         * The bar is that no match-type test fails. */
        if (!passed && v.all_match && v.code != V_SKIP && v.code != V_NOREF &&
            v.code != V_ERR && v.code != V_CRASH && v.code != V_TIMEOUT)
            match_fail++;
        if (!passed) {
            char c[64]; classify(opt_root, t.path, c, sizeof c);
            bump(c);
            char d[64]; specdir(t.path, d, sizeof d); bump_sd(d);
            if (wb) fprintf(wb, "%s\n", t.path);
            if (fl) fprintf(fl, "%-70s %-9s %8ld px  %s\n", t.path, vname[v.code], v.diffpx, c);
            if (baseline && !base_has(t.path)) {
                regressions++;
                if (opt_verbose || regressions <= 20)
                    printf("REGRESSION %s (%s, %ld px wrong)\n", t.path, vname[v.code], v.diffpx);
            }
        } else if (baseline && base_has(t.path)) {
            newpass++;
        }

        if ((judged % 500) == 0) {
            long ok = count[V_EXACT] + count[V_FUZZY];
            fprintf(stderr, "  %d judged, %ld passing (%.2f%%)\r", judged, ok,
                    100.0 * (double)ok / (double)judged);
            fflush(stderr);
        }
    }
    if (wb) fclose(wb);
    if (fl) fclose(fl);

    long ok = count[V_EXACT] + count[V_FUZZY];
    long denom = judged - count[V_SKIP];
    if (denom < 1) denom = 1;

    printf("\n================ WPT reftest: %s ================\n",
           opt_filter ? opt_filter : "(all)");
    if (opt_always_equal) printf("*** NEGATIVE CONTROL: comparator always reports equal ***\n");
    if (opt_nocss == 1)   printf("*** DIAGNOSTIC: author CSS withheld from BOTH sides ***\n");
    if (opt_nocss == 2)   printf("*** NEGATIVE CONTROL: author CSS withheld from the TEST only ***\n");
    printf("judged                 %6d\n", judged);
    printf("  exact match          %6ld\n", count[V_EXACT]);
    printf("  fuzzy match          %6ld   <- passed ONLY within the page's own tolerance\n",
           count[V_FUZZY]);
    printf("  fail (pixels differ) %6ld\n", count[V_FAIL]);
    printf("  reference missing    %6ld\n", count[V_NOREF]);
    printf("  render error         %6ld\n", count[V_ERR]);
    printf("  crash                %6ld\n", count[V_CRASH]);
    printf("  timeout              %6ld\n", count[V_TIMEOUT]);
    printf("  skipped (flags)      %6ld   not judgeable by any static comparison\n", count[V_SKIP]);
    if (!opt_tentative) printf("  .tentative. excluded %6ld\n", tent_skipped);
    printf("\nPASS RATE  %ld / %ld  =  %.2f%%\n", ok, denom,
           100.0 * (double)ok / (double)denom);
    printf("  of which fuzzy-only: %ld  (%.2f%% of the corpus)\n", fuzz_passes,
           100.0 * (double)fuzz_passes / (double)denom);
    printf("  exact-only rate:     %.2f%%\n",
           100.0 * (double)count[V_EXACT] / (double)denom);
    /* The two numbers that decide whether the one above is worth anything. */
    printf("\n  VACUOUS passes       %6ld   both sides drew <100 pixels of ink\n", vacuous_pass);
    printf("  NON-DISCRIMINATING   %6ld   still pass with the test's own CSS deleted\n",
           nondisc_pass);
    long real_ok = ok - nondisc_pass;
    if (real_ok < 0) real_ok = 0;
    printf("  DISCRIMINATING RATE  %ld / %ld  =  %.2f%%   <- the honest number\n",
           real_ok, denom, 100.0 * (double)real_ok / (double)denom);
    printf("     A pass that survives deleting the test's stylesheet is not\n"
           "     evidence about CSS or layout. Quote this rate, not the one above.\n");

    /* Is the discrimination check itself alive? It is negative control 2 folded
     * into the ordinary run, so it needs the same guarantee the comparator does:
     * a check that never fires is indistinguishable from a check that is not
     * there. Both directions have to be non-empty -- all-discriminating would
     * mean the CSS-stripped re-render is not happening, all-non-discriminating
     * would mean nothing we do depends on CSS. The gate greps for SUSPECT. */
    if (opt_always_equal) {
        printf("\nALWAYS-EQUAL CONTROL: %ld match-type test(s) still failing "
               "(must be 0)\n", match_fail);
        printf("  rel=mismatch tests are EXPECTED to fail here -- they require the\n"
               "  two renderings to differ, and this comparator says they never do.\n");
        if (match_fail == 0)
            printf("  ok: the suite goes green when the comparator cannot say no.\n");
    }

    if (!opt_nocss && !opt_always_equal && ok > 200) {
        if (nondisc_pass == 0 || real_ok == 0)
            printf("\nDISCRIMINATION CHECK: SUSPECT -- %ld non-discriminating, %ld "
                   "discriminating of %ld passes.\n  One side is empty, which means the "
                   "check is not measuring anything.\n", nondisc_pass, real_ok, ok);
        else
            printf("\nDISCRIMINATION CHECK: alive -- %ld non-discriminating, %ld "
                   "discriminating.\n", nondisc_pass, real_ok);
    }

    printf("\n---- ranked failure causes (heuristic: first feature marker in the source) ----\n");
    qsort(bk, (size_t)nbk, sizeof *bk, cmp_bk);
    for (int i = 0; i < nbk && i < 25; i++)
        printf("  %-20s %6ld\n", bk[i].name, bk[i].n);

    printf("\n---- failures by WPT spec directory (objective) ----\n");
    qsort(sd, (size_t)nsd, sizeof *sd, cmp_bk);
    for (int i = 0; i < nsd && i < 25; i++)
        printf("  %-32s %6ld\n", sd[i].name, sd[i].n);

    if (baseline) {
        printf("\nratchet: %ld regressions, %ld newly passing (baseline %s, %d entries)\n",
               regressions, newpass, baseline, nbase);
        if (regressions) {
            printf("FAIL: %ld test(s) that the baseline says should pass do not.\n", regressions);
            return 1;
        }
    }
    printf("failures listed in build/reftest/failures.txt\n");
    return 0;
}
