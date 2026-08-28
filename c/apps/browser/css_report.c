/* css_report.c -- the one accounting of what happened to a page's CSS.
 *
 * Read css_report.h first; it carries the argument and names the two parser
 * funnels this file collects from. This file is the storage and the printer,
 * and it is deliberately allocation-free and dependency-free so that it
 * compiles identically into the ring-3 browser (UCFLAGS, freestanding,
 * mini-libc) and into libcss_host.a for the host gates. A record that only
 * exists in one of those two builds is a record the control cannot be run
 * against.
 *
 * WHAT IS NOT HERE, AND WHY IT IS NOT A COUNTER OF ZERO.
 * The brief this file was written for asks for a count of stylesheets refused
 * for their content type. There is no such count because THERE IS NO SUCH
 * CHECK: browser.c's <link rel=stylesheet> path never looks at Content-Type,
 * so nothing can be refused for it. A field reading `refused for type: 0`
 * would say "we checked and none were" -- the exact shape of a stub-to-success
 * this tree has paid for elsewhere. css_report_print() says the check is
 * missing instead. Add the check and the counter together, or neither.
 */

#include "css_report.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* The authoritative declaration of the declaration/at-rule funnel and of the
 * CSS_DROP_* reason codes. Included rather than re-declared: css_engine.c and
 * two host tools already spell this extern by hand, and a fourth hand-written
 * copy of a signature is how one of them ends up disagreeing. */
#include "parse/language.h"

/* The selector funnel, defined in parse/parse.c. Declared here rather than in
 * a header because it has exactly one producer and one consumer, and the
 * linker holding those two to one signature is the whole contract. */
extern void (*css__parse_selector_drop_report)(const char *text, size_t len);
extern void (*css__parse_recover_report)(int kind);

/* Must match parse.c's CSS_RECOVER_*. Three integers with one producer; the
 * names are spelled here so a reader of this file can see what they mean. */
enum { REC_SELECTOR = 0, REC_ATRULE = 1, REC_DECL = 2 };

int css_report_verbose = 0;

static struct css_report g_rep;

/* ---------------------------------------------------------------------- */

static void cpy(char *dst, int cap, const char *src, int len)
{
    int i = 0;
    if (cap <= 0) return;
    if (src) for (; i < len && i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int streq(const char *a, const char *b)
{
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static int starts(const char *s, const char *p)
{
    int i = 0;
    if (!s || !p) return 0;
    for (; p[i]; i++) if (s[i] != p[i]) return 0;
    return 1;
}

static int is_name_ch(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* ---- the bucket table ------------------------------------------------- */

static void bucket(const char *kind, const char *key,
                   const char *sample, int slen)
{
    int i;
    for (i = 0; i < g_rep.nreason; i++)
        if (streq(g_rep.reason[i].key, key) && streq(g_rep.reason[i].kind, kind)) {
            g_rep.reason[i].n++;
            return;
        }
    if (g_rep.nreason >= CSSREP_REASONS) { g_rep.reason_overflow++; return; }
    {
        struct cssrep_reason *r = &g_rep.reason[g_rep.nreason++];
        cpy(r->key, CSSREP_KEY, key, CSSREP_KEY);
        cpy(r->kind, (int)sizeof r->kind, kind, (int)sizeof r->kind);
        cpy(r->sample, (int)sizeof r->sample, sample,
            slen < (int)sizeof r->sample ? slen : (int)sizeof r->sample);
        r->n = 1;
    }
}

/* ---- reason keys ------------------------------------------------------
 *
 * A key is a LABEL derived from the dropped text and nothing else. There is
 * deliberately no table of "pseudo-classes LibCSS supports" here: such a table
 * would be a second spelling of a fact that lives in language.c, it would rot,
 * and a stale entry would put a real drop in the wrong bucket while looking
 * right. So the selector key simply names every pseudo the dropped selector
 * contains. `.a:has(> b)` and `.c:has(d)` both bucket as ":has()"; a selector
 * mixing a supported pseudo with an unsupported one gets a compound key like
 * ":hover:has()", which is noisier but never lies. The verbatim sample is kept
 * beside every bucket, so the label is never the only evidence.
 */
static void key_for_selector(const char *t, int len, char *out, int cap)
{
    int o = 0, i, saw_amp = 0;
    out[0] = 0;
    for (i = 0; i < len; i++) {
        if (t[i] == '&') { saw_amp = 1; continue; }
        if (t[i] != ':') continue;
        {
            int j = i, colons = 1;
            if (j + 1 < len && t[j + 1] == ':') { colons = 2; j++; }
            j++;
            if (j >= len || !is_name_ch(t[j])) continue;
            if (o < cap - 5) { out[o++] = ':'; if (colons == 2) out[o++] = ':'; }
            while (j < len && is_name_ch(t[j]) && o < cap - 4) out[o++] = t[j++];
            if (j < len && t[j] == '(' && o < cap - 3) { out[o++] = '('; out[o++] = ')'; }
            i = j - 1;
        }
    }
    if (saw_amp && o < cap - 2) out[o++] = '&';
    out[o] = 0;
    if (o == 0) cpy(out, cap, "selector-syntax", 15);
}

/* Fold the expected noise. Every real page ships vendor-prefixed and custom
 * properties that a correct engine discards; four buckets say so without
 * burying the properties that are actually interesting. */
static void fold_prop(char *out, int cap)
{
    if (starts(out, "-webkit-"))   cpy(out, cap, "-webkit-*", 9);
    else if (starts(out, "-moz-")) cpy(out, cap, "-moz-*", 6);
    else if (starts(out, "-ms-"))  cpy(out, cap, "-ms-*", 5);
    else if (starts(out, "-o-"))   cpy(out, cap, "-o-*", 4);
    else if (starts(out, "--"))    cpy(out, cap, "--custom-*", 10);
}

/* ---- the two funnels --------------------------------------------------- */

/* parse/language.c: every declaration, and every unknown at-rule (with a
 * leading '@'). CSS_DROP_ACCEPTED is NOT a drop -- it is the count of what was
 * kept, and it is the only positive number in this record. */
static void on_decl_drop(const char *name, size_t nlen, int reason)
{
    char key[CSSREP_KEY];
    const char *kind;

    cpy(key, (int)sizeof key, name, (int)nlen);

    if (key[0] == '@') {
        g_rep.drops++;
        g_rep.drop_atrule++;
        if (css_report_verbose >= 2)
            printf("[css] drop at-rule: %s\n", key);
        bucket("at-rule", key, name, (int)nlen);
        return;
    }

    switch (reason) {
    case CSS_DROP_ACCEPTED:    g_rep.decl_accepted++; return;
    case CSS_DROP_UNKNOWN_PROP: kind = "unknown-prop"; g_rep.drop_unknown_prop++; break;
    case CSS_DROP_BAD_VALUE:    kind = "bad-value";    g_rep.drop_bad_value++;    break;
    case CSS_DROP_TRAILING:     kind = "trailing";     g_rep.drop_trailing++;     break;
    default:
        /* A reason code this file has never heard of. Bucketed LOUDLY under
         * its own name rather than dropped: rule 5, a control that cannot be
         * watched failing is worse than no control. If language.h grows a
         * fifth reason, this line is what makes it visible instead of
         * silently landing in "bad-value". */
        kind = "reason-?";
        break;
    }
    g_rep.drops++;
    fold_prop(key, (int)sizeof key);
    if (css_report_verbose >= 2)
        printf("[css] drop %s: %.*s\n", kind, (int)nlen, name);
    bucket(kind, key, name, (int)nlen);
}

/* parse/parse.c: a whole ruleset discarded because its selector did not parse.
 * This is the expensive one -- every declaration inside went with it. */
static void on_selector_drop(const char *text, size_t len)
{
    char key[CSSREP_KEY];
    key_for_selector(text, (int)len, key, (int)sizeof key);
    g_rep.drops++;
    g_rep.drop_selector++;
    if (css_report_verbose >= 2)
        printf("[css] drop selector: %.*s\n", len > 120 ? 120 : (int)len, text);
    bucket("selector", key, text, (int)len);
}

static void on_recover(int kind)
{
    switch (kind) {
    case REC_SELECTOR: g_rep.recovered_selector++; break;
    case REC_ATRULE:   g_rep.recovered_atrule++;   break;
    case REC_DECL:     g_rep.recovered_decl++;     break;
    default: break;
    }
}

/* ---------------------------------------------------------------------- */

void css_report_reset(void)
{
    memset(&g_rep, 0, sizeof g_rep);
    /* Installed HERE rather than at browser start-up on purpose: a page that
     * never resets the record is a page whose numbers would otherwise be the
     * previous page's, and that failure would look like a plausible report.
     * If this call goes missing, the hooks are NULL and every count is zero --
     * an obviously dead instrument rather than a quietly wrong one. */
    css__parse_drop_report = on_decl_drop;
    css__parse_selector_drop_report = on_selector_drop;
    css__parse_recover_report = on_recover;
}

void css_report_detach(void)
{
    css__parse_drop_report = NULL;
    css__parse_selector_drop_report = NULL;
    css__parse_recover_report = NULL;
}

const struct css_report *css_report_get(void) { return &g_rep; }

void css_report_style(int bytes)
{
    g_rep.linked_style++;
    (void)bytes;   /* the byte split is recorded once, by css_report_concat */
}

void css_report_link(const char *href, int skipped, const char *why)
{
    g_rep.linked_link++;
    if (!skipped) return;
    g_rep.link_skipped++;
    if (g_rep.nsheet < CSSREP_SHEETS) {
        struct cssrep_sheet *s = &g_rep.sheet[g_rep.nsheet++];
        cpy(s->url, CSSREP_URL, href, CSSREP_URL);
        cpy(s->why, (int)sizeof s->why, why ? why : "skipped", (int)sizeof s->why);
        s->outcome = CSSSH_SKIPPED;
        s->status = 0;
        s->bytes = 0;
    } else g_rep.sheet_overflow++;
}

void css_report_fetched(const char *url, int status, int bytes,
                        int outcome, const char *why)
{
    switch (outcome) {
    case CSSSH_OK:    g_rep.fetch_ok++;    break;
    case CSSSH_EMPTY: g_rep.fetch_empty++; break;
    case CSSSH_HTTP:  g_rep.fetch_http++;  break;
    default:          g_rep.fetch_error++; break;
    }
    if (g_rep.nsheet < CSSREP_SHEETS) {
        struct cssrep_sheet *s = &g_rep.sheet[g_rep.nsheet++];
        cpy(s->url, CSSREP_URL, url, CSSREP_URL);
        cpy(s->why, (int)sizeof s->why, why ? why : "", (int)sizeof s->why);
        s->status = status;
        s->bytes = bytes;
        s->outcome = (unsigned char)outcome;
    } else g_rep.sheet_overflow++;
}

void css_report_concat(int inline_bytes, int external_offered, int external_kept)
{
    g_rep.bytes_inline = inline_bytes;
    g_rep.bytes_offered = external_offered;
    g_rep.bytes_external = external_kept;
    if (external_offered > external_kept) {
        g_rep.truncated = 1;
        g_rep.truncated_bytes = external_offered - external_kept;
    }
}

void css_report_expand(int in_len, int out_len, int cap)
{
    /* css_expand_vars writes at most `cap`; an output that hit the cap is a
     * SECOND, independent truncation and has to be named separately -- the two
     * buffers are different sizes and only one of them is author_css. */
    if (in_len > 0 && out_len >= cap - 1) g_rep.expand_truncated = 1;
}

void css_report_parsed(int ok, int bytes)
{
    if (ok) { g_rep.parsed++; g_rep.parsed_bytes += bytes; }
    else g_rep.parse_failed++;
}

void css_report_parse_cached(void) { g_rep.parsed_cached++; }

/* ---------------------------------------------------------------------- */

static const char *outcome_name(int o)
{
    switch (o) {
    case CSSSH_INLINE:  return "inline";
    case CSSSH_OK:      return "ok";
    case CSSSH_EMPTY:   return "empty";
    case CSSSH_HTTP:    return "http";
    case CSSSH_ERROR:   return "error";
    default:            return "skipped";
    }
}

void css_report_print(void)
{
    const struct css_report *r = &g_rep;
    int i, j, shown;

    printf("[css] linked: %d <style>, %d <link rel=stylesheet> (%d not fetched)\n",
           r->linked_style, r->linked_link, r->link_skipped);
    /* The content-type note rides on the fetch line only when something was
     * actually fetched: it is a statement about a check this browser does not
     * do, and repeating it on a page with no <link> at all is the kind of
     * constant noise that trains people to stop reading the whole block. */
    printf("[css] fetched: %d ok, %d empty, %d http-error, %d transport-error%s\n",
           r->fetch_ok, r->fetch_empty, r->fetch_http, r->fetch_error,
           (r->fetch_ok || r->fetch_empty || r->fetch_http || r->fetch_error)
               ? "  (content-type: NOT CHECKED -- no MIME filter on <link>)" : "");
    printf("[css] bytes: %d inline + %d external = %d to the parser%s\n",
           r->bytes_inline, r->bytes_external, r->bytes_inline + r->bytes_external,
           r->truncated ? "" : "  (nothing truncated)");
    if (r->truncated)
        printf("[css] TRUNCATED: %d of %d external bytes never reached the parser "
               "(author_css full)\n", r->truncated_bytes, r->bytes_offered);
    if (r->expand_truncated)
        printf("[css] TRUNCATED: css_expand_vars filled css_expanded; the tail of "
               "the stylesheet never reached the parser\n");
    printf("[css] parsed: %d sheet(s) accepted, %d refused; %d declarations kept\n",
           r->parsed, r->parse_failed, r->decl_accepted);
    if (r->parsed_cached)
        printf("[css] %d sheet(s) served from the parse cache -- byte-identical to the "
               "previous parse, so the DROP COUNTS BELOW ARE NOT THIS LOAD'S\n",
               r->parsed_cached);
    printf("[css] dropped: %d  (%d selector, %d at-rule, %d unknown-prop, "
           "%d bad-value, %d trailing)\n",
           r->drops, r->drop_selector, r->drop_atrule, r->drop_unknown_prop,
           r->drop_bad_value, r->drop_trailing);
    if (r->recovered_selector || r->recovered_atrule || r->recovered_decl)
        printf("[css] parser resync: %d selector, %d at-rule, %d declaration "
               "(syntax-malformed; a different population from the drops above)\n",
               r->recovered_selector, r->recovered_atrule, r->recovered_decl);

    if (r->drops == 0) {
        printf("[css] no rule or declaration was discarded\n");
        return;
    }

    /* Buckets, by count. Dropping is normal, so the default shows the head of
     * the list and says how much it is hiding. */
    shown = css_report_verbose ? r->nreason : (r->nreason < 12 ? r->nreason : 12);
    {
        int order[CSSREP_REASONS];
        for (i = 0; i < r->nreason; i++) order[i] = i;
        for (i = 0; i < shown; i++) {
            int best = i;
            for (j = i + 1; j < r->nreason; j++)
                if (r->reason[order[j]].n > r->reason[order[best]].n) best = j;
            { int t = order[i]; order[i] = order[best]; order[best] = t; }
            printf("[css]   %6d  %-12s %-24s | %.60s\n",
                   r->reason[order[i]].n, r->reason[order[i]].kind,
                   r->reason[order[i]].key, r->reason[order[i]].sample);
        }
    }
    if (shown < r->nreason)
        printf("[css]   ... %d more reason(s); css_report_verbose=1 for all\n",
               r->nreason - shown);
    if (r->reason_overflow)
        printf("[css]   ... %d drop(s) past %d distinct reasons, not bucketed\n",
               r->reason_overflow, CSSREP_REASONS);

    if (css_report_verbose) {
        for (i = 0; i < r->nsheet; i++)
            printf("[css]   sheet %-8s %4d %8d B  %.60s %s\n",
                   outcome_name(r->sheet[i].outcome), r->sheet[i].status,
                   r->sheet[i].bytes, r->sheet[i].url, r->sheet[i].why);
        if (r->sheet_overflow)
            printf("[css]   ... %d more sheet(s)\n", r->sheet_overflow);
    }
}
