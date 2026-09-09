/* css_selstatic_test: the four STATIC pseudo-classes, and whether @supports
 * tells the truth about the properties css_extra.c produces.
 *
 * A GATE, not a measurement (css-drop-probe is the measurement next door).
 *
 * WHAT IT IS FOR. css_engine.c's select-handler table handed LibCSS h_false
 * for :checked, :disabled, :enabled and :target, so every rule behind those
 * selectors was selected away -- 578 uses across the 15-site corpus. They are
 * grouped as "static" because each is a function of the document as it stands
 * (an attribute, a form control's stored state, the URL fragment) and so needs
 * no re-style-on-state-change machinery, which is what still keeps :hover,
 * :active and :focus on h_false.
 *
 * THE SHAPE OF EVERY POSITIVE CASE IS A PAIR, and that is the whole design.
 * Asserting only that `:checked` styles the checked box would pass on an
 * engine that made the selector match EVERYTHING -- which is the exact failure
 * mode of a hastily written handler, and the one that is invisible to a drop
 * counter because nothing was dropped. So each case asserts the match AND the
 * non-match against a sibling that differs in one attribute. "Absent beats
 * present-and-wrong" is a claim about the second half of each pair.
 *
 * The :enabled/:disabled pair carries a third element on purpose: a plain
 * <div>. Neither selector may match it. They are not each other's negation --
 * both are defined only on elements that CAN be disabled -- and writing
 * :enabled as "not :disabled" matches every div, span and wrapper on the page.
 * That is the single most likely way to get this wrong, so it has its own
 * assertions rather than being left to the reader.
 *
 * THE @supports HALF ASSERTS THE ENGINE, NOT THE TABLE. A test that only
 * checked the boolean would be testing logit_css_extra_supports_name()'s
 * answer against itself. So every @supports case does two things: it takes the
 * TRUE branch of a real `@supports`/`@supports not` pair in a real stylesheet
 * (so the answer travels through language.c's supports_decl(), the route a
 * page uses -- NOT through css_supports_decl(), which is CSS.supports()'s
 * separate door), and it then asserts that the property under test REALLY
 * REACHES a used value in `struct cstyle`. An @supports that says YES to
 * something the cascade drops is the says-YES-cannot-do-it lie this gate
 * exists to catch, and only the second assertion can see it.
 *
 * Build: tests/cssweb.mk, `make test-css-selstatic`.
 * Controls: test-css-selstatic-negctl (handlers reverted to h_false) and
 * test-css-selstatic-supports-negctl (the xraw producer reverted). Both are
 * prerequisites of this target, so they cannot become stranded.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dom.h"
#include "css.h"

static int g_fail, g_checks;

static void ck(int cond, const char *what)
{
    g_checks++;
    if (cond) return;
    g_fail++;
    printf("FAIL: %s\n", what);
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
    static char ex[1 << 18];
    int n = css_expand_vars(css, (int)strlen(css), ex, (int)sizeof ex);
    css_apply(r, ex, n);
    css_extra_apply(r, ex, n);
    return r;
}

static struct cstyle *st_of(struct node *root, const char *id)
{
    struct node *n = find_id(root, id);
    return n ? (struct cstyle *)n->style : 0;
}

/* The probe every pseudo-class case uses: did #<id> get the rule?
 *
 * `display` is the witness because it is an enum with a value no UA sheet
 * produces for these elements, so a stale or defaulted style cannot be
 * mistaken for a match. DISP_FLEX is "matched"; anything else is not. */
static int matched(struct node *root, const char *id)
{
    struct cstyle *s = st_of(root, id);
    return s && s->display == DISP_FLEX;
}

/* ---------------------------------------------------------------- :checked */
static void t_checked(void)
{
    const char *html =
        "<!doctype html><html><body><form>"
        "<input id=on  type=checkbox checked>"
        "<input id=off type=checkbox>"
        "<input id=rad type=radio checked>"
        "<input id=txt type=text value=x>"
        "<select id=sel><option id=o1 selected>a<option id=o2>b</select>"
        "<div id=plain>d</div>"
        "</form></body></html>";
    struct node *r = style(html, ":checked{display:flex}");

    ck(matched(r, "on"),   ":checked styles a <input type=checkbox checked>");
    ck(!matched(r, "off"), ":checked must NOT style an unchecked checkbox");
    ck(matched(r, "rad"),  ":checked styles a checked radio");
    /* A text input can never be checked. If this matches, the handler is
     * answering from the attribute without asking what kind of control it is,
     * and `<input type=text checked>` would style too. */
    ck(!matched(r, "txt"), ":checked must NOT style a text input");
    ck(matched(r, "o1"),   ":checked styles <option selected>");
    ck(!matched(r, "o2"),  ":checked must NOT style an unselected <option>");
    ck(!matched(r, "plain"), ":checked must NOT style a plain <div>");
    dom_free(r);
}

/* ------------------------------------------------- :disabled and :enabled */
static void t_enabled_disabled(void)
{
    const char *html =
        "<!doctype html><html><body><form>"
        "<input id=dis type=text disabled>"
        "<input id=ena type=text>"
        "<button id=bdis disabled>b</button>"
        "<fieldset id=fs disabled><input id=inner type=text></fieldset>"
        "<div id=plain>d</div>"
        "</form></body></html>";

    struct node *r = style(html, ":disabled{display:flex}");
    ck(matched(r, "dis"),    ":disabled styles a disabled input");
    ck(!matched(r, "ena"),   ":disabled must NOT style an enabled input");
    ck(matched(r, "bdis"),   ":disabled styles a disabled <button>");
    /* The ancestor rule. A handler that reads only the element's own attribute
     * passes every other case in this function and fails this one. */
    ck(matched(r, "inner"),  ":disabled styles a control inside <fieldset disabled>");
    ck(!matched(r, "plain"), ":disabled must NOT style a plain <div>");
    dom_free(r);

    r = style(html, ":enabled{display:flex}");
    ck(matched(r, "ena"),    ":enabled styles an enabled input");
    ck(!matched(r, "dis"),   ":enabled must NOT style a disabled input");
    ck(!matched(r, "inner"), ":enabled must NOT style a control inside <fieldset disabled>");
    /* THE ONE THAT CATCHES ":enabled == not :disabled". A div is neither. */
    ck(!matched(r, "plain"), ":enabled must NOT style a plain <div> (it is neither enabled nor disabled)");
    dom_free(r);
}

/* ----------------------------------------------------------------- :target */
static void t_target(void)
{
    const char *html =
        "<!doctype html><html><body>"
        "<div id=sec1>1</div><div id=sec2>2</div>"
        "<a id=anch name=named>a</a>"
        "</body></html>";

    /* No fragment: :target matches NOTHING. The dangerous failure here is a
     * handler that treats "no fragment" as "match anything". */
    css_set_target_fragment(0, 0);
    struct node *r = style(html, ":target{display:flex}");
    ck(!matched(r, "sec1"), "with no fragment, :target matches nothing (sec1)");
    ck(!matched(r, "sec2"), "with no fragment, :target matches nothing (sec2)");
    dom_free(r);

    css_set_target_fragment("sec2", -1);
    r = style(html, ":target{display:flex}");
    ck(matched(r, "sec2"),  ":target styles the element the fragment names");
    ck(!matched(r, "sec1"), ":target must NOT style a different element");
    dom_free(r);

    /* A fragment naming nothing on the page must still match nothing -- not
     * fall back to the first element, and not to the body. */
    css_set_target_fragment("nosuch", -1);
    r = style(html, ":target{display:flex}");
    ck(!matched(r, "sec1"), "a fragment naming nothing matches nothing (sec1)");
    ck(!matched(r, "sec2"), "a fragment naming nothing matches nothing (sec2)");
    dom_free(r);

    /* HTML's <a name> fallback. */
    css_set_target_fragment("named", -1);
    r = style(html, ":target{display:flex}");
    ck(matched(r, "anch"), ":target falls back to <a name=...> when no id matches");
    dom_free(r);

    /* A prefix must not match: "sec" is not "sec1". A length-blind compare
     * (strncmp against the fragment length) passes every case above. */
    css_set_target_fragment("sec", -1);
    r = style(html, ":target{display:flex}");
    ck(!matched(r, "sec1"), ":target is not a prefix match (#sec must not hit #sec1)");
    dom_free(r);

    css_set_target_fragment(0, 0);   /* leave the global as we found it */
}

/* -------------------------------------------------------------- @supports */
/* Which branch of an @supports / @supports not pair applied?
 *   1 = the true branch, 0 = the false branch, -1 = both or neither. */
static int supports_says(const char *decl)
{
    char css[512];
    snprintf(css, sizeof css,
             "@supports (%s){#t{display:flex}}"
             "@supports not (%s){#t{display:grid}}", decl, decl);
    struct node *r = style("<!doctype html><html><body><div id=t>t</div></body></html>", css);
    struct cstyle *t = st_of(r, "t");
    int yes = t && t->display == DISP_FLEX;
    int no  = t && t->display == DISP_GRID;
    dom_free(r);
    if (yes && !no) return 1;
    if (no && !yes) return 0;
    return -1;
}

/* Does this declaration reach a used value -- i.e. can the engine really do
 * it? Measured on cstyle.xraw[], which is where css_extra.c's producer puts
 * these three and where browser_paint.c reads them from. */
static int reaches_xraw(const char *decl, int idx)
{
    char css[256];
    snprintf(css, sizeof css, "#t{%s}", decl);
    struct node *r = style("<!doctype html><html><body><div id=t>t</div></body></html>", css);
    struct cstyle *t = st_of(r, "t");
    int got = t && t->xraw[idx] && t->xrawlen[idx] > 0;
    dom_free(r);
    return got;
}

/* One @supports case: the answer AND the capability, asserted together.
 *
 * Either half alone is worthless. The boolean alone tests the lookup table
 * against itself; the capability alone says nothing about what a page is told.
 * The pairing is what makes a disagreement in EITHER direction a failure --
 * says-YES-cannot-do-it (the page loses its fallback) and says-NO-can-do-it
 * (the page loses the enhancement). */
static void sup_case(const char *decl, int idx, const char *label)
{
    int says = supports_says(decl);
    int can  = reaches_xraw(decl, idx);
    char msg[256];

    snprintf(msg, sizeof msg,
             "@supports (%s) takes the TRUE branch [says=%s]", decl,
             says == 1 ? "yes" : says == 0 ? "no" : "undecided");
    ck(says == 1, msg);

    snprintf(msg, sizeof msg,
             "...and `%s` really reaches cstyle.xraw[%s] (engine, not table)",
             decl, label);
    ck(can, msg);
}

static void t_supports(void)
{
    sup_case("transform:translateX(10px)",      XR_TRANSFORM,        "XR_TRANSFORM");
    sup_case("transform-origin:50% 50%",        XR_TRANSFORM_ORIGIN, "XR_TRANSFORM_ORIGIN");
    sup_case("box-shadow:0 1px 2px #0003",      XR_BOX_SHADOW,       "XR_BOX_SHADOW");

    /* The vendor-prefixed spellings come free from the derivation -- they are
     * in parse_xraw()'s OWN key arrays, which is now the only list. If someone
     * re-hand-copies the @supports list, these are the first to fall off. */
    ck(supports_says("-webkit-transform:translateX(10px)") == 1,
       "@supports affirms -webkit-transform (derived from the producer's keys)");
    ck(supports_says("-webkit-box-shadow:0 1px 2px #0003") == 1,
       "@supports affirms -webkit-box-shadow (derived from the producer's keys)");

    /* HONEST NEGATIVES. Without these the gate would pass on a hook that
     * answered yes to everything, which is the says-YES-cannot-do-it lie and
     * is worse than the bug being fixed. */
    ck(supports_says("backdrop-filter:blur(4px)") == 0,
       "@supports still says NO to backdrop-filter (nothing produces it)");
    ck(supports_says("transform-style:preserve-3d") == 0,
       "@supports says NO to transform-style (a `transform` PREFIX, not the property)");
    ck(supports_says("display:sideways-lr") == 0,
       "@supports still says NO to a bad value for a known property");
}

/* -------------------------- the DEAD-TEXT check --------------------------
 *
 * css_extra.c's @supports hook has two halves. The xraw family is DERIVED from
 * parse_xraw()'s own key arrays and cannot go stale. The rest -- the grid
 * family, the logical box properties, gap/inset -- is a hand-kept list, and
 * css_extra.c says so and says why it cannot be derived today (the grid
 * producer stores TEXT rather than a used value; the logical properties are
 * built by string concatenation at the call site, so there is no array to hand
 * back). This is the other half of that instruction: where a list cannot be
 * derived, leave something that FAILS when it disagrees with reality.
 *
 * WHAT CAN GO WRONG IS SPECIFIC, and it is not "somebody forgot to add a
 * name". logit_css_extra_supports_name() is consulted by language.c's
 * supports_decl() ONLY on the branch where the name was NOT found in LibCSS's
 * own FIRST_PROP..LAST_PROP table. So the moment a property MOVES INTO LibCSS
 * -- which is the declared direction of travel, and border-radius has already
 * made the trip -- its entry here stops being consulted and becomes DEAD TEXT
 * that still reads like a fix. Nothing about the build changes. Nothing goes
 * red. The list just quietly stops meaning what it says.
 *
 * That is exactly the trap css_extra.c documents for background-image, found
 * by measurement rather than by reading: it IS in LibCSS's table, so an entry
 * for it would change nothing. This check generalises that one finding into a
 * standing gate over the whole hand-kept list.
 *
 * The universe is read from LibCSS itself (css_known_prop_at), not from a
 * second copy here -- a hand-kept list checked against a hand-kept list is one
 * jar with two doors and no lock. */
/* css_extra.c exports this for language.c's supports_decl() and declares it in
 * no header -- the hook is a link-time contract between two files, so the test
 * takes it the same way its real caller does. */
extern int logit_css_extra_supports_name(const char *name, int len);

static int in_libcss_table(const char *name)
{
    int n = css_known_prop_count();
    for (int i = 0; i < n; i++) {
        int len = 0;
        const char *p = css_known_prop_at(i, &len);
        if (p && len == (int)strlen(name) && !memcmp(p, name, (size_t)len))
            return 1;
    }
    return 0;
}

static void t_supports_no_dead_text(void)
{
    /* Every name the hook affirms must be ABSENT from LibCSS's table, or the
     * hook is never asked about it and the entry is dead. Both halves are
     * walked: the derived family and the hand-kept one, because the failure is
     * a property of the ROUTE, not of how the name got into the list. */
    static const char *const affirmed[] = {
        /* derived (xr_names) */
        "transform", "-webkit-transform", "transform-origin", "box-shadow",
        /* hand-kept (extra[]) */
        "gap", "row-gap",   /* column-gap is LibCSS's own -- see css_extra.c */
        "grid-template-columns", "grid-template-rows", "grid-template-areas",
        "grid-auto-columns", "grid-auto-rows", "grid-auto-flow",
        "grid-column", "grid-row", "grid-area",
        "justify-items", "justify-self",
        "animation", "animation-name", "transition", "transition-property",
        "inset", "inset-inline-start", "inset-block-start",
        "margin-inline", "margin-inline-start", "margin-block-start",
        "padding-inline", "padding-inline-start", "padding-block-start",
    };
    char msg[256];
    for (size_t i = 0; i < sizeof(affirmed)/sizeof(affirmed[0]); i++) {
        const char *nm = affirmed[i];
        int claimed = logit_css_extra_supports_name(nm, (int)strlen(nm));
        int inlib   = in_libcss_table(nm);

        snprintf(msg, sizeof msg,
                 "css_extra's @supports hook still affirms `%s`", nm);
        ck(claimed, msg);

        /* THE ONE THAT ROTS. If this fires, the property has moved into
         * LibCSS and its entry is no longer consulted -- delete the entry
         * (that is the list shrinking as designed), do not "fix" this test. */
        snprintf(msg, sizeof msg,
                 "`%s` is absent from LibCSS's own table, so the hook is still "
                 "on the path (if this FAILS the entry is DEAD TEXT -- delete it)", nm);
        ck(!inlib, msg);
    }

    /* The control for the check itself: a name that IS in LibCSS's table must
     * be seen as such. Without this, in_libcss_table() returning a constant 0
     * would make every assertion above pass for the wrong reason. */
    ck(in_libcss_table("background-image"),
       "control: background-image IS in LibCSS's table (so an entry for it "
       "would be dead text -- the finding this check generalises)");
    ck(in_libcss_table("color"), "control: color is in LibCSS's table");
    ck(!logit_css_extra_supports_name("background-image", 16),
       "and the hook correctly does NOT list background-image");
}

int main(void)
{
    css_init();
    css_viewport(980, 700);
    css_set_post_pass(css_extra_apply);

    t_checked();
    t_enabled_disabled();
    t_target();
    t_supports();
    t_supports_no_dead_text();

    printf("%s: %d checks, %d failed\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
