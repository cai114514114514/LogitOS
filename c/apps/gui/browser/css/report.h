/* css_report.h -- THE SINGLE SITE where this browser accounts for what
 * happened to a page's CSS.
 *
 * WHY THIS FILE EXISTS. Measured 2026-08-29 on the shipped build: the browser
 * prints twelve different lines about scripts ("scripts collected: 6 external
 * classic, 0 external module, 14 inline", "load done: 97 requests, 93 reused,
 * 0 modules loaded") and NOT ONE about stylesheets. `grep -ic "text/css"` over
 * the whole serial log of a real bing.com load returns 0.
 *
 * The cost of that is not cosmetic. When a page renders with UA defaults --
 * bing's nav bar came out as a bulleted <ul> at x=8 with everything in one
 * column -- five completely different bugs produce that one symptom:
 *
 *     1. the CSS was never requested
 *     2. it was requested and the fetch failed
 *     3. it arrived and was never handed to the parser
 *     4. it parsed and whole rules were discarded
 *     5. it applied and the LAYOUT engine got it wrong
 *
 * From outside they are indistinguishable. This record is what tells them
 * apart, and (4) is the one nothing in this tree could see whole: LibCSS is
 * NetSurf's, not a 2026 browser, and the CSS specification tells a parser to
 * DISCARD what it does not understand -- correctly, and SILENTLY. A page whose
 * layout lives in :has(), a container query, @layer or nesting therefore
 * renders with UA defaults and leaves no error anywhere.
 *
 * ONE JAR, ONE DOOR -- and the doors were already there.
 * This file COLLECTS; it does not invent a funnel. The parser has exactly two:
 *
 *   parse/language.h  css__parse_drop_report(name, nlen, reason)
 *                     every DECLARATION (accepted too -- CSS_DROP_ACCEPTED is
 *                     the count of what was KEPT) and every unknown AT-RULE,
 *                     the latter reported with a leading '@'.
 *   parse/parse.c     css__parse_selector_drop_report(text, len)
 *                     a whole RULESET discarded because its selector list did
 *                     not parse. language.c cannot see this one: the decision
 *                     happens before parseProperty is ever reached.
 *
 * Both are installed by css_report_reset() and by nothing else. A third hook
 * for a fourth kind of drop is a bug: add it to whichever funnel already owns
 * that decision. The serial summary prints FROM this record
 * (css_report_print), and a DevTools panel reads it through css_report_get().
 * Do not add a second count of any of these quantities anywhere.
 *
 * DROPPING IS NORMAL AND MUST NOT READ AS AN ERROR. Every page on the web
 * carries vendor prefixes and forward-compatible rules that a correct engine
 * discards; a healthy page drops hundreds of declarations. So the default
 * output is a handful of aggregated lines, buckets sorted by count, and the
 * per-rule text is behind css_report_verbose. An instrument that screams on a
 * healthy page is an instrument that gets ignored on a sick one.
 */
#ifndef LOGIT_CSS_REPORT_H
#define LOGIT_CSS_REPORT_H

enum {
    CSSREP_REASONS = 128,  /* distinct drop reasons kept; overflow is counted */
    CSSREP_KEY     = 40,   /* reason key, e.g. ":has()" or "@layer" or "gap" */
    CSSREP_SHEETS  = 64,   /* per-sheet rows kept for the detail view */
    CSSREP_URL     = 96
};

/* What became of one stylesheet the document asked for. These are the outcomes
 * the fetch path can actually distinguish today -- see the note on content type
 * in css_report.c, which is a MISSING CHECK and is reported as one rather than
 * as a counter that is zero because nothing sets it. */
enum {
    CSSSH_INLINE = 0,      /* a <style> element; no fetch involved */
    CSSSH_OK,              /* bytes arrived */
    CSSSH_EMPTY,           /* fetch succeeded, zero bytes */
    CSSSH_HTTP,            /* server answered non-2xx; `status` says which */
    CSSSH_ERROR,           /* transport failed; `why` is bfetch's static string */
    CSSSH_SKIPPED          /* the browser chose not to fetch it; `why` says why */
};

struct cssrep_sheet {
    char url[CSSREP_URL];
    char why[40];          /* the error/skip reason, empty when there is none */
    int  status;           /* HTTP status, 0 if there was no response */
    int  bytes;
    unsigned char outcome; /* CSSSH_* */
};

struct cssrep_reason {
    char key[CSSREP_KEY];  /* what was dropped, aggregated: ":has()", "@layer" */
    char kind[14];         /* "selector" | "at-rule" | "unknown-prop" | ... */
    char sample[72];       /* the first text seen in this bucket, verbatim */
    int  n;
};

struct css_report {
    /* --- linked: what the document asks for -------------------------------- */
    int linked_style;      /* <style> elements with content */
    int linked_link;       /* <link rel=stylesheet>, every one, before filters */
    int link_skipped;      /* of those, not fetched (dup / data: / a11y theme) */

    /* --- fetched ----------------------------------------------------------- */
    int fetch_ok, fetch_empty, fetch_http, fetch_error;

    /* --- concatenated: the author_css buffer ------------------------------- */
    int bytes_inline;      /* from <style> */
    int bytes_external;    /* from <link>, as KEPT */
    int bytes_offered;     /* what the external sheets actually contained */
    int truncated;         /* sheets cut short by the buffer, 0 = none */
    int truncated_bytes;   /* how many bytes of CSS never reached the parser */
    int expand_truncated;  /* css_expand_vars overran css_expanded */

    /* --- parsed ------------------------------------------------------------ */
    int parsed;            /* css_stylesheet_create + data_done succeeded */
    int parse_failed;      /* make_sheet returned NULL */
    int parsed_bytes;
    /* css_engine.c caches the author sheet against its exact bytes, so a
     * RELOAD of the same page re-uses the previous parse and the parser never
     * runs. Without this counter the report would then read "parsed: 0,
     * dropped: 0" -- which is the exact sentence that means "the CSS never
     * arrived", said about a page whose CSS is fine. Counted, and printed as
     * its own sentence. */
    int parsed_cached;

    /* --- kept -------------------------------------------------------------- */
    int decl_accepted;     /* declarations the parser took (CSS_DROP_ACCEPTED) */

    /* --- dropped: the half nothing could see ------------------------------- */
    int drops;             /* total rules + declarations discarded */
    int drop_selector;     /* whole ruleset: selector list unparseable */
    int drop_atrule;       /* whole at-rule: @layer, @container, ... */
    int drop_unknown_prop; /* declaration: no such property in this LibCSS */
    int drop_bad_value;    /* declaration: property known, its handler refused */
    int drop_trailing;     /* declaration: junk after an otherwise valid value */

    /* Parser RECOVERY entries: how many times the state machine had to skip
     * forward to resynchronise. NOT the same population as the drops above and
     * never to be subtracted from them -- see the comment in parse.c. For
     * declarations in particular this counts only the SYNTAX-malformed ones,
     * which carry no property name and so can never be reported by name. */
    int recovered_selector, recovered_atrule, recovered_decl;

    int nreason, reason_overflow;
    struct cssrep_reason reason[CSSREP_REASONS];

    int nsheet, sheet_overflow;
    struct cssrep_sheet sheet[CSSREP_SHEETS];
};

/* Per-rule detail. 0 = the aggregated summary only (the default, because a
 * healthy page drops hundreds of vendor-prefixed declarations). 1 = also print
 * every bucket with its first sample and every sheet. 2 = print every drop as
 * it happens. */
extern int css_report_verbose;

/* Call once per navigation, BEFORE any stylesheet work. Also installs the two
 * parser hooks, so a reset that never happens is a report that stays at zero
 * rather than one that silently accumulates across pages. */
void css_report_reset(void);

/* Uninstall the hooks. For a host harness that wants the parser back the way
 * the shipping browser has it (one predicted branch, no reporting). */
void css_report_detach(void);

const struct css_report *css_report_get(void);

/* Discovery. Called from the one place that walks the document for CSS. */
void css_report_style(int bytes);
void css_report_link(const char *href, int skipped, const char *why);

/* Fetch outcome for one <link>. `why` may be NULL. */
void css_report_fetched(const char *url, int status, int bytes,
                        int outcome, const char *why);

/* The concatenation buffer. `offered` is what the sheets contained, `kept` what
 * fitted. They differ exactly when author_css overflowed -- the 216 KB-sheet
 * failure browser.c already carries a comment about. */
void css_report_concat(int inline_bytes, int external_offered,
                       int external_kept);
void css_report_expand(int in_len, int out_len, int cap);

/* One author-sheet parse. ok = the sheet object exists. Called from
 * css_engine.c's author_sheet(), the single site that parses a page's CSS. */
void css_report_parsed(int ok, int bytes);

/* The same site, when the cache answered and no parse happened. */
void css_report_parse_cached(void);

/* One "[css]" block. Always prints the five stage lines, so a page with a
 * perfect stylesheet still produces evidence that the pipeline ran. */
void css_report_print(void);

#endif
