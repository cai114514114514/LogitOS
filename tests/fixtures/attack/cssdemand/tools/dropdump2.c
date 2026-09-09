/* dropdump2.c -- dropdump with the browser's OWN var() pre-pass in front of
 * LibCSS (c/apps/browser/css_vars.c, unmodified, linked in), over the sources
 * of one site CONCATENATED in one buffer the way css_engine.c's collect_style
 * + collect_css_links feed css_apply (one author_css buffer per document).
 * Sentinel rules `#__logit_src_N{z-index:N}` are inserted between sources so
 * DECL lines can still be attributed to a source by the z-index report.
 * css_media_matches() is STUBBED here (the real one needs a LibCSS select
 * context and the whole engine): width/height queries are evaluated against
 * 1280x800, `prefers-color-scheme: dark` and `print` answer no, everything
 * else answers yes. That only steers WHICH @media block's `--x` wins in the
 * pre-pass; the declarations LibCSS then sees are the same either way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libcss/libcss.h>

extern void (*css__parse_selector_drop_report)(const char *text, size_t len);
extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);
int css_expand_vars(const char *in, int inlen, char *out, int outmax);
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }

static long g_num(const char *q, int len, const char *key)
{
    for (int i = 0; i + (int)strlen(key) < len; i++)
        if (!strncasecmp(q + i, key, strlen(key))) {
            const char *p = q + i + strlen(key);
            while (p < q + len && (*p == ' ' || *p == ':' )) p++;
            return atol(p);
        }
    return -1;
}
int css_media_matches(const char *query, int len)
{
    long v;
    if (!query || len <= 0) return 1;
    if ((v = g_num(query, len, "min-width")) >= 0 && v > 1280) return 0;
    if ((v = g_num(query, len, "max-width")) >= 0 && v < 1280) return 0;
    if ((v = g_num(query, len, "min-height")) >= 0 && v > 800) return 0;
    if ((v = g_num(query, len, "max-height")) >= 0 && v < 800) return 0;
    for (int i = 0; i + 5 <= len; i++) {
        if (!strncasecmp(query + i, "print", 5) && (i == 0 || query[i-1] == ' ')) return 0;
        if (!strncasecmp(query + i, "dark", 4)) return 0;
    }
    return 1;
}
static void on_sel(const char *t, size_t n) { printf("SELDROP\t%.*s\n", (int)n, t); }
static void on_decl(const char *name, size_t n, int reason) { printf("DECL\t%.*s\t%d\n", (int)n, name, reason); }
static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{ (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK; }
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); *len = fread(b, 1, (size_t)n, f); b[*len] = 0; fclose(f); return b;
}
static const char *ci_find(const char *s, const char *e, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = s; p + nl <= e; p++) {
        size_t i = 0;
        for (; i < nl; i++) { char a = p[i], b = needle[i]; if (a >= 'A' && a <= 'Z') a += 32; if (a != b) break; }
        if (i == nl) return p;
    }
    return NULL;
}
static char *g_buf; static size_t g_len, g_cap; static int g_nsrc;
static void app(const char *s, size_t n)
{
    if (g_len + n + 1 > g_cap) { g_cap = (g_len + n + 1) * 2; g_buf = realloc(g_buf, g_cap); }
    memcpy(g_buf + g_len, s, n); g_len += n; g_buf[g_len] = 0;
}
static void add_source(const char *name, int idx, const char *d, size_t n)
{
    char m[256];
    snprintf(m, sizeof m, "\n#__logit_src_%d{z-index:%d}\n", g_nsrc, g_nsrc);
    printf("SRCMAP\t%d\t%s\t%d\n", g_nsrc, name, idx);
    g_nsrc++;
    app(m, strlen(m)); app(d, n);
}
int main(int argc, char **argv)
{
    int expand = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--no-vars")) { expand = 0; continue; }
        size_t n = strlen(argv[a]), len; char *d;
        if (n > 4 && !strcmp(argv[a] + n - 4, ".css")) { d = slurp(argv[a], &len); add_source(argv[a], 0, d, len); free(d); }
        else if (n > 5 && !strcmp(argv[a] + n - 5, ".html")) {
            d = slurp(argv[a], &len); const char *p = d, *e = d + len; int k = 0;
            while (p < e) {
                const char *open = ci_find(p, e, "<style"); if (!open) break;
                const char *tagend = memchr(open, '>', (size_t)(e - open)); if (!tagend) break;
                const char *body = tagend + 1; const char *close = ci_find(body, e, "</style"); if (!close) break;
                add_source(argv[a], k++, body, (size_t)(close - body)); p = close + 8;
            }
            free(d);
        }
    }
    char m[64]; snprintf(m, sizeof m, "\n#__logit_src_%d{z-index:%d}\n", g_nsrc, g_nsrc); app(m, strlen(m));
    const char *src = g_buf; size_t slen = g_len; char *exp = NULL;
    if (expand) {
        size_t cap = g_len * 4 + 65536; exp = malloc(cap);
        int el = css_expand_vars(g_buf, (int)g_len, exp, (int)cap);
        if (el < 0) { fprintf(stderr, "expand failed\n"); return 1; }
        src = exp; slen = (size_t)el;
        printf("EXPANDED\t%zu\t%zu\n", g_len, slen);
    }
    css__parse_selector_drop_report = on_sel; css__parse_drop_report = on_decl;
    css_stylesheet_params p; css_stylesheet *s = NULL;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1; p.level = CSS_LEVEL_DEFAULT; p.charset = "UTF-8";
    p.url = "http://logit/probe.css"; p.allow_quirks = false; p.resolve = resolve_url;
    if (css_stylesheet_create(&p, &s) != CSS_OK) return 1;
    css_stylesheet_append_data(s, (const uint8_t *)src, slen);
    css_stylesheet_data_done(s); css_stylesheet_destroy(s);
    return 0;
}
