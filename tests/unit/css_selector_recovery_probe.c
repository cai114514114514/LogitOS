/* css_selector_recovery_probe -- MEASUREMENT, not a gate.
 *
 * Answers the one question the "selector table stopped at CSS2" diagnosis
 * needs answered on real bytes rather than on a corpus this session wrote:
 * of the rules a real-site corpus loses because this engine's selector
 * parser refuses their selector list outright (parsePseudo()'s pseudo_lut in
 * third_party/css/libcss/src/parse/language.c), how many are recovered by
 * the new table entries, and how many declarations ride inside them?
 *
 * TWO INDEPENDENT INSTRUMENTS, cross-checked against each other rather than
 * trusted alone:
 *
 *   1. THE REAL PARSER says WHICH selector lists are refused. Each corpus
 *      source (one external sheet's whole bytes, or one <style> block's
 *      whole bytes -- never a re-synthesised fragment: an isolated
 *      "selector{}" re-parse loses any @namespace earlier in the SAME
 *      sheet, which false-positived every namespaced selector the first
 *      version of this probe tried that shortcut on) is parsed ONCE through
 *      css_stylesheet_create/append_data/data_done -- the exact API
 *      c/apps/browser/css_engine.c's make_sheet() uses -- with
 *      css__parse_selector_drop_report installed, collecting the REPORTED
 *      TEXT of every rule it refuses. This is authoritative for "was this
 *      selector list accepted": it is the real grammar, not a
 *      re-implementation of it.
 *
 *   2. A DELIBERATELY DUMB TEXT SCANNER enumerates "prelude { body }" rules
 *      independently (bracket depth, comments, strings -- no CSS grammar),
 *      counts declarations in each body, and MATCHES each prelude's
 *      whitespace-normalised text against the pool of reports from (1) for
 *      that same source. A prelude that matches a report is a lost rule;
 *      its declarations are lost declarations. Unmatched reports at the end
 *      of a source are printed as a diagnostic -- the self-check that this
 *      cross-referencing is actually working, not just producing a number.
 *
 * A rule is counted as ONE regardless of how many declarations are in an
 * empty body (0 is legal and counted). Nesting inside @media inside @media
 * is handled by recursion, matching how real corpus CSS is written.
 *
 * Build (see tests/cssweb.mk):
 *   make BUILD=<yours> css-selector-recovery-probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

#include <libcss/libcss.h>

static const char *strcasestr_local(const char *s, const char *e,
                                     const char *needle);

extern void (*css__parse_selector_drop_report)(const char *text, size_t len);

/* ---- 1. the real parser: WHICH selector lists LibCSS actually refuses -- */

static char **g_drop_texts;
static int g_drop_n, g_drop_cap;

/* Collapse runs of whitespace to one space and trim both ends -- the same
 * normalisation report_selector_drop() (parse.c) applies to the token
 * stream, applied here to a raw SOURCE slice so the two sides compare
 * equal for ordinary selectors. Returns a malloc'd NUL-terminated string. */
static char *normalize(const char *s, size_t len)
{
    char *out = malloc(len + 1);
    size_t o = 0;
    int in_str = 0;
    char strch = 0;
    size_t i = 0;

    while (i < len) {
        char c = s[i];
        if (in_str) {
            out[o++] = c;
            if (c == '\\' && i + 1 < len) { out[o++] = s[i + 1]; i += 2; continue; }
            if (c == strch) in_str = 0;
            i++;
            continue;
        }
        if (c == '"' || c == '\'') { in_str = 1; strch = c; out[o++] = c; i++; continue; }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
            i++;
            continue;
        }
        out[o++] = c;
        i++;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    size_t start = 0;
    while (start < o && out[start] == ' ') start++;
    if (start > 0) { memmove(out, out + start, o - start); o -= start; }
    out[o] = '\0';
    return out;
}

static void on_sel_drop(const char *text, size_t len)
{
    char *norm = normalize(text, len);
    /* An unrecognised at-keyword (this corpus's worst offender:
     * "@-webkit-keyframes ...", which this engine's language.c does not
     * special-case the way it does "@keyframes") falls all the way through
     * to ruleset parsing, so its OWN name gets reported through the
     * selector-drop hook -- a real drop, but of a WHOLE AT-RULE, a
     * different diagnosed defect than "the selector table stopped at
     * CSS2". Out of scope here on purpose: counting it as a selector-table
     * loss would credit/blame this table for something it cannot touch. */
    if (norm[0] == '@') { free(norm); return; }
    if (g_drop_n == g_drop_cap) {
        g_drop_cap = g_drop_cap ? g_drop_cap * 2 : 16;
        g_drop_texts = realloc(g_drop_texts, (size_t)g_drop_cap * sizeof(char *));
    }
    g_drop_texts[g_drop_n++] = norm;
}

static void free_drop_pool(void)
{
    for (int i = 0; i < g_drop_n; i++) free(g_drop_texts[i]);
    g_drop_n = 0;
}

/* Remove and report whether `norm` (already normalised) is in the pool. */
static int pool_take(const char *norm)
{
    for (int i = 0; i < g_drop_n; i++) {
        if (strcmp(g_drop_texts[i], norm) == 0) {
            free(g_drop_texts[i]);
            g_drop_texts[i] = g_drop_texts[--g_drop_n];
            return 1;
        }
    }
    return 0;
}

static css_error resolve_url(void *pw, const char *base, lwc_string *rel,
                              lwc_string **abs)
{ (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK; }

/* Parse one whole source (an external sheet's bytes, or one <style> block's
 * bytes) for real, filling g_drop_texts with every refused selector list's
 * normalised text. Every call gets a fresh css_stylesheet AND a fresh pool
 * (free_drop_pool() first) -- one source's drops must never bleed into the
 * next source's matching pass. */
static void collect_real_drops(const char *data, size_t len)
{
    css_stylesheet_params p;
    css_stylesheet *s = NULL;

    free_drop_pool();

    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url = "http://logit/probe.css";
    p.allow_quirks = false;
    p.resolve = resolve_url;

    if (css_stylesheet_create(&p, &s) != CSS_OK || s == NULL)
        return;

    css_stylesheet_append_data(s, (const uint8_t *)data, len);
    css_stylesheet_data_done(s);
    css_stylesheet_destroy(s);
}

/* ---- 2. the dumb scanner: rules + declarations, independent of LibCSS - */

struct totals {
    long rules, decls;
    long lost_rules, lost_decls;
};

/* Does `prelude` (NOT nul-terminated, [s,e)) open a block whose CONTENTS are
 * further rules (recurse) versus raw declarations/opaque data (do not treat
 * as a "rule", do not recurse)? Matched case-insensitively on the at-keyword
 * only -- exactly the boundary parsePseudo()'s own pseudo_lut patch does not
 * touch, so this scanner's rule/non-rule split is independent of it. */
static int atrule_is_group(const char *s, const char *e)
{
    static const char *group[] = {
        "@media", "@supports", "@layer", "@container", "@scope", NULL
    };
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
        s++;
    if (s >= e || *s != '@')
        return 0;
    for (int i = 0; group[i]; i++) {
        size_t n = strlen(group[i]);
        if ((size_t)(e - s) >= n && strncasecmp(s, group[i], n) == 0)
            return 1;
    }
    return 0;
}

static int is_atrule(const char *s, const char *e)
{
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
        s++;
    return s < e && *s == '@';
}

/* Count top-level ';'-terminated declarations in a rule BODY (no braces of
 * its own expected at top level once nested {} -- e.g. inside a value none
 * of this corpus uses -- are skipped by depth tracking). A trailing
 * declaration with no ';' before '}' still counts, matching CSS grammar. */
static long count_decls(const char *s, const char *e)
{
    long n = 0;
    int depth = 0;
    int seen_nonspace = 0;
    int in_str = 0;
    char strch = 0;

    for (const char *p = s; p < e; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < e) { p++; continue; }
            if (c == strch) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'') { in_str = 1; strch = c; continue; }
        if (c == '/' && p + 1 < e && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            p++;
            continue;
        }
        if (c == '{') { depth++; continue; }
        if (c == '}') { if (depth > 0) depth--; continue; }
        if (depth == 0 && !(c == ' ' || c == '\t' || c == '\n' || c == '\r'))
            seen_nonspace = 1;
        if (depth == 0 && c == ';') {
            if (seen_nonspace) n++;
            seen_nonspace = 0;
        }
    }
    if (seen_nonspace) n++; /* last declaration, no trailing ';' */
    return n;
}

/* Scan [s,e) for top-level "prelude { body }" rules, recursing into
 * conditional-group at-rule bodies. For every plain rule (prelude does not
 * start with '@'), calls back with the prelude text so the caller can ask
 * the REAL parser about it, and counts its declarations with count_decls().
 */
static void scan_block(const char *s, const char *e, struct totals *t,
                        void (*on_rule)(const char *ps, const char *pe,
                                        struct totals *t, const char *bs,
                                        const char *be));

static void on_plain_rule(const char *ps, const char *pe, struct totals *t,
                           const char *bs, const char *be)
{
    long decls = count_decls(bs, be);
    t->rules++;
    t->decls += decls;

    char *norm = normalize(ps, (size_t)(pe - ps));
    if (pool_take(norm)) {
        t->lost_rules++;
        t->lost_decls += decls;
    }
    free(norm);
}

static void scan_block(const char *s, const char *e, struct totals *t,
                        void (*on_rule)(const char *ps, const char *pe,
                                        struct totals *t, const char *bs,
                                        const char *be))
{
    const char *p = s;
    const char *prelude_start = s;
    int in_str = 0;
    char strch = 0;

    while (p < e) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < e) { p += 2; continue; }
            if (c == strch) in_str = 0;
            p++;
            continue;
        }
        if (c == '"' || c == '\'') { in_str = 1; strch = c; p++; continue; }
        if (c == '/' && p + 1 < e && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            p += 2;
            continue;
        }
        if (c == ';' && is_atrule(prelude_start, p)) {
            /* @charset/@import/@namespace -- no block at all. */
            p++;
            prelude_start = p;
            continue;
        }
        if (c == '{') {
            const char *prelude_end = p;
            int depth = 1;
            const char *body_start = p + 1;
            const char *q = p + 1;
            int qin_str = 0;
            char qstrch = 0;

            while (q < e && depth > 0) {
                char qc = *q;
                if (qin_str) {
                    if (qc == '\\' && q + 1 < e) { q += 2; continue; }
                    if (qc == qstrch) qin_str = 0;
                    q++;
                    continue;
                }
                if (qc == '"' || qc == '\'') {
                    qin_str = 1; qstrch = qc; q++; continue;
                }
                if (qc == '/' && q + 1 < e && q[1] == '*') {
                    q += 2;
                    while (q + 1 < e && !(q[0] == '*' && q[1] == '/')) q++;
                    q += 2;
                    continue;
                }
                if (qc == '{') depth++;
                else if (qc == '}') depth--;
                q++;
            }
            const char *body_end = (depth == 0) ? q - 1 : q;

            if (is_atrule(prelude_start, prelude_end)) {
                if (atrule_is_group(prelude_start, prelude_end))
                    scan_block(body_start, body_end, t, on_rule);
                /* else: opaque (@font-face/@page/@keyframes/@property/...),
                 * not a rule, do not recurse -- its body is declarations or
                 * percentage-keyed keyframe blocks, not selectors. */
            } else {
                on_rule(prelude_start, prelude_end, t, body_start, body_end);
            }

            p = (depth == 0) ? q : e;
            prelude_start = p;
            continue;
        }
        p++;
    }
}

/* ---- corpus walking ---------------------------------------------------- */

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static long g_unmatched_total;

static void process_source(const char *data, size_t len, struct totals *t)
{
    collect_real_drops(data, len);
    scan_block(data, data + len, t, on_plain_rule);
    /* Self-check: every real drop report should have matched some rule the
     * scanner enumerated. A leftover means the two disagree about where a
     * rule boundary is (or the report's text normalises differently from
     * the source slice) -- counted rather than silently absorbed into
     * "lost", because THIS number, not lost_rules, is what tells us to
     * distrust the measurement instead of the engine. */
    if (getenv("PROBE_DEBUG_UNMATCHED") && g_drop_n > 0) {
        for (int i = 0; i < g_drop_n; i++)
            fprintf(stderr, "UNMATCHED: [%s]\n", g_drop_texts[i]);
    }
    g_unmatched_total += g_drop_n;
    free_drop_pool();
}

/* Pull every <style>...</style> body out of an HTML file. Deliberately not
 * an HTML parser: this corpus's index.html files are captures, not
 * adversarial input, and a <style> tag inside a CDATA/comment/string would
 * be a pathological fixture nobody committed here. */
static void process_html_inline(const char *data, size_t len, struct totals *t)
{
    const char *p = data, *e = data + len;
    while (p < e) {
        const char *open = strcasestr_local(p, e, "<style");
        if (!open) break;
        const char *tagend = memchr(open, '>', (size_t)(e - open));
        if (!tagend) break;
        const char *body = tagend + 1;
        const char *close = strcasestr_local(body, e, "</style");
        if (!close) break;
        process_source(body, (size_t)(close - body), t);
        p = close + 8;
    }
}

/* strcasestr is a BSD/GNU extension and not guaranteed in a freestanding
 * -nostdinc build elsewhere in this tree; this probe is host-only, but write
 * a tiny bounded version rather than depend on it being declared under
 * whatever _GNU_SOURCE/_DARWIN_C_SOURCE combination the host TU picks up. */
static const char *strcasestr_local(const char *s, const char *e,
                                     const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = s; p + nlen <= e; p++) {
        size_t i = 0;
        for (; i < nlen; i++) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == nlen) return p;
    }
    return NULL;
}

static void walk_dir(const char *dir, struct totals *t, int *nsheets,
                      int *npages)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "cannot open %s\n", dir); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_dir(path, t, nsheets, npages);
            continue;
        }
        size_t n = strlen(ent->d_name);
        size_t len = 0;
        char *data;
        if (n > 4 && strcmp(ent->d_name + n - 4, ".css") == 0) {
            data = slurp(path, &len);
            if (!data) continue;
            process_source(data, len, t);
            (*nsheets)++;
            free(data);
        } else if (n > 5 && strcmp(ent->d_name + n - 5, ".html") == 0) {
            data = slurp(path, &len);
            if (!data) continue;
            process_html_inline(data, len, t);
            (*npages)++;
            free(data);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "tests/fixtures/cssweb";
    struct totals t = {0};
    int nsheets = 0, npages = 0;

    css__parse_selector_drop_report = on_sel_drop;

    walk_dir(root, &t, &nsheets, &npages);

    printf("css-selector-recovery-probe: %s\n", root);
    printf("  pages (inline <style> scanned): %d\n", npages);
    printf("  external sheets scanned:        %d\n", nsheets);
    printf("  top-level rules seen:            %ld\n", t.rules);
    printf("  declarations seen:               %ld\n", t.decls);
    printf("  rules with an UNPARSEABLE selector under THIS build: %ld\n",
           t.lost_rules);
    printf("  declarations inside those rules:                    %ld\n",
           t.lost_decls);
    if (t.rules > 0)
        printf("  rule loss: %.2f%%   decl loss: %.2f%%\n",
               100.0 * (double)t.lost_rules / (double)t.rules,
               t.decls ? 100.0 * (double)t.lost_decls / (double)t.decls : 0.0);
    printf("  self-check -- real drop reports the scanner could NOT match to "
           "a rule: %ld%s\n", g_unmatched_total,
           g_unmatched_total ? "  (distrust the number above)" : "  (clean)");

    return 0;
}
