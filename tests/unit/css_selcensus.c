/* css_selcensus -- WHICH SELECTORS LOSE, AND WHAT THEY CARRY.
 *
 * MEASUREMENT, not a gate. Named without a `test-` prefix for the reason
 * css-drop-probe's comment in tests/cssweb.mk gives: tools/audit_tests.py
 * counts any `test-*` with a recipe as a suite CI runs, and a target that
 * reports rather than asserts must not be counted as a test.
 *
 * THE QUESTION. "3,129 rules and 5,870 declarations are lost to selectors" is
 * a number, not a work order. This turns it into one: for every rule in the
 * real corpus that applies to NOTHING, it records WHY, and ranks the reasons
 * BY DECLARATIONS CARRIED rather than by rule count -- one failing :is() may
 * carry four hundred declarations and forty failing :nth-of-type rules six.
 * The precedent is tools/wpt_rank.py and cssom_rank.py; CLAUDE.md calls the
 * practice "ranking, not rates".
 *
 * WHY IT ASKS THE ENGINE INSTEAD OF READING THE SELECTOR TEXT. A text scan
 * can tell you a selector contains `:has(`. It cannot tell you whether that
 * rule would have applied to anything if :has() worked, and that is the whole
 * difference between a work order and a list of grievances. So the two
 * numbers this tool rests on both come from the shipping engine through two
 * NULL-by-default hooks (see their comments in select/select.c and
 * stylesheet.c):
 *
 *   css__select_match_report(sel, 0|1)  -- 0: this selector was OFFERED a
 *       candidate element (its subject's tag/class/id bucket produced one).
 *       1: the whole chain was just confirmed TRUE.
 *   css__stylesheet_rule_decl_report(rule) -- one call per ACCEPTED
 *       declaration, with its owning rule. This is the "declarations carried".
 *
 * THE SPLIT THAT MAKES THE NUMBER MEAN ANYTHING, and it is the trap every
 * count of "rules that never matched" falls into: MOST RULES THAT NEVER MATCH
 * ARE NOT A DEFECT. Every real site ships one bundle for many pages, so a
 * large fraction of any sheet is rules for elements that are simply not in
 * this document. A census that does not separate those publishes a huge
 * number that a correct browser would also produce. The hook's `considered`
 * arm is what separates them:
 *
 *   NEVER OFFERED  -> no element on this page carries the subject's
 *                     tag/class/id. Not an engine defect. Reported, never
 *                     ranked.
 *   OFFERED, NEVER TRUE -> real elements were tested and the chain was always
 *                     false. This is where the engine's gaps live.
 *
 * WHY A RULE'S REASON IS THE **CHEAPEST** OF ITS SELECTORS, not the worst.
 * `.btn:hover, .btn:focus-visible { ... }` fails on both arms. Teaching the
 * engine :focus-visible would still paint nothing at rest, because the rule
 * needs a state this engine deliberately answers false. So a rule is credited
 * to ENGINE-INERT only when EVERY one of its selectors is engine-inert. That
 * is the direction that cannot flatter the finding: it under-counts the
 * engine's share, never over-counts it.
 *
 * THE CONTROL, and it is watchable. The classifier below decides "inert" by
 * reading the parsed selector chain against select.c's matcher. The engine
 * decides it by matching. If the classifier calls a selector inert and the
 * engine matched it, the classifier is WRONG -- and the tool prints
 * `CLASSIFIER CONTRADICTED` with the count, rather than quietly reporting a
 * number derived from a broken rule. A zero there is what earns the rest of
 * the table.
 *
 * Build: the css-selector-census rule in tests/cssweb.mk.
 *   make css-selector-census BUILD=<yours>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "dom.h"
#include "css.h"

#include <libcss/libcss.h>
#include "stylesheet.h"

extern void (*css__select_match_report)(const css_selector *sel, int matched);
extern void (*css__stylesheet_rule_decl_report)(const css_rule *rule);

/* c/apps/browser/css_extra.c -- the approximation accounting. Declared here
 * rather than in css.h because it is a measurement seam, not part of the
 * engine's contract with the browser. */
extern void css_extra_approx_stats(long *re, long *ra, long *ae, long *aa,
                                   long *de, long *da);
extern void css_extra_approx_reset(void);

#define WINW  1280
#define VIEWH  800
#define CSSMAX (8 * 1024 * 1024)

static char *sheets_buf, *author_css, *expanded;

/* ------------------------------------------------------------------ tables */
/* Open-addressed pointer maps. The corpus is ~44k rules; 1<<19 keeps the load
 * factor under 20% so probe chains stay short and the run stays a few
 * seconds, which is what makes re-measuring cheap enough to actually do. */
#define HN (1u << 19)

struct selrec { const css_selector *k; long considered, matched; };
struct rulerec { const css_rule *k; long ndecl; };

static struct selrec  *g_sel;
static struct rulerec *g_rule;

static unsigned hashp(const void *p)
{
    unsigned long long v = (unsigned long long)(size_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33;
    return (unsigned)(v & (HN - 1));
}
static struct selrec *sel_slot(const css_selector *k)
{
    unsigned h = hashp(k);
    for (;;) {
        if (g_sel[h].k == k) return &g_sel[h];
        if (g_sel[h].k == NULL) { g_sel[h].k = k; return &g_sel[h]; }
        h = (h + 1) & (HN - 1);
    }
}
static struct rulerec *rule_slot(const css_rule *k)
{
    unsigned h = hashp(k);
    for (;;) {
        if (g_rule[h].k == k) return &g_rule[h];
        if (g_rule[h].k == NULL) { g_rule[h].k = k; return &g_rule[h]; }
        h = (h + 1) & (HN - 1);
    }
}

/* Sheets we saw a declaration land in, so the rule walk below covers exactly
 * the sheets this page parsed and not the UA sheet (parsed by css_init before
 * the hooks are installed) or a previous page's. */
#define MAXSHEET 8
static const css_stylesheet *g_sheet[MAXSHEET];
static int g_nsheet;

static void note_sheet(const css_rule *r)
{
    while (r != NULL && r->ptype == CSS_RULE_PARENT_RULE)
        r = (const css_rule *)r->parent;
    if (r == NULL || r->parent == NULL) return;
    const css_stylesheet *s = (const css_stylesheet *)r->parent;
    /* SKIP INLINE STYLES, and this was found by AddressSanitizer rather than
     * by reasoning: css_engine.c's style_node() creates a whole css_stylesheet
     * for every element carrying a style="" attribute and DESTROYS it before
     * returning (css_engine.c:2497 and :2552). Holding one of those past the
     * pass is a use-after-free, and it read as an intermittent segfault on 3
     * of 15 pages that vanished under a debugger -- rule 1, and the apparatus
     * was mine. They are also not what this census is about: an inline style
     * has no selector. */
    if (s->inline_style) return;
    for (int i = 0; i < g_nsheet; i++) if (g_sheet[i] == s) return;
    if (g_nsheet < MAXSHEET) g_sheet[g_nsheet++] = s;
}

static int g_hooks_on;

static void on_match(const css_selector *sel, int matched)
{
    if (!g_hooks_on) return;
    struct selrec *r = sel_slot(sel);
    if (matched) r->matched++; else r->considered++;
}
static void on_decl(const css_rule *rule)
{
    if (!g_hooks_on) return;
    rule_slot(rule)->ndecl++;
    note_sheet(rule);
}

/* ------------------------------------------------- the static classifier */
/* WHAT select.c's match_detail() CAN REPORT TRUE, transcribed from the
 * else-if chain in third_party/css/libcss/src/select/select.c. This is a
 * SECOND DOOR on that chain and it is stated as one rather than hidden: the
 * cross-check at the bottom of main() is the lock on it. A name that belongs
 * here and is missing shows up as CLASSIFIER CONTRADICTED (the engine matched
 * something this list calls inert); a name that does NOT belong here and is
 * present can only under-report, which is the safe direction. */
static const char *matchable_pc[] = {
    "root", "empty", "link", "visited", "hover", "active", "focus", "target",
    "lang", "enabled", "disabled", "checked",
    "first-child", "last-child", "only-child",
    "first-of-type", "last-of-type", "only-of-type",
    "nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type",
    NULL
};
/* Of the above, the four whose select handler in c/apps/browser/css_engine.c
 * is h_false: they are *correctly* false on a freshly loaded document (no
 * pointer is over anything, nothing is focused, no history), so a rule that
 * needs one is not an engine gap in the same sense. Kept in its own bucket
 * rather than merged either way. */
static const char *state_pc[] = { "visited", "hover", "active", "focus", NULL };

/* select.c's PSEUDO_ELEMENT case sets *match=true for exactly these four. */
static const char *matchable_pe[] = {
    "first-line", "first-letter", "before", "after", NULL
};
/* ...and of those four, the two that cascade into a pseudo-element computed
 * style NOTHING IN THIS BROWSER EVER READS. `grep -c CSS_PSEUDO_ELEMENT_BEFORE
 * c/apps/browser/*.c` is 0: layout.c and browser_paint.c only ever look at
 * styles[CSS_PSEUDO_ELEMENT_NONE] (css_engine.c:2507). So these rules match,
 * cascade, and render nothing -- the "parsed and never read" shape, one layer
 * up from the property table where audit-css already finds it. */
static const char *unread_pe[] = { "before", "after", NULL };

static int in_list(const char **list, const char *s, int len)
{
    for (int i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == len && !strncmp(list[i], s, len)) return 1;
    return 0;
}

/* The SHAPE of a selector that was offered real elements and was never true.
 * Sub-classifying that bucket is what keeps the work order complete: a
 * combinator this engine gets wrong and a compound that simply does not
 * describe anything on the page are the same row until they are split. It is
 * a structural description, not a diagnosis -- said that way in the output. */
static const char *shape_of(const css_selector *sel)
{
    int comb = 0, attr = 0, nth = 0, list = 0, ncomp = 0;
    for (const css_selector *s = sel; s != NULL; s = s->combinator) {
        ncomp++;
        if (s->data.comb != CSS_COMBINATOR_NONE) {
            switch (s->data.comb) {
            case CSS_COMBINATOR_ANCESTOR: comb = comb > 1 ? comb : 1; break;
            case CSS_COMBINATOR_PARENT:   comb = comb > 2 ? comb : 2; break;
            case CSS_COMBINATOR_SIBLING:  comb = 3; break;
            case CSS_COMBINATOR_GENERIC_SIBLING: comb = 4; break;
            default: break;
            }
        }
        const css_selector_detail *d = &s->data;
        for (;;) {
            if (d->type >= CSS_SELECTOR_ATTRIBUTE) attr = 1;
            if (d->value_type == CSS_SELECTOR_DETAIL_VALUE_SELECTOR_LIST) list = 1;
            if (d->value_type == CSS_SELECTOR_DETAIL_VALUE_NTH) nth = 1;
            if (!d->next) break;
            d++;
        }
    }
    if (comb == 3) return "sibling combinator  +";
    if (comb == 4) return "sibling combinator  ~";
    if (comb == 2) return "child combinator    >";
    if (comb == 1) return "descendant combinator";
    if (list) return ":is()/:where()/:not()";
    if (nth)  return "nth-*()";
    if (attr) return "[attribute]";
    return "single compound";
}

/* Reason codes, ordered CHEAPEST FIRST -- see the header. A rule takes the
 * minimum over its selectors. */
enum {
    R_MATCHED = 0,
    R_NOMATCH,        /* offered real elements, chain always false */
    R_SUBJECT_ABSENT, /* never offered: no such tag/class/id on this page */
    R_STATE,          /* needs :hover/:focus/:active/:visited */
    R_UNREAD_PE,      /* ::before/::after: matches, cascades, never rendered */
    R_INERT,          /* a detail select.c can never report true */
    R_NREASON
};
static const char *reason_name[] = {
    "MATCHED", "OFFERED-NEVER-TRUE", "SUBJECT-ABSENT", "NEEDS-STATE",
    "PSEUDO-EL-NEVER-RENDERED", "ENGINE-INERT"
};

/* The blocking name, when there is one (":focus-visible", "::-webkit-foo"). */
static void classify_sel(const css_selector *sel, int *reason, char *why, int whymax)
{
    const css_selector *s = sel;
    int worst = R_MATCHED;
    why[0] = 0;

    for (; s != NULL; s = s->combinator) {
        const css_selector_detail *d = &s->data;
        for (;;) {
            if (d->type == CSS_SELECTOR_PSEUDO_CLASS ||
                d->type == CSS_SELECTOR_PSEUDO_ELEMENT) {
                /* :is()/:where()/:not() carry a parsed alternative list and
                 * are matched by the SELECTOR_LIST branch, never by name. */
                if (d->value_type != CSS_SELECTOR_DETAIL_VALUE_SELECTOR_LIST) {
                    const char *nm = lwc_string_data(d->qname.name);
                    int nl = (int)lwc_string_length(d->qname.name);
                    int isel = (d->type == CSS_SELECTOR_PSEUDO_ELEMENT);
                    const char **ok = isel ? matchable_pe : matchable_pc;
                    if (!in_list(ok, nm, nl)) {
                        if (worst < R_INERT) {
                            worst = R_INERT;
                            snprintf(why, whymax, "%s%.*s", isel ? "::" : ":", nl, nm);
                        }
                    } else if (isel && in_list(unread_pe, nm, nl)) {
                        if (worst < R_UNREAD_PE) {
                            worst = R_UNREAD_PE;
                            snprintf(why, whymax, "::%.*s", nl, nm);
                        }
                    } else if (!isel && in_list(state_pc, nm, nl)) {
                        if (worst < R_STATE) {
                            worst = R_STATE;
                            snprintf(why, whymax, ":%.*s", nl, nm);
                        }
                    }
                }
            }
            if (!d->next) break;
            d++;
        }
    }
    *reason = worst;
}

/* ------------------------------------------------------- reason accounting */
struct bucket {
    char key[48];
    int  reason;
    long rules, decls, sels;
    int  pages, seen_page;
};
#define MAXB 512
static struct bucket g_b[MAXB];
static int g_nb;

static struct bucket *bucket(int reason, const char *key)
{
    for (int i = 0; i < g_nb; i++)
        if (g_b[i].reason == reason && !strcmp(g_b[i].key, key)) return &g_b[i];
    if (g_nb >= MAXB) return &g_b[MAXB - 1];
    struct bucket *b = &g_b[g_nb++];
    snprintf(b->key, sizeof(b->key), "%s", key);
    b->reason = reason;
    return b;
}

static long g_tot_rules, g_tot_decls, g_tot_sels;
static long g_reason_rules[R_NREASON], g_reason_decls[R_NREASON];
static long g_mixed_rules;          /* lost rules whose selectors disagree */
static long g_contradicted;         /* classifier said inert, engine matched */
static char g_contra_eg[160];

struct pagerow { char name[16]; long rules, decls, lost_rules, lost_decls,
                                    inert_decls, absent_decls;
                 long css_in, css_out; int nsheet;
                 long cat_rules, cat_decls, sep_rules, sep_decls, nsrc; };
static struct pagerow g_page[32];
static int g_npage;

/* ------------------------------------------------------------- rule walk */
static void walk_rules(const css_rule *r, struct pagerow *pr)
{
    for (; r != NULL; r = r->next) {
        if (r->type == CSS_RULE_MEDIA) {
            walk_rules(((const css_rule_media *)r)->first_child, pr);
            continue;
        }
        if (r->type != CSS_RULE_SELECTOR) continue;

        const css_rule_selector *rs = (const css_rule_selector *)r;
        long nd = rule_slot(r)->ndecl;

        int best = R_INERT + 1;         /* nothing yet; min over selectors */
        char bestwhy[48] = "";
        int nreason = 0, seen[R_NREASON];
        memset(seen, 0, sizeof seen);

        for (int i = 0; i < r->items; i++) {
            const css_selector *sel = rs->selectors[i];
            struct selrec *sr = sel_slot(sel);
            int reason; char why[48];
            classify_sel(sel, &reason, why, sizeof why);

            if (sr->matched > 0) {
                if (reason == R_INERT && g_contradicted++ == 0)
                    snprintf(g_contra_eg, sizeof g_contra_eg,
                             "%s matched %ld time(s)", why, sr->matched);
                reason = R_MATCHED;
                snprintf(why, sizeof why, "%s", shape_of(sel));
            } else if (reason == R_MATCHED) {
                /* nothing structurally wrong with it; was it even tried? */
                if (sr->considered > 0) {
                    reason = R_NOMATCH;
                    snprintf(why, sizeof why, "%s", shape_of(sel));
                } else {
                    reason = R_SUBJECT_ABSENT;
                }
            } else if (reason != R_INERT && sr->considered == 0) {
                /* a state/pseudo-element rule whose subject is not here
                 * either: the cheaper reason is the honest one */
                reason = R_SUBJECT_ABSENT;
                why[0] = 0;
            }

            g_tot_sels++;
            if (!seen[reason]) { seen[reason] = 1; nreason++; }
            if (reason < best) { best = reason; snprintf(bestwhy, sizeof bestwhy, "%s", why); }
        }
        if (best > R_INERT) continue;   /* rule with no selectors */

        g_tot_rules++; g_tot_decls += nd;
        pr->rules++; pr->decls += nd;
        g_reason_rules[best]++; g_reason_decls[best] += nd;
        if (best != R_MATCHED) {
            pr->lost_rules++; pr->lost_decls += nd;
            if (nreason > 1) g_mixed_rules++;
            if (best == R_INERT) pr->inert_decls += nd;
            if (best == R_SUBJECT_ABSENT) pr->absent_decls += nd;
        }
        {
            struct bucket *b = bucket(best, bestwhy[0] ? bestwhy : "-");
            b->rules++; b->decls += nd; b->sels += r->items;
            if (!b->seen_page) { b->seen_page = 1; b->pages++; }
        }
    }
}

/* ----------------------------------------------------------------- driver */
static char *slurp(const char *p, int *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, n, f); fclose(f);
    b[got] = 0; if (len) *len = (int)got;
    return b;
}

static int tag_is(const char *t, const char *lit)
{ int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0; }

static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style"))
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
    for (struct node *c = n->first_child; c; c = c->next) o = collect_style(c, out, o, max);
    return o;
}


/* ---------------------------------------------- the concatenation probe ---
 * A SECOND, INDEPENDENT question, and it is not about selectors at all -- it
 * is here because the selector census could not be believed without it.
 *
 * This browser builds ONE author stylesheet by concatenating every <style>
 * element's text and every external sheet's bytes (css_engine.c's
 * author_sheet(), fed by the embedder's collected buffer). A real browser
 * gives each of those its OWN css_stylesheet, and the CSSOM requires it.
 *
 * The difference is not academic and it is not a fairness argument. In CSS a
 * block runs to its matching '}' OR TO END OF STYLESHEET. So an unclosed
 * brace in ONE source is contained by end-of-sheet in a real browser and
 * swallows EVERY LATER SOURCE ON THE PAGE here. Real pages ship unclosed
 * <style> blocks.
 *
 * The measurement is a strict control on itself: the same bytes, parsed by
 * the same library, with concatenation as the ONLY variable. Var expansion is
 * deliberately NOT applied on either side, so nothing but the joining differs.
 */
static long count_sel_rules(const css_rule *r)
{
    long n = 0;
    for (; r != NULL; r = r->next) {
        if (r->type == CSS_RULE_MEDIA) { n += count_sel_rules(((const css_rule_media *)r)->first_child); continue; }
        if (r->type == CSS_RULE_SELECTOR) n++;
    }
    return n;
}

static long g_probe_decls;
static void on_probe_decl(const css_rule *rule) { (void)rule; g_probe_decls++; }

static css_error probe_resolve(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{ (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK; }

/* Parse one source in isolation; add its rules/declarations to *rules/*decls. */
static void probe_parse(const char *data, int len, long *rules, long *decls)
{
    if (len <= 0) return;
    css_stylesheet_params p;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url = "probe";
    p.resolve = probe_resolve;
    css_stylesheet *s = NULL;
    if (css_stylesheet_create(&p, &s) != CSS_OK || s == NULL) return;
    long before = g_probe_decls;
    css_stylesheet_append_data(s, (const uint8_t *)data, (size_t)len);
    css_stylesheet_data_done(s);
    *rules += count_sel_rules(s->rule_list);
    *decls += g_probe_decls - before;
    css_stylesheet_destroy(s);
}

/* Every <style> element's text, one call per element -- the granularity a
 * real browser uses. */
static void probe_styles(struct node *n, long *rules, long *decls, long *nsrc,
                         char *tmp, int tmpmax)
{
    if (!n) return;
    if (n->type == N_ELEM && tag_is(n->tag, "style")) {
        int o = 0;
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < tmpmax - 1; i++) tmp[o++] = c->text[i];
        if (o) { probe_parse(tmp, o, rules, decls); (*nsrc)++; }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        probe_styles(c, rules, decls, nsrc, tmp, tmpmax);
}

static void census_one(const char *label, const char *htmlpath,
                       char **sheetpaths, int nsheet)
{
    int htmllen = 0;
    char *html = slurp(htmlpath, &htmllen);
    if (!html) { fprintf(stderr, "css_selcensus: cannot open %s\n", htmlpath); return; }

    int sheetlen = 0;
    for (int i = 0; i < nsheet; i++) {
        int l = 0; char *s = slurp(sheetpaths[i], &l);
        if (!s) continue;
        if (sheetlen + l + 1 < CSSMAX) {
            memcpy(sheets_buf + sheetlen, s, l); sheetlen += l;
            sheets_buf[sheetlen++] = '\n';
        }
        free(s);
    }

    struct node *root = dom_parse(html, htmllen);
    if (!root) { fprintf(stderr, "css_selcensus: parse failed %s\n", htmlpath); free(html); return; }

    int css_len = collect_style(root, author_css, 0, CSSMAX);
    if (sheetlen && css_len + sheetlen < CSSMAX) {
        memcpy(author_css + css_len, sheets_buf, sheetlen); css_len += sheetlen;
    }
    int exlen = css_expand_vars(author_css, css_len, expanded, CSSMAX);

    memset(g_sel, 0, sizeof(struct selrec) * HN);
    memset(g_rule, 0, sizeof(struct rulerec) * HN);
    g_nsheet = 0;
    for (int i = 0; i < g_nb; i++) g_b[i].seen_page = 0;

    g_hooks_on = 1;
    css_apply(root, expanded, exlen);
    g_hooks_on = 0;
    /* The post pass is registered AND called explicitly, exactly as
     * tests/unit/css_audit.c does it: css_apply() runs g_post_pass only on the
     * scoped path, so a census that only registered it would report zero
     * approximations and read like a clean bill of health. */
    css_extra_apply(root, expanded, exlen);

    struct pagerow *pr = &g_page[g_npage++];
    memset(pr, 0, sizeof *pr);
    snprintf(pr->name, sizeof pr->name, "%s", label);
    pr->css_in = css_len; pr->css_out = exlen; pr->nsheet = g_nsheet;

    for (int i = 0; i < g_nsheet; i++)
        walk_rules(g_sheet[i]->rule_list, pr);

    /* --- the concatenation control: same bytes, joining as the only variable */
    css__stylesheet_rule_decl_report = on_probe_decl;
    g_probe_decls = 0;
    probe_styles(root, &pr->sep_rules, &pr->sep_decls, &pr->nsrc, sheets_buf, CSSMAX);
    for (int i = 0; i < nsheet; i++) {
        int l = 0; char *s2 = slurp(sheetpaths[i], &l);
        if (!s2) continue;
        probe_parse(s2, l, &pr->sep_rules, &pr->sep_decls); pr->nsrc++;
        free(s2);
    }
    probe_parse(author_css, css_len, &pr->cat_rules, &pr->cat_decls);
    css__stylesheet_rule_decl_report = on_decl;

    free(html);
}

int main(int argc, char **argv)
{
    sheets_buf = malloc(CSSMAX); author_css = malloc(CSSMAX); expanded = malloc(CSSMAX);
    g_sel = calloc(HN, sizeof *g_sel);
    g_rule = calloc(HN, sizeof *g_rule);
    if (!sheets_buf || !author_css || !expanded || !g_sel || !g_rule) return 1;

    int top = 24;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strncmp(argv[argi], "--top=", 6)) top = atoi(argv[argi] + 6);
        else break;
    }

    css_init();
    css_viewport(WINW, VIEWH);
    css_set_post_pass(css_extra_apply);

    css__select_match_report = on_match;
    css__stylesheet_rule_decl_report = on_decl;

    for (; argi < argc; argi++) {
        const char *dir = argv[argi];
        char htmlp[512]; snprintf(htmlp, sizeof htmlp, "%s/index.html", dir);
        char *sp[40]; int ns = 0;
        for (int i = 1; i <= 40 && ns < 40; i++) {
            char p[512]; snprintf(p, sizeof p, "%s/sheet-%d.css", dir, i);
            FILE *f = fopen(p, "rb"); if (!f) continue; fclose(f);
            sp[ns++] = strdup(p);
        }
        char lbl[16];
        int dl = (int)strlen(dir);
        while (dl > 0 && dir[dl-1] == '/') dl--;
        int bs = dl; while (bs > 0 && dir[bs-1] != '/') bs--;
        snprintf(lbl, sizeof lbl, "%.*s", dl - bs, dir + bs);
        census_one(lbl, htmlp, sp, ns);
        for (int i = 0; i < ns; i++) free(sp[i]);
    }

    css__select_match_report = NULL;
    css__stylesheet_rule_decl_report = NULL;

    /* ---------------------------------------------------------- report */
    printf("\n=== css_selcensus: %d pages ===\n", g_npage);
    printf("%-9s %9s %9s %8s %9s %8s %9s %9s %9s\n",
           "page", "cssIn", "cssOut", "rules", "decls", "lostR", "lostD",
           "inertD", "absentD");
    for (int i = 0; i < g_npage; i++) {
        struct pagerow *r = &g_page[i];
        printf("%-9s %9ld %9ld %8ld %9ld %8ld %9ld %9ld %9ld%s\n",
               r->name, r->css_in, r->css_out, r->rules, r->decls,
               r->lost_rules, r->lost_decls, r->inert_decls, r->absent_decls,
               r->rules == 0 && r->css_in > 0 ? "   <-- CSS ARRIVED, ZERO RULES" : "");
    }

    printf("\n=== CONCATENATION: one author sheet vs one sheet per source ===\n");
    printf("  Same bytes, same library, NO var expansion on either side -- the only\n");
    printf("  variable is that this browser joins every <style> and every external\n");
    printf("  sheet into ONE css_stylesheet. An unclosed brace is contained by\n");
    printf("  end-of-stylesheet; concatenated, it eats every later source.\n");
    printf("%-9s %6s %10s %10s %10s %10s %10s\n",
           "page", "srcs", "rulesSEP", "rulesCAT", "declSEP", "declCAT", "declLOST");
    long ts_r = 0, tc_r = 0, ts_d = 0, tc_d = 0;
    for (int i = 0; i < g_npage; i++) {
        struct pagerow *r = &g_page[i];
        long lost = r->sep_decls - r->cat_decls;
        printf("%-9s %6ld %10ld %10ld %10ld %10ld %10ld%s\n",
               r->name, r->nsrc, r->sep_rules, r->cat_rules, r->sep_decls,
               r->cat_decls, lost,
               lost > r->sep_decls / 2 ? "   <-- MOST OF THE PAGE'S CSS" : "");
        ts_r += r->sep_rules; tc_r += r->cat_rules;
        ts_d += r->sep_decls; tc_d += r->cat_decls;
    }
    printf("%-9s %6s %10ld %10ld %10ld %10ld %10ld\n",
           "TOTAL", "", ts_r, tc_r, ts_d, tc_d, ts_d - tc_d);

    printf("\n=== every rule accounted for, by reason (cheapest reason wins) ===\n");
    printf("%-28s %9s %10s %6s  %s\n", "reason", "rules", "decls", "%decl", "engine defect?");
    for (int i = 0; i < R_NREASON; i++) {
        printf("%-28s %9ld %10ld %5ld%%  %s\n", reason_name[i],
               g_reason_rules[i], g_reason_decls[i],
               g_tot_decls ? g_reason_decls[i] * 100 / g_tot_decls : 0,
               i == R_MATCHED         ? "-- applied" :
               i == R_SUBJECT_ABSENT  ? "NO -- no such element on this page" :
               i == R_NOMATCH         ? "maybe -- real elements tested, always false" :
               i == R_STATE           ? "no -- correctly false on a document at rest" :
               i == R_UNREAD_PE       ? "YES -- matches and cascades, nothing renders it" :
                                        "YES -- select.c can never report this true");
    }
    printf("%-28s %9ld %10ld\n", "TOTAL", g_tot_rules, g_tot_decls);
    printf("  selectors seen: %ld   lost rules whose selectors disagree: %ld\n",
           g_tot_sels, g_mixed_rules);

    printf("\n=== THE WORK ORDER: ranked by DECLARATIONS CARRIED ===\n");
    /* insertion sort, the table is tiny */
    for (int i = 1; i < g_nb; i++) {
        struct bucket t = g_b[i]; int j = i - 1;
        while (j >= 0 && g_b[j].decls < t.decls) { g_b[j+1] = g_b[j]; j--; }
        g_b[j+1] = t;
    }
    printf("%-26s %-30s %8s %9s %6s\n", "reason", "blocking construct", "rules", "decls", "pages");
    int shown = 0;
    for (int i = 0; i < g_nb && shown < top; i++) {
        if (g_b[i].reason == R_SUBJECT_ABSENT) continue;
        printf("%-26s %-30s %8ld %9ld %6d\n",
               reason_name[g_b[i].reason], g_b[i].key, g_b[i].rules, g_b[i].decls,
               g_b[i].pages);
        shown++;
    }

    {
        long re = 0, ra = 0, ae = 0, aa = 0, de = 0, da = 0;
        css_extra_approx_stats(&re, &ra, &ae, &aa, &de, &da);
        /* THE DISCRIMINATOR FOR THE "maybe" ROW, and without it the biggest
     * non-absent bucket is unreadable. A descendant selector that was offered
     * real elements and was never true can mean (a) this engine's descendant
     * matching is broken or (b) the page has a .b that is not inside an .a --
     * ordinary, and every browser sees it. Putting MATCHED beside NEVER-TRUE
     * for the SAME shape answers it: a mechanism that matched thousands of
     * times is not broken, so its never-true rows are page content. */
    printf("\n=== does each selector SHAPE work at all? (matched vs offered-never-true) ===\n");
    printf("%-24s %10s %10s %10s\n", "shape", "matchedR", "neverR", "works?");
    for (int i = 0; i < g_nb; i++) {
        if (g_b[i].reason != R_MATCHED) continue;
        long never = 0;
        for (int j = 0; j < g_nb; j++)
            if (g_b[j].reason == R_NOMATCH && !strcmp(g_b[j].key, g_b[i].key))
                never = g_b[j].rules;
        printf("%-24s %10ld %10ld %10s\n", g_b[i].key, g_b[i].rules, never,
               g_b[i].rules > 0 ? "YES" : "-- never once");
    }

    printf("\n=== THE ERROR NO DROP COUNTER CAN SEE: css_extra's APPROXIMATION ===\n");
        printf("  css_extra.c reduces `.card .thumb` to `.thumb` and lets source order\n");
        printf("  decide. Nothing was dropped -- the declaration was APPLIED, to a\n");
        printf("  SUPERSET of the elements the author named. A falling drop count is\n");
        printf("  not evidence of correctness while this number is large.\n");
        printf("  %-46s %8s %8s %7s\n", "", "exact", "approx", "approx%%");
        printf("  %-46s %8ld %8ld %6ld%%\n", "RULES compiled by css_extra", re, ra,
               (re + ra) ? ra * 100 / (re + ra) : 0);
        printf("  %-46s %8ld %8ld %6ld%%\n", "APPLICATIONS (element x rule)", ae, aa,
               (ae + aa) ? aa * 100 / (ae + aa) : 0);
        printf("  %-46s %8ld %8ld %6ld%%\n", "DECLARATIONS carried by those", de, da,
               (de + da) ? da * 100 / (de + da) : 0);
    }

    printf("\n=== CONTROL ===\n");
    if (g_contradicted)
        printf("  CLASSIFIER CONTRADICTED %ld time(s) -- e.g. %s\n"
               "  The static list in this file disagrees with select.c. DISTRUST the\n"
               "  ENGINE-INERT row above until this is 0.\n",
               g_contradicted, g_contra_eg);
    else
        printf("  classifier vs engine: 0 contradictions "
               "(nothing this file called inert was ever matched)\n");
    return 0;
}
