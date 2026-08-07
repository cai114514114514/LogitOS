/* Fuzz the custom-property scanner in c/apps/browser/css_vars.c.
 *
 * The scanner walks attacker-controlled stylesheet bytes with raw index
 * arithmetic -- it looks backwards from a declaration start (s[i-1]), trims
 * trailing whitespace off spans (s[pe-1]), brace-matches, and skips comments
 * and quoted strings that may be unterminated at end of buffer. Every one of
 * those is an out-of-bounds read away from being wrong on a truncated or
 * malicious sheet, and none of it shows up as a wrong ANSWER, which is what the
 * unit test measures. So: run it under ASan/UBSan over mutated CSS and require
 * only that it returns.
 *
 * Two corpora. `SEEDS` are the shapes that actually broke something during
 * development (an at-rule body of bare descriptors, an unterminated comment, a
 * '}' inside a string); the mutator then chops, truncates and byte-flips them,
 * which is what produces the half-token inputs at a buffer edge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css.h"

static const char *const SEEDS[] = {
    ":root{--a:1px}",
    ":root{--a:1px}@media (prefers-color-scheme:dark){:root{--a:2px}}",
    "@counter-style x{system:numeric;symbols:'\\1' '\\2'}:root{--a:3}",
    "@font-face{src:url(a.woff2);--a:4}",
    "@media screen and (min-width:600px){@supports (d:g){.c{--a:5}}}",
    "p::after{content:\"}\";--a:6}",
    "/* --a:7 */:root{--a:8}",
    ":root{--a:var(--b,var(--c,9))}",
    ":root{--a:url(x.png?q=;&r=})}",
    "@media",
    "@media{",
    "{--a:1}",
    "}}}}--a:1",
    ":root{--:1}",
    ":root{--a:}",
    ":root{--a",
    "/*",
    "\"",
    "'",
    ":root{--a:1 !important}#b{--a:2 !important}",
    ":root,,,.x,{--a:1}",
    ":not(:is(:where(.a))){--a:1}",
    "@media (min-width:calc(640px - 1px)){:root{--a:1}}",
};
#define NSEED ((int)(sizeof SEEDS / sizeof *SEEDS))

static unsigned long rs = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 33);
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 20000;
    static char in[8192], out[65536];

    css_init();
    css_viewport(1000, 700);

    for (int it = 0; it < iters; it++) {
        /* Build an input by concatenating 1..4 seeds, then damaging it. */
        int n = 0, parts = 1 + (int)(rnd() % 4);
        for (int p = 0; p < parts; p++) {
            const char *s = SEEDS[rnd() % NSEED];
            int l = (int)strlen(s);
            if (n + l >= (int)sizeof in) break;
            memcpy(in + n, s, l); n += l;
        }
        if (n == 0) continue;

        switch (rnd() % 5) {
        case 0: break;                                    /* untouched */
        case 1: n = 1 + (int)(rnd() % (unsigned)n); break; /* truncate */
        case 2: {                                          /* byte flips */
            int k = 1 + (int)(rnd() % 8);
            while (k--) in[rnd() % (unsigned)n] = (char)(rnd() & 0xFF);
            break;
        }
        case 3: {                                          /* delete a span */
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (a > b) { int t = a; a = b; b = t; }
            memmove(in + a, in + b, (size_t)(n - b)); n -= (b - a);
            break;
        }
        case 4: {                                          /* brace storm */
            int k = 1 + (int)(rnd() % 16);
            while (k-- && n < (int)sizeof in)
                in[n++] = "{}();:'\"/*\\"[rnd() % 11];
            break;
        }
        }
        if (n <= 0) continue;

        /* Flip the preference occasionally so the media path is exercised in
         * both states. */
        if ((it & 63) == 0) css_set_color_scheme(it & 64 ? 1 : 0);

        /* The only contract under fuzz is that it returns, in bounds, having
         * written a NUL-terminated result no longer than it was given room for. */
        int r = css_expand_vars(in, n, out, (int)sizeof out);
        if (r < 0 || r >= (int)sizeof out) {
            printf("FAIL: css_expand_vars returned %d for a %d-byte input\n", r, n);
            return 1;
        }
        if (out[r] != 0) {
            printf("FAIL: result not NUL-terminated at %d\n", r);
            return 1;
        }
        /* A tiny output buffer must be respected too. */
        char small[24];
        int r2 = css_expand_vars(in, n, small, (int)sizeof small);
        if (r2 < 0 || r2 >= (int)sizeof small || small[r2] != 0) {
            printf("FAIL: small-buffer expansion returned %d\n", r2);
            return 1;
        }
        (void)css_vars_count();
    }

    printf("css_vars_fuzz: %d inputs, no crash, no overflow\n", iters);
    return 0;
}
