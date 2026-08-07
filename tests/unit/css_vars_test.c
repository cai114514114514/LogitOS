/* Host unit test for the CSS custom-property pre-pass (c/apps/browser/css_vars.c)
 * and for the single media-query evaluator it now shares with LibCSS
 * (css_media_matches -> css_select_ctx_media_matches).
 *
 * The decisive test in this file is `dark-not-applied-in-light-mode`, and it is
 * a NEGATIVE CONTROL: it fails on the previous implementation. That version
 * collected `--name: value` with a flat last-wins text scan, so
 *
 *     :root { --bg: #ffffff }
 *     @media (prefers-color-scheme: dark) { :root { --bg: #101418 } }
 *
 * resolved to #101418 for every reader regardless of preference, and every real
 * page -- wikipedia included -- rendered dark. LibCSS had already declined that
 * @media block; the pre-pass flattened the decision away before LibCSS saw the
 * sheet, and won. This asserts the light value survives, and then flips the
 * preference and asserts the dark one takes over -- so it pins BOTH directions
 * rather than just the one that happens to be the default.
 *
 * Build: see the test-browser rule in the Makefile (needs css_engine.c and
 * libcss_host.a, because the media evaluator is LibCSS's). */
#include <stdio.h>
#include <string.h>

#include "dom.h"
#include "css.h"

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("FAIL %s\n", what); fails++; }
    else printf("ok   %s\n", what);
}

/* Expand `css` and assert the CASCADE's winner for `--name`. Asserting on the
 * variable table rather than on the substituted text is deliberate: it
 * distinguishes "the right value won" from "a value was substituted". */
static void want_var(const char *css, const char *name, const char *want,
                     const char *what)
{
    static char out[1 << 20];
    css_expand_vars(css, (int)strlen(css), out, (int)sizeof out);
    const char *got = css_vars_value(name);
    int ok = want ? (got && !strcmp(got, want)) : (got == 0);
    checks++;
    if (!ok) {
        printf("FAIL %s\n  %s = %s, want %s\n", what, name,
               got ? got : "(undeclared)", want ? want : "(undeclared)");
        fails++;
    } else {
        printf("ok   %s (%s = %s)\n", what, name, got ? got : "(undeclared)");
    }
}

static void want_out(const char *css, const char *want, const char *what)
{
    static char out[1 << 20];
    css_expand_vars(css, (int)strlen(css), out, (int)sizeof out);
    checks++;
    if (strcmp(out, want)) {
        printf("FAIL %s\n  got:  %s\n  want: %s\n", what, out, want);
        fails++;
    } else printf("ok   %s\n", what);
}

int main(void)
{
    css_init();
    css_viewport(1000, 700);

    /* ---------------- the reported bug, both directions ---------------- */

    /* Exactly wikipedia's shape. */
    static const char THEME[] =
        ":root{--background-color-base:#ffffff;--color-base:#202122}"
        "@media (prefers-color-scheme: dark){"
        ":root{--background-color-base:#101418;--color-base:#eaecf0}}"
        "body{background-color:var(--background-color-base);color:var(--color-base)}";

    css_set_color_scheme(0);
    ck(css_color_scheme() == 0, "default colour scheme is light");
    want_var(THEME, "--background-color-base", "#ffffff",
             "NEGATIVE CONTROL: a dark-only @media override does NOT win in light mode");
    want_var(THEME, "--color-base", "#202122",
             "the light foreground survives too");

    css_set_color_scheme(1);
    want_var(THEME, "--background-color-base", "#101418",
             "with the preference flipped, the dark override DOES win");
    want_var(THEME, "--color-base", "#eaecf0",
             "the dark foreground wins too");
    css_set_color_scheme(0);

    /* The other half of the same mistake: a sheet that puts its LIGHT theme in
     * an @media block. With media->prefers_color_scheme left NULL, as it was,
     * NEITHER block matched and such a page got no theme at all. */
    want_var(":root{--fg:#000}"
             "@media (prefers-color-scheme: light){:root{--fg:#222}}"
             "@media (prefers-color-scheme: dark){:root{--fg:#eee}}",
             "--fg", "#222",
             "a light-only @media block DOES match in light mode");

    /* ---------------- width media queries ---------------- */

    css_viewport(1000, 700);
    want_var(":root{--pad:8px}@media (min-width:900px){:root{--pad:24px}}",
             "--pad", "24px", "min-width:900px holds at a 1000px viewport");
    want_var(":root{--pad:8px}@media (min-width:1200px){:root{--pad:24px}}",
             "--pad", "8px", "min-width:1200px does not hold at a 1000px viewport");
    want_var(":root{--pad:8px}@media (max-width:600px){:root{--pad:2px}}",
             "--pad", "8px", "max-width:600px does not hold at a 1000px viewport");

    /* Nested conditional groups AND together. */
    want_var(":root{--x:a}"
             "@media (min-width:900px){@media (prefers-color-scheme: dark){:root{--x:b}}}",
             "--x", "a", "nested @media: the inner dark query fails, so the block is skipped");
    want_var(":root{--x:a}"
             "@media (min-width:900px){@media (min-width:800px){:root{--x:b}}}",
             "--x", "b", "nested @media: both hold, so the block applies");

    /* The evaluator itself, called the way js_webapi's matchMedia should. */
    ck(css_media_matches("(min-width: 900px)", -1) == 1, "css_media_matches: min-width holds");
    ck(css_media_matches("(min-width: 1400px)", -1) == 0, "css_media_matches: min-width fails");
    ck(css_media_matches("(prefers-color-scheme: dark)", -1) == 0,
       "css_media_matches: dark is false in light mode");
    ck(css_media_matches("", -1) == 1, "css_media_matches: an empty query is `all`");
    ck(css_media_matches("screen", -1) == 1, "css_media_matches: screen matches");
    ck(css_media_matches("print", -1) == 0, "css_media_matches: print does not");

    /* ---------------- cascade order ---------------- */

    /* Later source order only wins at EQUAL specificity. A plain-element rule
     * after a more specific one must not overwrite it -- the old scan always
     * took the last one it saw. Both selectors here are unqualified, so the
     * theme-switch rule below is not what is being measured. */
    want_var(":root{--c:red}html{--c:blue}", "--c", "red",
             "higher specificity wins over later source order");
    want_var("html{--c:blue}:root{--c:red}", "--c", "red",
             "...in either order");
    want_var(":root{--c:one}:root{--c:two}", "--c", "two",
             "equal specificity: later source order wins");
    want_var(":root{--c:one !important}:root{--c:two}", "--c", "one",
             "!important is not overwritten by a later normal declaration");
    want_var(":root{--c:one}#a{--c:two !important}", "--c", "two",
             "!important survives with the bang stripped from the value");

    /* A selector list takes the maximum specificity of its arms, which is what
     * keeps `:root, [data-theme=light] { --bg: #fff }` from being outranked. */
    want_var(":root,[data-theme=light]{--bg:#fff}p{--bg:#000}", "--bg", "#fff",
             "a selector list is ranked by its most specific arm");

    /* ---------------- theme switches ----------------
     *
     * Wikipedia's second disguise of the same bug: no @media is involved, and
     * `html.skin-theme-clientpref-night` (0,1,1) genuinely outranks `:root`
     * (0,1,0). A class/id/attribute-qualified selector is a switch, and with no
     * element to test it against the base value has to win -- otherwise every
     * reader gets the night palette again. */
    want_var(":root,.skin-invert,.notheme{--bg:#fff}"
             "html.skin-theme-clientpref-night{--bg:#101418}",
             "--bg", "#fff",
             "a class-gated theme override yields to the unqualified base value");
    want_var("[data-theme=dark]{--bg:#101418}:root{--bg:#fff}", "--bg", "#fff",
             "an attribute-gated override yields too, in either source order");
    want_var("#theme-dark{--bg:#101418}html{--bg:#fff}", "--bg", "#fff",
             "an id-gated override yields even though an id is the most specific thing there is");
    /* ...but a gate is only overridden by an UNGATED declaration. Between two
     * gated ones the normal cascade resumes. */
    want_var(".a{--bg:#111}#b{--bg:#222}", "--bg", "#222",
             "between two gated declarations, specificity decides normally");
    /* ...and a pseudo-class is not a gate: :root and :where(html) name the
     * element every document has. */
    want_var(":root{--bg:#fff}:where(html){--bg:#eee}", "--bg", "#eee",
             ":root and :where(html) are both ungated, so source order decides");

    /* A functional pseudo's argument must not be read as top-level selector
     * text: if the two classes inside :not() were counted that way, `:not(.a.b)`
     * would score (0,3,0) and outrank `:root` (0,1,0), and --z would come back
     * 1. (Real CSS gives :not() its argument's specificity, which we do not
     * implement -- see selector_spec. What is asserted here is only that the
     * argument is SKIPPED rather than double-counted.) */
    want_var(":not(.a.b){--z:1}:root{--z:2}", "--z", "2",
             "a functional pseudo's argument is skipped, not counted as selector text");

    /* ---------------- what must NOT be collected ---------------- */

    want_var(":root{--o:0}"
             "@keyframes fade{from{--o:9;opacity:0}to{opacity:1}}",
             "--o", "0", "@keyframes steps do not declare document variables");
    want_var(":root{--src:a}@font-face{font-family:x;--src:b;src:url(x.woff2)}",
             "--src", "a", "@font-face descriptors do not declare document variables");
    want_var(":root{--g:1}@property --g{syntax:'<number>';initial-value:7;inherits:false}",
             "--g", "1", "@property's initial-value does not overwrite the cascade");

    /* @supports is ENTERED: we cannot evaluate it, and a modern sheet writes it
     * expecting the feature to be present. */
    want_var(":root{--c:old}@supports (display:grid){:root{--c:new}}", "--c", "new",
             "@supports is entered rather than skipped");

    /* ---------------- scanner robustness ---------------- */

    want_var("/* :root{--c:comment} */:root{--c:real}", "--c", "real",
             "a declaration inside a comment is not collected");
    /* If the '}' in the string ended the rule, `--c:instring` would be loose
     * text outside any rule and would never be collected at all -- so the
     * assertion is simply that it WAS collected. */
    want_var("p::after{content:\"}\";--c:instring}", "--c", "instring",
             "a '}' inside a string does not end the rule early");
    /* The same shape for the at-rule stack, and it is the failure that cost a
     * whole stylesheet: an at-rule whose body holds bare descriptors rather
     * than nested rules ends with a '}' that the prelude scanner used to
     * swallow without popping. Three @counter-style blocks 87 KB into
     * wikipedia's sheet left the scanner permanently inside an inactive region
     * and it collected 5 custom properties out of 193. */
    want_var("@counter-style thai{system:numeric;symbols:'\\E50' '\\E51'}"
             "@counter-style lao{system:numeric;symbols:'\\ED0' '\\ED1'}"
             ":root{--after:collected}",
             "--after", "collected",
             "an at-rule body of bare descriptors does not swallow its own closing brace");
    want_var(":root{--img:url(a.png?x=;y)}", "--img", "url(a.png?x=;y)",
             "a ';' inside a function does not truncate the value");
    want_var(":root{--sh:0 1px 2px rgba(0,0,0,.1), 0 4px 8px rgba(0,0,0,.06)}",
             "--sh", "0 1px 2px rgba(0,0,0,.1), 0 4px 8px rgba(0,0,0,.06)",
             "a multi-layer shadow value survives whole");
    want_var("p{color:red}", "--nope", 0, "an undeclared variable reports undeclared");

    /* ---------------- substitution still works ---------------- */

    want_out(":root{--bg:#0d1117}body{background-color:var(--bg)}",
             ":root{--bg:#0d1117}body{background-color:#0d1117}", "direct substitution");
    want_out("a{color:var(--nope,#ff8800)}", "a{color:#ff8800}", "fallback when undeclared");
    want_out(":root{--green:#238636;--accent:var(--green)}b{background:var(--accent)}",
             ":root{--green:#238636;--accent:#238636}b{background:#238636}",
             "indirection is resolved");
    /* The end-to-end shape of the bug: the substituted sheet must carry the
     * LIGHT colour into the property LibCSS will actually parse. */
    {
        static char out[1 << 20];
        css_set_color_scheme(0);
        css_expand_vars(THEME, (int)strlen(THEME), out, (int)sizeof out);
        ck(strstr(out, "background-color:#ffffff") != 0 &&
           strstr(out, "background-color:#101418") == 0,
           "the substituted sheet carries the light colour into background-color");
        css_set_color_scheme(1);
        css_expand_vars(THEME, (int)strlen(THEME), out, (int)sizeof out);
        ck(strstr(out, "background-color:#101418") != 0,
           "...and the dark one once the preference flips");
        css_set_color_scheme(0);
    }

    /* ---------------- <general-enclosed> does not crash the browser ----------
     *
     * SECOND NEGATIVE CONTROL, and it is a crash rather than a wrong colour.
     * `(min-widtcalc(640px - 1px))` -- a typo'd feature name that lexes as a
     * FUNCTION token -- is valid CSS: the <general-enclosed> production exists
     * so that syntax from a later level does not invalidate the whole query,
     * and it must evaluate to FALSE. LibCSS's parser accepted it and recorded
     * it as a NULL part; its matcher then read straight through the NULL.
     *
     * This goes in through css_apply, not through css_media_matches, because
     * the point is that it was reachable from any page's own @media -- LibCSS
     * runs the same matcher from mq_rule_good_for_media. On HEAD's mq.h this
     * function segfaults; the fuzz corpus in css_vars_fuzz.c is what found it.
     * The assertion is simply that we get here. */
    {
        static const char H[] = "<!doctype html><html><body><p>hi</p></body></html>";
        static const char C[] = "@media (min-widtcalc(640px - 1px)){p{color:red}}"
                                "@media (someday-feature: 3){p{color:lime}}"
                                "p{color:blue}";
        struct node *r = dom_parse(H, (int)strlen(H));
        ck(r != 0, "fixture document parsed");
        css_apply(r, C, (int)strlen(C));
        ck(1, "an @media whose query is <general-enclosed> does not crash css_apply");
        ck(css_media_matches("(min-widtcalc(640px - 1px))", -1) == 0,
           "...and it evaluates to false, as the spec requires");
        ck(css_media_matches("screen and (someday-feature: 3)", -1) == 0,
           "an unimplemented feature makes its query false, not true");
        ck(css_media_matches("(min-width:900px) or (someday-feature: 3)", -1) == 1,
           "...but only that arm: an `or` with a known true arm still matches");
    }

    printf("\ncss_vars_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
