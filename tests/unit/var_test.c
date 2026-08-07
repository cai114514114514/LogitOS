/* Host unit test for the CSS var() preprocessor (src/apps/browser/css_vars.c).
 * Build: cc -o /tmp/var_test tools/t/var_test.c src/apps/browser/css_vars.c */
#include <stdio.h>
#include <string.h>

int css_expand_vars(const char *in, int inlen, char *out, int outmax);

static int fails;
static void chk(const char *name, const char *css, const char *want)
{
    char out[8192];
    css_expand_vars(css, (int)strlen(css), out, sizeof out);
    if (strcmp(out, want) != 0) {
        printf("FAIL %s\n  in:   %s\n  got:  %s\n  want: %s\n", name, css, out, want);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    /* 1. direct substitution */
    chk("direct",
        ":root{--bg:#0d1117}body{background-color:var(--bg)}",
        ":root{--bg:#0d1117}body{background-color:#0d1117}");

    /* 2. fallback used when undefined */
    chk("fallback-missing",
        "a{color:var(--nope,#ff8800)}",
        "a{color:#ff8800}");

    /* 3. defined wins over fallback */
    chk("fallback-defined",
        ":root{--c:#112233}a{color:var(--c,#ff8800)}",
        ":root{--c:#112233}a{color:#112233}");

    /* 4. indirection: --a -> var(--b) -> literal */
    chk("indirection",
        ":root{--green:#238636;--accent:var(--green)}b{background-color:var(--accent)}",
        ":root{--green:#238636;--accent:#238636}b{background-color:#238636}");

    /* 5. last definition wins (dark overrides light) */
    /* This case used to assert that `.dark` won, because collection was a flat
     * last-wins scan. That IS the bug: `.dark` is a theme switch, it applies
     * only to a document that carries the class, and taking it unconditionally
     * is why every real page rendered dark. The base value wins now -- see
     * rank_beats() in css_vars.c and the cascade cases in css_vars_test.c. */
    chk("theme-switch-yields-to-base",
        ":root{--bg:#ffffff}.dark{--bg:#0d1117}x{color:var(--bg)}",
        ":root{--bg:#ffffff}.dark{--bg:#0d1117}x{color:#ffffff}");

    /* Equal footing: two unqualified selectors, so the later one really does win. */
    chk("last-wins-among-equals",
        ":root{--bg:#ffffff}:root{--bg:#0d1117}x{color:var(--bg)}",
        ":root{--bg:#ffffff}:root{--bg:#0d1117}x{color:#0d1117}");

    /* 6. no custom props -> passthrough */
    chk("passthrough",
        "h1{color:red;font-size:20px}",
        "h1{color:red;font-size:20px}");

    /* 7. fallback that is itself a var() */
    chk("fallback-var",
        ":root{--y:#abcdef}a{color:var(--x,var(--y))}",
        ":root{--y:#abcdef}a{color:#abcdef}");

    /* 8. spaces inside var() */
    chk("spaces",
        ":root{--p:10px}d{padding: var( --p )}",
        ":root{--p:10px}d{padding: 10px}");

    /* 9. multiple uses on one line */
    chk("multi",
        ":root{--a:1px;--b:2px}m{margin:var(--a) var(--b) var(--a) var(--b)}",
        ":root{--a:1px;--b:2px}m{margin:1px 2px 1px 2px}");

    /* 10. undefined, no fallback -> empties out (declaration becomes invalid, fine) */
    chk("undef-nofallback",
        "z{color:var(--gone)}",
        "z{color:}");

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
