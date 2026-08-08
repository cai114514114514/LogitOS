/* refmanifest -- finding the reftests in a WPT checkout and reading what each
 * one claims. Pure text, no rendering, no pixels: this half can be (and is)
 * unit-tested on its own, because misreading the corpus produces a wrong number
 * just as efficiently as misrendering it does.
 *
 * A WPT reftest is an HTML file carrying
 *     <link rel="match"    href="...">     renders identically to that file
 *     <link rel="mismatch" href="...">     must render DIFFERENTLY
 * and optionally
 *     <meta name="fuzzy"  content="maxDifference;totalPixels">
 *     <meta name="fuzzy"  content="ref.html:0-2;0-100">   (scoped to one ref)
 *     <meta name="flags"  content="ahem dom interact ...">
 *
 * Several `match` links mean ANY ONE matching is a pass. A reference may itself
 * carry a `match`, forming a chain -- following it is not optional, and the
 * corpus contains chains three deep. */
#ifndef REFMANIFEST_H
#define REFMANIFEST_H

#define RM_MAXREFS   8
#define RM_PATHMAX   512

/* One `<link rel=match|mismatch>` on one page. */
struct rm_ref {
    char path[RM_PATHMAX];       /* resolved, relative to the WPT root */
    int  mismatch;               /* 0 = must match, 1 = must differ */
    /* fuzzy, already resolved for THIS reference: a scoped annotation that
     * names another file does not apply here and is not copied in. -1 = none.
     * WPT writes each as a range "a-b"; the max is what a comparison may use. */
    int  fuzz_maxdiff;           /* largest permitted per-channel delta */
    long fuzz_maxpixels;         /* largest permitted count of differing pixels */
};

/* Flags that decide whether a test is JUDGEABLE by a static pixel comparison at
 * all. These are not excuses -- each one names a capability the harness does
 * not have, and a test skipped for one of them is reported in its own bucket so
 * the number is visible rather than quietly folded into the denominator. */
enum {
    RM_F_AHEM     = 1 << 0,   /* needs the Ahem font */
    RM_F_INTERACT = 1 << 1,   /* needs user interaction -- unjudgeable */
    RM_F_ANIMATED = 1 << 2,   /* renders differently over time -- unjudgeable */
    RM_F_PAGED    = 1 << 3,   /* paged media -- we have no paginator */
    RM_F_HTTP     = 1 << 4,   /* needs specific HTTP headers -- no server here */
    RM_F_SPEECH   = 1 << 5,   /* speech-only */
    RM_F_USERSTYLE= 1 << 6,   /* needs a user stylesheet installed */
    RM_F_ASIS     = 1 << 7,   /* served byte-for-byte, .asis file */
    RM_F_INVALID  = 1 << 8,   /* deliberately invalid input */
    RM_F_MAY      = 1 << 9,   /* tests an optional behaviour -- failing is legal */
    RM_F_SHOULD   = 1 << 10,  /* tests a recommendation */
};

struct rm_test {
    char path[RM_PATHMAX];       /* relative to the WPT root */
    struct rm_ref refs[RM_MAXREFS];
    int  nrefs;
    int  flags;                  /* RM_F_* */
    int  tentative;              /* filename contains ".tentative." */
    int  xhtml;                  /* .xht/.xhtml -- parsed by an HTML parser here */
};

/* Parse one page's source for its reftest metadata. `rel` is the page's path
 * relative to the WPT root (used to resolve hrefs). Returns the number of
 * references found; 0 means "not a reftest". Fills *out regardless. */
int rm_parse(const char *src, long len, const char *rel, struct rm_test *out);

/* Read `rel` under `root` and rm_parse it. Returns -1 if unreadable. */
int rm_parse_file(const char *root, const char *rel, struct rm_test *out);

/* Resolve `href` against the directory of `base` (both root-relative), writing a
 * clean root-relative path with . and .. removed. Returns 0, or -1 if the href
 * is absolute, external, a fragment or a data: URI -- none of which this
 * harness can fetch. */
int rm_resolve(const char *base, const char *href, char *out, int outmax);

/* Walk `root` recursively and call `cb` with every root-relative path whose
 * extension is one a reftest can have. Returns the count. `filter`, when
 * non-NULL, keeps only paths containing it as a substring. */
int rm_walk(const char *root, const char *filter,
            void (*cb)(const char *rel, void *ud), void *ud);

/* True for a path that is a REFERENCE rather than a test: WPT keeps these in
 * `reference/` or `references/` directories, or names them `-ref.html` /
 * `-notref.html`. They carry no `match` of their own most of the time, but the
 * ones that do would otherwise be counted as tests in their own right and
 * inflate both the numerator and the denominator. */
int rm_is_reference(const char *rel);

#endif /* REFMANIFEST_H */
