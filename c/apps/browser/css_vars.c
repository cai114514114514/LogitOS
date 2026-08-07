/* CSS custom properties (var()) preprocessor.
 *
 * The vendored LibCSS predates CSS variables: it rejects `--x: 8px` as an
 * unknown property and rejects `color: var(--x)` as an unparseable value, so a
 * site whose every colour is a var() comes back unstyled. We resolve var() in
 * the raw stylesheet text before LibCSS sees it.
 *
 * WHAT CHANGED, AND WHY IT MATTERED
 *
 * The first version of this file collected `--name: value` with a flat text
 * scan and took the LAST one it saw. Two things are wrong with that, and the
 * second one was visible on essentially every real page:
 *
 *   1. It ignored specificity. `--x` declared once on `:root` and again on a
 *      lower-specificity selector later in the file took the later value, which
 *      the cascade would not have done.
 *
 *   2. It ignored MEDIA CONTEXT. Wikipedia -- and every stylesheet written in
 *      the last five years -- says
 *
 *          :root { --background-color-base: #fff }
 *          @media (prefers-color-scheme: dark) {
 *              :root { --background-color-base: #101418 }
 *          }
 *
 *      A scan that cannot see @media takes #101418 unconditionally, so the dark
 *      theme was applied to every reader forever. LibCSS itself had correctly
 *      declined that block; this pre-pass had already flattened the decision
 *      away before LibCSS was handed the sheet, and won.
 *
 * So the scan is now a real one: brace-matched, at-rule aware, and it asks
 * css_media_matches() -- LibCSS's own parser and matcher, via the
 * css_select_ctx_media_matches patch -- whether an @media block's declarations
 * take part at all. There is exactly ONE media-query evaluator in the browser
 * and this file is a client of it, not a second implementation.
 *
 * Winners are then resolved the way the cascade resolves them: `!important`
 * first, then specificity, then source order -- with one deliberate departure
 * for selectors that are theme SWITCHES rather than rules, explained at
 * rank_beats().
 *
 * WHAT IS STILL AN APPROXIMATION, stated plainly because it bounds what this
 * can be trusted for: custom properties are really INHERITED, per-element,
 * cascaded values, and this produces ONE table for the whole document. That is
 * right for the ~95% case (`:root`/`html`/`body` declares them and everything
 * inherits) and wrong for a page that redefines `--x` on a subtree -- a themed
 * card that overrides `--fg` for its own contents gets that value applied
 * document-wide if its selector is the more specific one. Fixing that properly
 * means deferring VALUE parsing to selection time, which is a rewrite of
 * LibCSS's parse-to-bytecode pipeline rather than a patch to it.
 *
 * Pure text in/out except for the one media call, so it stays unit-testable on
 * the host (tests/unit/css_vars_test.c). */

unsigned long strlen(const char *);
int css_media_matches(const char *query, int len);

/* Sized from the corpus, not from taste: a single modern application's
 * stylesheet set defines well over a thousand custom properties (a design-token
 * sheet plus a utility framework's --tw-* registers), and the old 1024x160
 * silently dropped both the overflow names and the tail of every long value --
 * a truncated `--shadow-lg: 0 10px 15px -3px rgb(0 0 0 / 0.1), 0 4px 6px...`
 * expands to a syntactically broken declaration that LibCSS then discards
 * whole. Names run to ~40 chars in practice (--background-color-interactive-
 * subtle); values to ~180 (multi-layer shadows and gradients). */
#define MAXVARS 2048
#define VNAME   64
#define VVAL    192

struct vent {
    char name[VNAME];
    char val[VVAL];
    unsigned spec;                 /* packed CSS specificity, a<<20|b<<10|c */
    unsigned char important;
    unsigned char gated;           /* the declaring selector is a THEME SWITCH:
                                    * every arm of it is qualified by a class,
                                    * id or attribute. See rank_beats(). */
};
static struct vent vars_[MAXVARS];
static int  nvars_;

static int vid(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'; }
static int spc_(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

static int var_find(const char *name, int nlen){
    for (int i = 0; i < nvars_; i++) {
        int j = 0; for (; j < nlen && vars_[i].name[j]; j++) if (vars_[i].name[j] != name[j]) break;
        if (j == nlen && vars_[i].name[j] == 0) return i;
    }
    return -1;
}

/* Does the incoming declaration outrank the one already held?
 *
 * !important first, then UNGATED-BEATS-GATED, then specificity, then source
 * order (the scan runs in source order, so "later" is "now" -- hence >= on the
 * specificity compare).
 *
 * The middle term is not in the CSS cascade and is the one thing here that
 * deliberately departs from it, so it is worth being precise about why.
 * Wikipedia declares its palette twice:
 *
 *     :root, .skin-invert, .notheme        { --background-color-base: #fff    }
 *     html.skin-theme-clientpref-night     { --background-color-base: #101418 }
 *
 * Both are outside any @media that fails, so the media gate cannot separate
 * them, and `html.skin-theme-clientpref-night` (0,1,1) genuinely outranks
 * `:root` (0,1,0) on specificity. In a real browser that does not matter,
 * because the second rule only applies to an <html> that CARRIES that class --
 * it is a switch, thrown by a user preference. We have one variable table for
 * the whole document and no element to test the switch against, so honouring
 * specificity here means every reader gets the night palette: exactly the bug,
 * in a second disguise.
 *
 * So a declaration whose every selector arm is qualified by a class, id or
 * attribute is treated as conditional and yields to an unqualified one. The
 * effect is that a page renders in its BASE theme, which is what a browser
 * shows before any switch is thrown.
 *
 * THE COST, stated exactly: a page that ships the switch already thrown --
 * server-rendered `<html class="dark">`, which is how Tailwind's dark mode
 * works -- renders light. Gating on the real document instead would fix that,
 * and was not done: css_expand_vars runs on the sheet BEFORE the document is
 * styled and takes no node, so the only document it could consult is the
 * PREVIOUS page's. A gate that is silently one navigation stale is worse than
 * a conservative one, because it fails differently depending on where you came
 * from. Closing this properly means custom properties becoming real per-element
 * cascaded values -- see the file header. */
static int rank_beats(const struct vent *cur, unsigned spec, int important, int gated)
{
    if (!!important != !!cur->important) return !!important;
    if (!!gated != !!cur->gated) return !gated;
    return spec >= cur->spec;
}

/* Record `--name: value` as declared by a selector of specificity `spec`. */
static void var_set(const char *name, int nlen, const char *val, int vlen,
                    unsigned spec, int important, int gated){
    if (nlen <= 0 || nlen >= VNAME) return;
    if (vlen >= VVAL) vlen = VVAL - 1; if (vlen < 0) vlen = 0;
    int idx = var_find(name, nlen);
    if (idx < 0) {
        if (nvars_ >= MAXVARS) return;
        idx = nvars_++;
        for (int k = 0; k < nlen; k++) vars_[idx].name[k] = name[k];
        vars_[idx].name[nlen] = 0;
    } else if (!rank_beats(&vars_[idx], spec, important, gated)) {
        return;
    }
    for (int k = 0; k < vlen; k++) vars_[idx].val[k] = val[k];
    vars_[idx].val[vlen] = 0;
    vars_[idx].spec = spec;
    vars_[idx].important = (unsigned char)!!important;
    vars_[idx].gated = (unsigned char)!!gated;
}

/* CSS specificity of a selector list: (#id, .class/[attr]/:pseudo-class,
 * element/::pseudo-element), packed into one comparable integer.
 *
 * A comma list takes the MAXIMUM of its arms. Only the arm that actually
 * matched contributes in a real cascade, and we do not know which -- but a
 * sheet writes `:root, [data-theme="light"] { --bg: #fff }` precisely so that
 * both arms carry the same value, and taking the max keeps such a rule from
 * being outranked by a plain-element rule elsewhere.
 *
 * *gated (may be NULL) reports whether EVERY arm is qualified by a class, id or
 * attribute -- see rank_beats(). Every arm, not any: `:root, .skin-invert` is
 * ungated because `:root` alone applies unconditionally, which is precisely how
 * a palette that also wants to be reachable through a wrapper class is written.
 * A pseudo-class does NOT gate: `:root` and `:where(html)` name the element
 * every document has, they are not switches.
 *
 * Simplification worth knowing about: a functional pseudo's argument is SKIPPED
 * rather than scored. Real CSS gives `:not(X)`/`:is(X)` the specificity of
 * their most specific argument (and `:where(X)` zero), so `p:not(.a)` is
 * (0,1,1) here and (0,1,1) there by luck, while `p:not(.a.b)` is (0,1,1) here
 * and (0,2,1) there. It has never decided a custom-property winner in the
 * corpus -- sheets declare their tokens on `:root`, not on `:not()` chains --
 * and skipping is the safe direction, since counting the argument as top-level
 * text would inflate rather than deflate. */
static unsigned selector_spec(const char *s, int len, int *gated)
{
    unsigned best = 0, a = 0, b = 0, c = 0;
    int i = 0, arm_gated = 0, all_gated = 1, arms = 0;
    while (i <= len) {
        if (i == len || s[i] == ',') {
            unsigned v = (a << 20) | (b << 10) | c;
            if (v > best) best = v;
            /* An empty trailing arm (a stray comma) is not an arm. */
            if (a || b || c) { arms++; if (!arm_gated) all_gated = 0; }
            a = b = c = 0; arm_gated = 0;
            i++;
            continue;
        }
        char ch = s[i];
        if (ch == '#') { a++; arm_gated = 1; i++; while (i < len && vid(s[i])) i++; }
        else if (ch == '.') { b++; arm_gated = 1; i++; while (i < len && vid(s[i])) i++; }
        else if (ch == '[') { b++; arm_gated = 1; while (i < len && s[i] != ']') i++; if (i < len) i++; }
        else if (ch == ':') {
            if (i + 1 < len && s[i+1] == ':') { c++; i += 2; }
            else { b++; i++; }
            while (i < len && vid(s[i])) i++;
            /* a functional pseudo -- :not(...), :is(...), :nth-child(...) --
             * carries a parenthesised argument that must not be counted as if
             * it were top-level selector text. */
            if (i < len && s[i] == '(') {
                int d = 0;
                while (i < len) { if (s[i] == '(') d++; else if (s[i] == ')') { d--; if (!d) { i++; break; } } i++; }
            }
        }
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
            c++; while (i < len && (vid(s[i]) || s[i] == '|')) i++;
        }
        else i++;              /* combinators, '*', whitespace: no contribution */
    }
    if (gated) *gated = (arms > 0) && all_gated;
    return best;
}

/* substitute var() in s[0..n) into out (single pass); returns out length.
 * `depth` bounds fallback recursion: nested var(--x, var(--x, ...)) needs only
 * ~8 input bytes per level, so an unbounded recursion would blow the ring-3
 * stack on a large stylesheet. Past the limit the fallback is left unexpanded. */
#define VAR_MAX_DEPTH 32
static int var_subst(const char *s, int n, char *out, int omax, int depth){
    int o = 0;
    for (int i = 0; i < n && o < omax - 1;) {
        if (i + 4 <= n && s[i]=='v'&&s[i+1]=='a'&&s[i+2]=='r'&&s[i+3]=='(') {
            int p = i + 4; while (p < n && s[p]==' ') p++;
            if (p + 1 < n && s[p]=='-' && s[p+1]=='-') {
                int ns = p + 2, ne = ns; while (ne < n && vid(s[ne])) ne++;
                int depth_ = 1, r = ne, fbs = -1;
                while (r < n && depth_ > 0) { char ch = s[r];
                    if (ch=='(') depth_++; else if (ch==')') { depth_--; if (!depth_) break; }
                    else if (ch==',' && depth_==1 && fbs < 0) fbs = r + 1; r++; }
                int close = r;                            /* s[close]==')' or n */
                int idx = var_find(s + ns, ne - ns);
                if (idx >= 0) { for (int k = 0; vars_[idx].val[k] && o < omax-1; k++) out[o++] = vars_[idx].val[k]; }
                else if (fbs >= 0 && depth < VAR_MAX_DEPTH) { int fe = close; while (fbs < fe && s[fbs]==' ') fbs++;
                                     o += var_subst(s + fbs, fe - fbs, out + o, omax - o, depth + 1); }  /* fallback may itself be var() */
                i = (close < n) ? close + 1 : n;
                continue;
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0; return o;
}
static int has_var(const char *s){ for (; s[0]; s++) if (s[0]=='v'&&s[1]=='a'&&s[2]=='r'&&s[3]=='(') return 1; return 0; }

/* ---- the collecting scan ----
 *
 * One pass, brace-matched, with a stack of at-rule contexts. `collect` is the
 * AND of every enclosing context's verdict, so a `--x` two @media deep is
 * collected only if BOTH queries hold. */

#define AT_DEPTH 24

/* Skip a /*...*(/ comment or a quoted string starting at *i. 1 if it consumed
 * anything. Both must be skipped by the scanner or a ':' inside a url() or a
 * '{' inside a content:"{" string derails the whole parse. */
static int skip_noise(const char *s, int n, int *i)
{
    int p = *i;
    if (p + 1 < n && s[p] == '/' && s[p+1] == '*') {
        p += 2;
        while (p + 1 < n && !(s[p] == '*' && s[p+1] == '/')) p++;
        *i = (p + 1 < n) ? p + 2 : n;
        return 1;
    }
    if (s[p] == '"' || s[p] == '\'') {
        char q = s[p++];
        while (p < n && s[p] != q) { if (s[p] == '\\' && p + 1 < n) p++; p++; }
        *i = (p < n) ? p + 1 : n;
        return 1;
    }
    return 0;
}

/* Does the prelude name an at-rule whose body holds DECLARATIONS that are not
 * element custom properties? @keyframes steps, @font-face descriptors and
 * @property's initial-value all look like `--x: y` to a naive scan but none of
 * them defines a variable for the document. */
static int at_is_opaque(const char *s, int len)
{
    static const char *op[] = { "keyframes", "font-face", "property", "page",
                                "counter-style", "font-feature-values",
                                "-webkit-keyframes", "-moz-keyframes" };
    int i = 1;                                  /* s[0] == '@' */
    int st = i; while (i < len && (vid(s[i]))) i++;
    int nl = i - st;
    for (unsigned k = 0; k < sizeof op / sizeof *op; k++) {
        const char *o = op[k]; int ol = 0; while (o[ol]) ol++;
        if (ol != nl) continue;
        int j = 0; for (; j < nl; j++) { char a = s[st+j]; if (a >= 'A' && a <= 'Z') a += 32; if (a != o[j]) break; }
        if (j == nl) return 1;
    }
    return 0;
}

/* Collect the `--name: value` declarations of one rule body [d, dend). */
static void collect_decls(const char *s, int d, int dend, unsigned spec, int gated)
{
    int i = d;
    while (i < dend) {
        if (skip_noise(s, dend, &i)) continue;
        char ch = s[i];
        if (ch == '{') {                       /* a nested rule (CSS nesting): skip */
            int depth = 1; i++;
            while (i < dend && depth) { if (skip_noise(s, dend, &i)) continue;
                                        if (s[i] == '{') depth++; else if (s[i] == '}') depth--; i++; }
            continue;
        }
        if (ch != '-' || i + 1 >= dend || s[i+1] != '-') { i++; continue; }
        /* must start a declaration */
        int k = i - 1;
        while (k >= d && spc_(s[k])) k--;
        if (k >= d && s[k] != ';' && s[k] != '{' && s[k] != '}') { i++; continue; }
        int ns = i + 2, ne = ns;
        while (ne < dend && vid(s[ne])) ne++;
        int p = ne; while (p < dend && spc_(s[p])) p++;
        if (p >= dend || s[p] != ':') { i = ne; continue; }
        p++;
        while (p < dend && spc_(s[p])) p++;
        int vs = p, vdepth = 0;
        while (p < dend) {
            if (skip_noise(s, dend, &p)) continue;
            char c2 = s[p];
            if (c2 == '(') vdepth++;
            else if (c2 == ')') { if (vdepth) vdepth--; }
            else if (!vdepth && (c2 == ';' || c2 == '}')) break;
            p++;
        }
        int ve = p;
        while (ve > vs && spc_(s[ve-1])) ve--;
        /* `!important` is a cascade flag, not part of the value */
        int important = 0;
        for (int q = vs; q + 10 <= ve; q++) {
            if (s[q] != '!') continue;
            int r = q + 1; while (r < ve && spc_(s[r])) r++;
            if (r + 9 <= ve) {
                int m = 1;
                static const char *imp = "important";
                for (int z = 0; z < 9; z++) { char a = s[r+z]; if (a>='A'&&a<='Z') a += 32; if (a != imp[z]) { m = 0; break; } }
                if (m) { important = 1; ve = q; while (ve > vs && spc_(s[ve-1])) ve--; break; }
            }
        }
        var_set(s + ns, ne - ns, s + vs, ve - vs, spec, important, gated);
        i = (p < dend) ? p + 1 : dend;
    }
}

static void collect(const char *s, int n)
{
    int active[AT_DEPTH];               /* per enclosing at-rule: collect inside? */
    int adepth = 0, i = 0;
    nvars_ = 0;
    while (i < n) {
        if (skip_noise(s, n, &i)) continue;
        if (spc_(s[i])) { i++; continue; }
        if (s[i] == '}') { if (adepth > 0) adepth--; i++; continue; }
        if (s[i] == ';') { i++; continue; }

        /* Read the prelude up to '{' or ';'. */
        int ps = i;
        while (i < n) {
            if (skip_noise(s, n, &i)) continue;
            if (s[i] == '{' || s[i] == ';' || s[i] == '}') break;
            i++;
        }
        int pe = i;
        while (pe > ps && spc_(s[pe-1])) pe--;
        if (i >= n) break;
        /* The prelude ran into the '}' that closes the block we are INSIDE.
         * That happens for every at-rule whose body holds bare descriptors
         * rather than nested rules -- @counter-style's `symbols: '\ABF0' ...`,
         * @font-face's `src:`, @page's margins. The text we just skipped was
         * junk, but the brace is not: it must reach the pop branch at the top
         * of the loop. Consuming it here is how the scanner used to drift a
         * level deeper at every such at-rule; three @counter-style blocks
         * 87 KB into wikipedia's stylesheet left it permanently "inside" an
         * inactive region, and it collected nothing at all from the 100 KB of
         * design tokens that followed -- including every colour the page is
         * painted with. */
        if (s[i] == '}') continue;
        if (s[i] != '{') { i++; continue; }             /* @import ...; and strays */
        i++;                                            /* past '{' */

        int inherited = (adepth == 0) || active[adepth-1];

        if (pe > ps && s[ps] == '@') {
            int opaque = at_is_opaque(s + ps, pe - ps);
            int ok = inherited && !opaque;
            if (ok) {
                /* @media: ask LibCSS. Every other conditional group rule
                 * (@supports, @layer, @scope, @container) is entered: we cannot
                 * evaluate it, and entering matches what a browser that DOES
                 * support the feature would do, which is the side a modern
                 * sheet is written for. */
                int nl = 1; int q = ps + 1;
                while (q < pe && vid(s[q])) q++;
                nl = q - ps - 1;
                if (nl == 5) {
                    char b[6]; for (int z = 0; z < 5; z++) { char a = s[ps+1+z]; b[z] = (a>='A'&&a<='Z')?a+32:a; } b[5]=0;
                    if (b[0]=='m'&&b[1]=='e'&&b[2]=='d'&&b[3]=='i'&&b[4]=='a')
                        ok = css_media_matches(s + q, pe - q);
                }
            }
            if (adepth < AT_DEPTH) active[adepth++] = ok;
            else { /* absurd nesting: brace-skip the whole block */
                int depth = 1;
                while (i < n && depth) { if (skip_noise(s, n, &i)) continue;
                                         if (s[i]=='{') depth++; else if (s[i]=='}') depth--; i++; }
            }
            continue;
        }

        /* A style rule: brace-match its body and read the declarations. */
        int d = i, depth = 1;
        while (i < n && depth) {
            if (skip_noise(s, n, &i)) continue;
            if (s[i] == '{') depth++;
            else if (s[i] == '}') { depth--; if (!depth) break; }
            i++;
        }
        int dend = i;
        if (i < n) i++;                                 /* past the closing '}' */
        if (inherited && dend > d) {
            int gated = 0;
            unsigned spec = selector_spec(s + ps, pe - ps, &gated);
            collect_decls(s, d, dend, spec, gated);
        }
    }
}

int css_expand_vars(const char *in, int inlen, char *out, int outmax){
    collect(in, inlen);
    for (int it = 0; it < 8; it++) {                   /* resolve var()-in-value chains */
        int changed = 0; char tmp[VVAL];
        for (int i = 0; i < nvars_; i++) {
            if (!has_var(vars_[i].val)) continue;
            int tn = var_subst(vars_[i].val, (int)strlen(vars_[i].val), tmp, VVAL, 0);
            int same = 1; for (int k = 0; k <= tn; k++) if (tmp[k] != vars_[i].val[k]) { same = 0; break; }
            if (!same) { for (int k = 0; k <= tn; k++) vars_[i].val[k] = tmp[k]; changed = 1; }
        }
        if (!changed) break;
    }
    return var_subst(in, inlen, out, outmax, 0);       /* substitute throughout */
}

/* Test seam: how many distinct custom properties the last expansion collected,
 * and the winning value of one of them. Lets the host test assert on the
 * CASCADE decision directly instead of inferring it from substituted output. */
int css_vars_count(void) { return nvars_; }
const char *css_vars_value(const char *name)
{
    int nlen = 0; while (name[nlen]) nlen++;
    if (nlen > 2 && name[0] == '-' && name[1] == '-') { name += 2; nlen -= 2; }
    int i = var_find(name, nlen);
    return i < 0 ? 0 : vars_[i].val;
}
