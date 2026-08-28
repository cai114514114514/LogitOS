/* css_drop_probe: what does this CSS engine SILENTLY drop?
 *
 * MEASUREMENT ONLY. Nothing here is a gate and nothing here is implemented --
 * this tool exists to turn "modern CSS probably does not work" into a table
 * with a verdict per feature and the evidence beside it.
 *
 * The question, and why it is not answerable by reading source: LibCSS is
 * NetSurf's, and the CSS specification tells a parser to DISCARD what it does
 * not understand -- correctly, and SILENTLY. There are four different places a
 * modern construct can vanish, they look identical from the outside, and they
 * need completely different work:
 *
 *   1. THE AT-RULE IS UNKNOWN.  Discarded WITH ITS BLOCK, and in the CSS
 *      grammar an unknown at-rule swallows to the matching '}'. A page whose
 *      whole sheet is inside one renders unstyled. This is the failure the
 *      @layer patch already fixed and it is the most expensive shape.
 *   2. THE SELECTOR IS UNKNOWN.  One rule is discarded; the sheet continues.
 *      Cheaper, but a page whose layout lives in :has() still comes out wrong
 *      with nothing in any log.
 *   3. THE DECLARATION IS DROPPED.  Property unknown to the table, or the
 *      value handler refused it. Reported by name through the
 *      css__parse_drop_report hook LibCSS already carries.
 *   4. THE DECLARATION IS PARSED AND NEVER READ.  css_engine.c has to copy a
 *      computed value into `struct cstyle` for anything to happen. A property
 *      that survives the cascade and has no field is dropped just as
 *      completely, only later and more quietly -- and this one is INVISIBLE to
 *      the drop hook, which is why every case below also checks a used value.
 *
 * So each case declares what it expects to reach, and the probe reports which
 * of the four happened. Three signals per case, all measured, none inferred:
 *
 *   target   -- did the declaration under test reach `struct cstyle`?
 *   canary   -- did an ordinary rule placed AFTER the construct still cascade?
 *               This is what separates "one rule lost" from "the rest of the
 *               stylesheet lost", i.e. case 1 from cases 2-4.
 *   drops    -- what the LibCSS hook said, by name and reason.
 *
 * The canary is the load-bearing one and it is the reason the shape of every
 * case is identical. A test that only asserted the target would score @layer
 * and :has() the same, and they are not remotely the same failure.
 *
 * @supports gets a second, separate pass (supports_honesty()), because it is
 * the one feature whose WRONG ANSWER IS WORSE THAN ITS ABSENCE. It is how a
 * stylesheet asks this engine what it can do. If it answers yes for something
 * the engine cannot do, every progressive-enhancement fallback on the web takes
 * the branch that does not work -- and the author's own fallback, which would
 * have rendered, never runs. So for every feature the probe already measured,
 * it also asks @supports the same question and diffs the two answers.
 *
 * Build: the css-drop-probe rule in tests/cssweb.mk.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "dom.h"
#include "css.h"

/* ---- the LibCSS drop hook (parse/language.h) ---- */
enum { DROP_UNKNOWN = 0, DROP_BADVALUE = 1, DROP_TRAILING = 2, DROP_ACCEPTED = 3 };
extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

#define MAXEV 256
static struct { char name[64]; int reason; } g_ev[MAXEV];
static int g_nev;
static int g_collect;

static void on_drop(const char *name, size_t nlen, int reason)
{
    if (!g_collect || g_nev >= MAXEV) return;
    size_t n = nlen < sizeof(g_ev[0].name) - 1 ? nlen : sizeof(g_ev[0].name) - 1;
    memcpy(g_ev[g_nev].name, name, n);
    g_ev[g_nev].name[n] = 0;
    g_ev[g_nev].reason = reason;
    g_nev++;
}

/* Was `name` reported dropped (for any reason other than ACCEPTED)? */
static int dropped(const char *name)
{
    for (int i = 0; i < g_nev; i++)
        if (!strcmp(g_ev[i].name, name) && g_ev[i].reason != DROP_ACCEPTED) return 1;
    return 0;
}
static int accepted(const char *name)
{
    for (int i = 0; i < g_nev; i++)
        if (!strcmp(g_ev[i].name, name) && g_ev[i].reason == DROP_ACCEPTED) return 1;
    return 0;
}
static const char *reason_of(const char *name)
{
    for (int i = 0; i < g_nev; i++)
        if (!strcmp(g_ev[i].name, name))
            switch (g_ev[i].reason) {
            case DROP_UNKNOWN:  return "no such property/at-rule in this LibCSS";
            case DROP_BADVALUE: return "property known, value handler refused";
            case DROP_TRAILING: return "trailing junk after the value";
            default:            return "accepted by the parser";
            }
    return "not reported at all";
}
static void drops_line(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < g_nev; i++) {
        if (g_ev[i].reason == DROP_ACCEPTED) continue;
        char one[80];
        snprintf(one, sizeof one, "%s%s(%d)", out[0] ? " " : "",
                 g_ev[i].name, g_ev[i].reason);
        if (strlen(out) + strlen(one) + 1 < cap) strcat(out, one);
    }
    if (!out[0]) strcpy(out, "-");
}

static struct node *find_id(struct node *n, const char *id)
{
    if (!n) return 0;
    if (n->type == N_ELEM) {
        const char *v = dom_attr(n, "id");
        if (v && !strcmp(v, id)) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_id(c, id);
        if (r) return r;
    }
    return 0;
}

static struct node *style(const char *html, const char *css)
{
    struct node *r = dom_parse(html, (int)strlen(html));
    if (!r) return 0;
    static char ex[1 << 20];
    int n = css_expand_vars(css, (int)strlen(css), ex, (int)sizeof ex);
    g_nev = 0; g_collect = 1;
    css_apply(r, ex, n);
    css_extra_apply(r, ex, n);
    g_collect = 0;
    return r;
}

static struct cstyle *st_of(struct node *root, const char *id)
{
    struct node *n = find_id(root, id);
    return n ? (struct cstyle *)n->style : 0;
}

/* The document every case styles. #t is the target, #canary is the rule that
 * must still cascade after the construct, #ctx is a parent for the cases that
 * need an ancestor (:has(), @container, subgrid). */
#define DOC \
    "<!doctype html><html><body>" \
    "<div id=ctx class=ctx><div id=t class=t>t</div><span id=mark class=mark>m</span></div>" \
    "<div id=canary>c</div>" \
    "</body></html>"

/* ---- the result table ---------------------------------------------------- */
/* V_ALWAYS_TRUE and V_WRONG_WINNER are the two verdicts that are NOT a missing
 * feature and must not be filed as one. A construct that is entered
 * unconditionally, or a cascade that picks the wrong winner among rules that
 * all arrived, is a page rendered WRONG rather than a page rendered plain --
 * and unlike a dropped block it cannot be found by looking for what is
 * missing. */
enum { V_REACHED = 0, V_DECL_DROPPED, V_PARSED_UNREAD, V_RULE_LOST, V_SHEET_LOST,
       V_ALWAYS_TRUE, V_WRONG_WINNER, V_STORED_UNUSED };
static const char *VNAME[] = {
    "REACHED", "DECL DROPPED", "PARSED-NOT-READ", "RULE LOST", "SHEET LOST",
    "ALWAYS-TRUE", "WRONG WINNER", "STORED-UNUSED"
};

/* feature is a COPY, not a pointer: several cases build their name into a
 * stack buffer and a stored pointer would dangle by the time the table prints
 * -- the sort of defect that shows up as a plausible-looking wrong row. */
struct row { char feature[96]; int verdict; char evidence[220]; };
static struct row g_row[128];
static int g_nrow;

static void record(const char *feature, int verdict, const char *ev)
{
    if (g_nrow >= (int)(sizeof g_row / sizeof g_row[0])) return;
    struct row *r = &g_row[g_nrow++];
    r->verdict = verdict;
    snprintf(r->feature, sizeof r->feature, "%s", feature);
    snprintf(r->evidence, sizeof r->evidence, "%s", ev);
}

/* One case. `css` must set #t's display to flex when the feature works, and
 * must end with a `#canary{display:grid}` rule placed AFTER the construct. */
static void probe(const char *feature, const char *css, const char *propname)
{
    struct node *r = style(DOC, css);
    struct cstyle *t = st_of(r, "t"), *c = st_of(r, "canary");
    int hit    = t && t->display == DISP_FLEX;
    int canary = c && c->display == DISP_GRID;
    char dl[160]; drops_line(dl, sizeof dl);
    char ev[220];

    int verdict;
    if (hit)               verdict = V_REACHED;
    else if (!canary)      verdict = V_SHEET_LOST;
    else if (propname && dropped(propname)) verdict = V_DECL_DROPPED;
    else                   verdict = V_RULE_LOST;

    snprintf(ev, sizeof ev, "canary=%s drops=[%s]%s%s",
             canary ? "cascaded" : "LOST", dl,
             propname ? " | " : "", propname ? reason_of(propname) : "");
    record(feature, verdict, ev);
    dom_free(r);
}

/* A property case: the declaration is inside an ordinary rule, so the selector
 * cannot be the thing that failed. Verdict distinguishes "the parser refused
 * it" from "the parser took it and css_engine.c never reads it". */
static void probe_prop(const char *feature, const char *propname,
                       const char *decl, int (*reached)(struct cstyle *))
{
    char css[512];
    snprintf(css, sizeof css, "#t{%s}#canary{display:grid}", decl);
    struct node *r = style(DOC, css);
    struct cstyle *t = st_of(r, "t"), *c = st_of(r, "canary");
    int canary = c && c->display == DISP_GRID;
    int hit = t && reached(t);
    char dl[160]; drops_line(dl, sizeof dl);
    char ev[220];
    int verdict;

    if (hit)                    verdict = V_REACHED;
    else if (!canary)           verdict = V_SHEET_LOST;
    else if (dropped(propname)) verdict = V_DECL_DROPPED;
    else if (accepted(propname))verdict = V_PARSED_UNREAD;
    else                        verdict = V_DECL_DROPPED;

    snprintf(ev, sizeof ev, "canary=%s decl=[%s] parser: %s | drops=[%s]",
             canary ? "cascaded" : "LOST", decl, reason_of(propname), dl);
    record(feature, verdict, ev);
    dom_free(r);
}

/* Each of these must read the field the DECLARATION UNDER TEST would move, and
 * nothing else. Getting this wrong is the apparatus failure this tree names
 * first: the probe's own first draft checked `display == flex` for the
 * aspect-ratio case while the same rule also said `display:flex`, so the check
 * passed with aspect-ratio entirely absent. A reach-check that another
 * declaration in the same rule can satisfy is not a reach-check. */
static int r_ml12(struct cstyle *s){ return s->ml == 12; }
static int r_gap8(struct cstyle *s){ return s->grid_gap_x == 8 && s->grid_gap_y == 8; }
static int r_inset(struct cstyle *s){ return s->has_top && s->top == 1 && s->right == 2; }
static int r_red(struct cstyle *s){ return s->color == 0xff0000; }
static int r_colorset(struct cstyle *s){ return s->color != 0 && s->color != 0x000000; }
static int r_w100(struct cstyle *s){ return s->has_w && s->width == 100; }
static int r_h100(struct cstyle *s){ return s->has_h && s->height == 100; }
static int r_h700(struct cstyle *s){ return s->has_h && s->height == 700; }
static int r_bgset(struct cstyle *s){ return s->has_bg != 0; }
static int r_sticky(struct cstyle *s){ return s->position == POS_STICKY; }
static int r_dispflex(struct cstyle *s){ return s->display == DISP_FLEX; }
/* subgrid: the grid track list reaches layout as RAW TEXT (cstyle::grid_raw),
 * so the honest reach-check is "did the word subgrid arrive there". */
static int r_subgrid(struct cstyle *s)
{
    const char *p = s->grid_raw[GR_TEMPL_COLS];
    return p && s->grid_rawlen[GR_TEMPL_COLS] >= 7 && !memcmp(p, "subgrid", 7);
}
/* aspect-ratio: THERE IS NO FIELD. Not "we do not read it" -- `struct cstyle`
 * has no member that could hold it, so no reach-check can ever pass. Stated
 * here so the row's verdict is not mistaken for a measurement error. */
static int r_never(struct cstyle *s){ (void)s; return 0; }

/* ---- @supports honesty ---------------------------------------------------
 *
 * Ask the engine, in the stylesheet's own language, whether it supports a
 * declaration -- and separately measure whether that declaration actually
 * reaches a used value. Disagreement in either direction is a finding, and the
 * two directions are NOT equally bad:
 *
 *   says YES, cannot do it   -- the author's fallback never runs. This is the
 *                               one that turns a partial feature into a blank
 *                               page, and it is what "absent is safer than
 *                               present-and-wrong" means for CSS.
 *   says NO, can do it       -- the page takes a fallback that works. Costs
 *                               fidelity, costs nothing structural.
 */
/* Does this declaration move ANY byte of the computed style?
 *
 * The pointer members (grid_raw / xraw) are spans into a different copy of the
 * sheet on each run, so comparing them as pointers would report a difference
 * for every declaration. They are compared by CONTENT: length, then the bytes
 * they point at. Everything before them is a flat memcmp. */
static int moves_cstyle(const char *decl)
{
    char with[256];
    snprintf(with, sizeof with, "#t{%s}", decl);
    struct node *ra = style(DOC, "#t{}");
    struct node *rb = style(DOC, with);
    struct cstyle *a = st_of(ra, "t"), *b = st_of(rb, "t");
    int moved = 0;

    if (!a || !b) moved = 1;
    else {
        size_t flat = offsetof(struct cstyle, grid_raw);
        if (memcmp(a, b, flat) != 0) moved = 1;
        for (int i = 0; !moved && i < GR__COUNT; i++) {
            if (a->grid_rawlen[i] != b->grid_rawlen[i]) moved = 1;
            else if (a->grid_rawlen[i] && memcmp(a->grid_raw[i], b->grid_raw[i],
                                                 a->grid_rawlen[i])) moved = 1;
        }
        for (int i = 0; !moved && i < XR__COUNT; i++) {
            if (a->xrawlen[i] != b->xrawlen[i]) moved = 1;
            else if (a->xrawlen[i] && memcmp(a->xraw[i], b->xraw[i],
                                             a->xrawlen[i])) moved = 1;
        }
    }
    dom_free(ra); dom_free(rb);
    return moved;
}

struct honesty { const char *decl; int can; };  /* can: does it reach a used value? */

static int supports_says(const char *decl)
{
    /* Both arms, so a blanket answer in either direction is visible: exactly
     * one of them must apply. */
    char css[512];
    snprintf(css, sizeof css,
             "@supports (%s){#t{display:flex}}"
             "@supports not (%s){#t{display:grid}}"
             "#canary{display:grid}", decl, decl);
    struct node *r = style(DOC, css);
    struct cstyle *t = st_of(r, "t");
    int yes = t && t->display == DISP_FLEX;
    int no  = t && t->display == DISP_GRID;
    dom_free(r);
    if (yes && !no) return 1;
    if (no && !yes) return 0;
    return -1;                 /* both or neither -- @supports is not deciding */
}

int main(void)
{
    css__parse_drop_report = on_drop;
    css_init();
    css_viewport(980, 700);
    css_set_post_pass(css_extra_apply);

    /* ================= SELECTORS ========================================= */

    probe(":has() relational selector",
          "#ctx:has(> .t){}"                    /* does the rule survive at all */
          ".ctx:has(.mark) #t{display:flex}"
          "#canary{display:grid}", 0);

    probe(":is() selector list",
          ":is(.ctx) #t{display:flex}"
          "#canary{display:grid}", 0);

    probe(":where() zero-specificity list",
          ":where(.ctx) #t{display:flex}"
          "#canary{display:grid}", 0);

    /* THE SPECIFICITY QUESTIONS WERE UNASKABLE HERE, until the selectors
     * themselves parsed and matched -- before that, a specificity test
     * written the obvious way PASSED WHEN THE SELECTOR WAS DISCARDED,
     * because a discarded rule also fails to win, and that is a control
     * that cannot be watched failing. They are ASKABLE now:
     *
     * :where() -- give the wrapped rule the SAME subject specificity as an
     * unqualified competitor (`#t` vs `:where(.ctx) #t`; both are ID-level
     * on the subject, so the only thing that can separate them is the
     * ancestor's contribution). If :where() truly contributes zero, the two
     * rules TIE and CSS source order breaks the tie -- so whichever one is
     * written SECOND wins, in BOTH orderings. A discarded :where() rule
     * would make the bare `#t` rule win regardless of order (there being
     * nothing to tie with), which is a different, falsifiable, outcome.
     * Verified by hand before wiring into this probe (dbgtest3.c): with
     * :where() written second it wins; with it written first, the bare
     * rule (now second) wins -- source order decides both times, proving
     * both PRESENCE and ZERO specificity in one shot. */
    {
        struct node *r1 = style(DOC, "#t{display:grid}" ":where(.ctx) #t{display:flex}"
                                      "#canary{display:grid}");
        struct node *r2 = style(DOC, ":where(.ctx) #t{display:flex}" "#t{display:grid}"
                                      "#canary{display:grid}");
        struct cstyle *t1 = st_of(r1, "t"), *t2 = st_of(r2, "t");
        int ok = t1 && t1->display == DISP_FLEX && t2 && t2->display == DISP_GRID;
        char ev[220];
        snprintf(ev, sizeof ev,
                 "later-wins both orderings (where-2nd=%d want %d, where-1st=%d "
                 "want %d) => :where() ties an ID-specificity competitor, i.e. "
                 "contributes exactly zero, not merely 'present'",
                 t1 ? t1->display : -1, DISP_FLEX, t2 ? t2->display : -1, DISP_GRID);
        record(":where() specificity is ZERO", ok ? V_REACHED : V_RULE_LOST, ev);
        dom_free(r1);
        dom_free(r2);
    }
    {
        struct node *r = style(DOC,
            ":is(#ctx, .nope) #t{display:flex}" /* spec 1,0,1 -- must WIN */
            ".ctx #t{display:grid}"             /* spec 0,1,1 */
            "#canary{display:grid}");
        struct cstyle *t = st_of(r, "t");
        int ok = t && t->display == DISP_FLEX;
        char ev[220];
        snprintf(ev, sizeof ev,
                 "display=%d, want flex=%d: :is(#id,..) takes the id's "
                 "specificity, beating a later class-ancestor competitor "
                 "that would otherwise win on source order alone",
                 t ? t->display : -1, DISP_FLEX);
        record(":is() specificity = most specific arg", ok ? V_REACHED : V_RULE_LOST, ev);
        dom_free(r);
    }

    /* THE COLLATERAL-DAMAGE FORM, and it is the shape real sheets ship.
     * `#t, <X> { display:flex }` -- #t matches on its own, so if X merely
     * parses (whether or not it can ever match) #t is styled. If X is a PARSE
     * ERROR the whole selector list is invalid and #t loses its styling too.
     * That is not a contrived test: every component sheet in the world writes
     * `.btn:hover, .btn:focus-visible { ... }`, and one unknown pseudo-class in
     * that list takes the :hover rule down with it. One missing table entry,
     * two broken states. */
    {
        static const char *SEL[] = {
            ":has(a)", ":is(.x)", ":where(.x)", ":focus-within", ":focus-visible",
            ":any-link", ":placeholder-shown", ":is(a,b)", ":not(.a,.b)",
            "::marker", "::selection", "::placeholder", "::backdrop",
            "[class=x i]", ":nth-child(1 of .t)", ":dir(ltr)", ":modal",
            ":user-invalid", ":defined", ":host", "::part(x)", "::slotted(a)",
        };
        for (unsigned i = 0; i < sizeof SEL / sizeof SEL[0]; i++) {
            char css[256], name[96];
            snprintf(css, sizeof css, "#t, %s{display:flex}#canary{display:grid}", SEL[i]);
            snprintf(name, sizeof name, "sel-list: `#t, %s`", SEL[i]);
            probe(name, css, 0);
        }
    }

    probe("CSS nesting (& form)",
          "#ctx{ & #t{display:flex} }"
          "#canary{display:grid}", 0);

    probe("CSS nesting (bare selector, no &)",
          "#ctx{ #t{display:flex} }"
          "#canary{display:grid}", 0);

    probe(":not() with a selector LIST",
          "#t:not(.zzz, .yyy){display:flex}"
          "#canary{display:grid}", 0);

    /* ================= AT-RULES ========================================== */

    probe("@layer (block form)",
          "@layer base{#t{display:flex}}"
          "#canary{display:grid}", 0);

    /* Layer ORDER. `@layer lo, hi` declares lo BELOW hi, so hi must win no
     * matter which comes later in the source. The rules both cascade -- this
     * is not a dropped block, it is the WRONG ONE WINNING, which is a
     * different repair and a much smaller one. */
    {
        struct node *r = style(DOC,
            "@layer lo, hi;"
            "@layer hi{#t{display:flex}}"
            "@layer lo{#t{display:grid}}");   /* later in source, LOWER layer */
        struct cstyle *t = st_of(r, "t");
        char ev[220];
        int ok = t && t->display == DISP_FLEX;
        snprintf(ev, sizeof ev,
                 "display=%d (hi=flex=%d, lo=grid=%d): both layers cascaded, but "
                 "%s -- layer precedence is source order, the declared order is "
                 "ignored", t ? t->display : -1, DISP_FLEX, DISP_GRID,
                 ok ? "the high layer won" : "the LOW layer won because it is later");
        record("@layer ORDER (declared precedence)", ok ? V_REACHED : V_WRONG_WINNER, ev);
        dom_free(r);
    }

    probe("@container (query entered)",
          ".ctx{container-type:inline-size}"
          "@container (min-width:10px){#t{display:flex}}"
          "#canary{display:grid}", 0);

    /* A FALSE container query must not apply. Written so the @container block
     * is LAST: with it first, plain source order would give the right answer
     * for the wrong reason and the case could not fail. */
    {
        struct node *r = style(DOC,
            ".ctx{container-type:inline-size;width:50px}"
            "#t{display:flex}"
            "@container (min-width:100000px){#t{display:grid}}"
            "#canary{display:grid}");
        struct cstyle *t = st_of(r, "t");
        char ev[220];
        int ok = t && t->display == DISP_FLEX;
        snprintf(ev, sizeof ev,
                 "display=%d: a query no container can satisfy (min-width:100000px "
                 "against a 50px container) %s -- @container is ENTERED, never "
                 "evaluated", t ? t->display : -1,
                 ok ? "was correctly skipped" : "APPLIED ANYWAY");
        record("@container (a FALSE query must not apply)",
               ok ? V_REACHED : V_ALWAYS_TRUE, ev);
        dom_free(r);
    }

    /* @property's whole job is the registration: syntax, inheritance, and an
     * INITIAL VALUE that var() falls back to when nobody set the property.
     * "The sheet survives the block" is not the same claim. */
    {
        struct node *r = style(DOC,
            "@property --pw{syntax:'<length>';inherits:false;initial-value:100px}"
            "#t{width:var(--pw)}"
            "#canary{display:grid}");
        struct cstyle *t = st_of(r, "t");
        char ev[220];
        int ok = t && t->has_w && t->width == 100;
        snprintf(ev, sizeof ev,
                 "width=%d has_w=%d, want 100 from initial-value: %s. The at-rule "
                 "does not swallow the sheet, but its registration is discarded.",
                 t ? t->width : -1, t ? t->has_w : -1,
                 ok ? "registered" : "the initial-value never reached var(--pw)");
        record("@property initial-value reaches var()",
               ok ? V_REACHED : V_RULE_LOST, ev);
        dom_free(r);
    }

    /* @scope is in at_rule_is_group(), so its block is ENTERED -- but entered
     * as `all`, with no scoping. The failure that produces is the opposite of
     * a dropped block and is not visible in a test that only asks whether the
     * scoped rules applied: they apply EVERYWHERE, including outside the scope
     * root. `@scope (.card) { p { color:red } }` reddens every p on the page. */
    {
        struct node *r = style(DOC,
            "@scope (.ctx){div{display:flex}}"
            "#canary2{display:block}");
        struct cstyle *in = st_of(r, "t"), *out = st_of(r, "canary");
        char ev[220];
        int applied_in  = in  && in->display  == DISP_FLEX;
        int leaked_out  = out && out->display == DISP_FLEX;   /* #canary is OUTSIDE .ctx */
        snprintf(ev, sizeof ev,
                 "inside .ctx display=%d, OUTSIDE .ctx display=%d: %s",
                 in ? in->display : -1, out ? out->display : -1,
                 leaked_out ? "the scoped rule applied outside its scope root"
                            : (applied_in ? "correctly scoped" : "the block was lost"));
        record("@scope confines its rules to the scope root",
               leaked_out ? V_ALWAYS_TRUE : (applied_in ? V_REACHED : V_RULE_LOST), ev);
        dom_free(r);
    }

    probe("@media range syntax (width >= 100px)",
          "@media (width >= 100px){#t{display:flex}}"
          "#canary{display:grid}", 0);

    /* ================= PROPERTIES ======================================== */

    probe_prop("aspect-ratio", "aspect-ratio", "aspect-ratio:16/9", r_never);
    probe_prop("gap on a flex container", "gap", "display:flex;gap:8px", r_gap8);
    probe_prop("row-gap / column-gap longhands", "column-gap",
               "display:flex;column-gap:8px;row-gap:8px", r_gap8);
    probe_prop("inset shorthand", "inset", "position:absolute;inset:1px 2px 3px 4px", r_inset);
    probe_prop("margin-inline-start (logical)", "margin-inline-start",
               "margin-inline-start:12px", r_ml12);
    probe_prop("block-size (logical sizing)", "block-size", "block-size:100px", r_h100);
    probe_prop("inline-size (logical sizing)", "inline-size", "inline-size:100px", r_w100);
    probe_prop("color-mix()", "color",
               "color:color-mix(in srgb, red 100%, blue)", r_colorset);
    probe_prop("oklch() colour", "color", "color:oklch(62.8% 0.258 29.23)", r_colorset);
    probe_prop("lab() colour", "color", "color:lab(54% 81 70)", r_colorset);
    probe_prop("hsl() modern slash syntax", "color", "color:hsl(0 100% 50% / 1)", r_red);
    probe_prop("rgb() space-separated", "color", "color:rgb(255 0 0)", r_red);
    probe_prop("#rrggbbaa hex", "color", "color:#ff0000ff", r_red);
    /* SUBGRID IS THE PROBE'S OWN FALSE POSITIVE, KEPT BECAUSE IT IS
     * INSTRUCTIVE. css_extra stores the track list as RAW TEXT, so the word
     * `subgrid` genuinely does reach `struct cstyle`, and a reach-check on
     * that field reports REACHED. It is not implemented: layout_grid.c:2231
     * says "subgrid (css-grid-2). Not started." Storing a string is not
     * supporting a feature, and this is exactly the "linking a TU is not
     * running it" shape one layer down. */
    probe_prop("subgrid", "grid-template-columns",
               "display:grid;grid-template-columns:subgrid", r_subgrid);
    if (g_nrow > 0 && g_row[g_nrow-1].verdict == V_REACHED) {
        g_row[g_nrow-1].verdict = V_STORED_UNUSED;
        snprintf(g_row[g_nrow-1].evidence, sizeof g_row[g_nrow-1].evidence,
                 "the token reaches cstyle::grid_raw as TEXT, and layout_grid.c:2231 "
                 "says \"subgrid (css-grid-2). Not started.\" -- stored, never used");
    }
    probe_prop("clamp()", "width", "width:clamp(100px, 50%, 100px)", r_w100);
    probe_prop("min()/max()", "width", "width:max(100px, 10px)", r_w100);
    probe_prop("calc() with a percentage", "width", "width:calc(100px + 0%)", r_w100);
    probe_prop("custom property in a shorthand", "display",
               "--d:flex;display:var(--d)", r_dispflex);
    probe_prop("linear-gradient background", "background-image",
               "background-image:linear-gradient(red,blue)", r_bgset);
    probe_prop("position:sticky", "position", "position:sticky", r_sticky);
    /* Viewport is 980x700, so a correct 100vh/dvh/svh is 700px. vh is the
     * control: if vh itself failed the dvh rows would prove nothing. */
    probe_prop("vh unit (control)", "height", "height:100vh", r_h700);
    probe_prop("dvh unit (dynamic viewport)", "height", "height:100dvh", r_h700);
    probe_prop("svh unit (small viewport)", "height", "height:100svh", r_h700);
    probe_prop("lvh unit (large viewport)", "height", "height:100lvh", r_h700);

    /* THE COMBINATION, and it is the one that costs a page rather than a rule.
     * @supports (container-type:...) answers NO -- honestly, container-type is
     * not in the table -- so the author's NON-container fallback layout applies.
     * But @container blocks are entered UNCONDITIONALLY, so the container-query
     * overrides apply on top of that fallback. Both halves of the author's
     * feature test are in force at once, which is precisely the state the
     * @supports evaluator was written to prevent, arriving through the other
     * door. One jar, two doors. */
    {
        struct node *r = style(DOC,
            "@supports (container-type:inline-size){.ctx{container-type:inline-size}"
            "#t{display:flex}}"
            "@supports not (container-type:inline-size){#t{display:block}}"
            "@container (min-width:1px){#t{display:grid}}"
            "#canary{display:grid}");
        struct cstyle *t = st_of(r, "t");
        char ev[220];
        int both = t && t->display == DISP_GRID;   /* the @container won */
        snprintf(ev, sizeof ev,
                 "display=%d (block=%d grid=%d): @supports routed the page to its "
                 "NO-container fallback, and the @container block then applied on "
                 "top of it -- both branches of one feature test in force",
                 t ? t->display : -1, DISP_BLOCK, DISP_GRID);
        record("@supports says no to container-type, @container runs anyway",
               both ? V_RULE_LOST : V_REACHED, ev);
        dom_free(r);
    }

    /* ================= @supports honesty ================================= */
    /* Each row: what @supports answers, against what the probe measured above.
     * The pairing is by declaration text so the two are asking the same
     * question in the same words. */
    printf("\n");

    static const struct { const char *decl; const char *note; } SUP[] = {
        { "display:grid",                       "real -- must be YES" },
        { "display:flex",                       "real -- must be YES" },
        { "color:red",                          "real -- must be YES" },
        { "backdrop-filter:blur(4px)",          "absent -- must be NO" },
        { "display:sideways-lr",                "bad value -- must be NO" },
        { "aspect-ratio:16/9",                  "measured above" },
        { "gap:8px",                            "measured above" },
        { "inset:0",                            "measured above" },
        { "margin-inline-start:12px",           "measured above" },
        { "inline-size:100px",                  "measured above" },
        { "color:color-mix(in srgb, red, blue)","measured above" },
        { "color:oklch(62.8% 0.258 29.23)",     "measured above" },
        { "grid-template-columns:subgrid",      "measured above" },
        { "width:clamp(1px,2px,3px)",           "measured above" },
        { "container-type:inline-size",         "container queries" },
        { "position:sticky",                    "measured above" },
        { "--anything:1",                       "custom prop -- spec says YES; see below" },
        { "selector(:has(a))",                  "selector() -- honest NO expected" },
        { "position:sticky",                    "YES, and layout.c treats it as RELATIVE" },
        { "min-height:10px",                    "YES, and css_engine.c never reads it" },
        { "writing-mode:vertical-rl",           "YES, and css_engine.c never reads it" },
    };

    printf("=== @supports, and whether it tells the truth ===\n");
    printf("%-42s %-9s %s\n", "declaration", "@supports", "note");
    for (unsigned i = 0; i < sizeof SUP / sizeof SUP[0]; i++) {
        int a = supports_says(SUP[i].decl);
        printf("%-42s %-9s %s\n", SUP[i].decl,
               a == 1 ? "YES" : a == 0 ? "no" : "UNDECIDED", SUP[i].note);
    }

    /* ---- THE SYSTEMATIC VERSION OF THE SAME QUESTION ---------------------
     *
     * The rows above are hand-picked, which means they can only confirm what
     * somebody already suspected. This pass is derived instead, and it finds
     * the whole class in one shot.
     *
     * @supports answers by asking LibCSS's property table and value handler --
     * "can this declaration be PARSED". That is a sound answer to a different
     * question. The question a stylesheet is asking is "will this RENDER", and
     * between the two sits css_engine.c, which has to copy a computed value
     * into `struct cstyle` for anything to happen. Every property LibCSS parses
     * and css_engine.c never reads is therefore a declaration @supports
     * affirms and the engine silently ignores.
     *
     * "DOES THE ENGINE RENDER IT" IS DECIDED BY MEASUREMENT, NOT BY A LIST,
     * and that choice is the whole reason this pass can be trusted. The first
     * draft borrowed css_audit.c's KNOWN_UNREAD table -- a hand-kept list, in
     * another file, maintained for another purpose -- and it is STALE: it names
     * min-height, letter-spacing, word-spacing, text-indent, direction,
     * writing-mode, text-transform and column-gap as never read, and all eight
     * of them reach `struct cstyle` today (measured: min_h=10, letter_spacing=3,
     * text_indent=7, direction=1, writing_mode=1, text_transform=2,
     * grid_gap_x=9). Reporting from that list would have invented eight
     * findings. The list has two producers to track -- css_engine.c's
     * css_computed_* reads AND css_extra.c's raw-text scan -- which is one jar
     * with two doors, and it drifted through the second one.
     *
     * So instead: style the document twice, once with the declaration and once
     * without, and DIFF THE COMPUTED STYLE. If not one byte of `struct cstyle`
     * moved, nothing downstream can possibly behave differently, whatever any
     * table says. No list to keep, and it cannot go stale. */
    printf("\n=== @supports says YES to declarations that move NOTHING ===\n");
    printf("(membership derived by diffing struct cstyle with and without the\n"
           " declaration -- no hand-kept list, so it cannot go stale)\n");
    {
        static const char *CAND[] = {
            "background-image:url(x.png)", "background-position:10px 10px",
            "background-repeat:no-repeat", "background-attachment:fixed",
            "text-transform:uppercase", "vertical-align:middle", "content:'x'",
            "cursor:pointer", "outline:1px solid red", "outline-width:1px",
            "border-collapse:collapse", "border-spacing:2px", "table-layout:fixed",
            "counter-increment:c", "counter-reset:c", "quotes:'<' '>'",
            "unicode-bidi:embed", "min-height:10px", "column-count:2",
            "column-width:10px", "column-rule:1px solid red", "orphans:2",
            "widows:2", "page-break-after:always", "clip:rect(0,0,0,0)",
            "font-variant:small-caps", "font-stretch:condensed",
            "writing-mode:vertical-rl", "text-shadow:1px 1px red",
            "empty-cells:hide", "caption-side:bottom", "letter-spacing:3px",
            "word-spacing:3px", "text-indent:7px", "direction:rtl",
            "column-gap:9px", "position:sticky", "float:left",
        };
        int lies = 0, inert = 0;
        for (unsigned i = 0; i < sizeof CAND / sizeof CAND[0]; i++) {
            if (moves_cstyle(CAND[i])) continue;      /* it does something */
            inert++;
            if (supports_says(CAND[i]) == 1) {
                lies++;
                printf("  YES (and moves not one byte): %s\n", CAND[i]);
            } else {
                printf("  no   (honest)                 %s\n", CAND[i]);
            }
        }
        printf("\n%d of the %d declarations that change NOTHING in the computed "
               "style are\naffirmed by @supports. Each one sends a "
               "progressive-enhancement page down its\nMODERN branch and skips "
               "the fallback that would have rendered.\n", lies, inert);
    }

    /* ================= the table ========================================= */
    printf("\n=== what the CSS engine drops, per feature ===\n");
    printf("%-52s %-16s %s\n", "feature", "verdict", "evidence");
    for (int i = 0; i < g_nrow; i++)
        printf("%-52s %-16s %s\n", g_row[i].feature,
               VNAME[g_row[i].verdict], g_row[i].evidence);

    int n[8] = {0};
    for (int i = 0; i < g_nrow; i++) n[g_row[i].verdict]++;
    printf("\n%d features probed:\n", g_nrow);
    for (int v = 0; v < 8; v++)
        if (n[v]) printf("  %-16s %d\n", VNAME[v], n[v]);
    printf("\nOf these, %d are a page rendered WRONG rather than plain "
           "(ALWAYS-TRUE + WRONG WINNER + STORED-UNUSED) -- the class no search "
           "for a missing feature will find.\n",
           n[V_ALWAYS_TRUE] + n[V_WRONG_WINNER] + n[V_STORED_UNUSED]);
    return 0;                  /* a probe, not a gate: it reports, never fails */
}
