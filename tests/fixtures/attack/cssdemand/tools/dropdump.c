/* dropdump.c -- MEASUREMENT ONLY (scratch, not part of the tree).
 * Parse every source of a corpus directory through the tree's OWN vendored
 * LibCSS (build-atk-cssdemand/libcss_host.a) exactly as css_engine.c's
 * make_sheet() does (css_stylesheet_create/append_data/data_done), with the
 * two NULL-by-default report hooks the tree already carries installed, and
 * print one line per event so a script can join them to a tinycss2 census:
 *   SRC\t<path>\t<n>            a new source (external sheet, or the n-th
 *                               <style> block of an .html file)
 *   SELDROP\t<text>             a ruleset whose selector list LibCSS refused
 *                               (parse.c report_selector_drop, whitespace
 *                               collapsed, 191-byte cap)
 *   DECL\t<name>\t<reason>      parseProperty's verdict for one declaration:
 *                               0 unknown-prop, 1 bad-value, 2 trailing,
 *                               3 ACCEPTED
 * Modeled on tests/unit/css_selector_recovery_probe.c's collect_real_drops. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libcss/libcss.h>

extern void (*css__parse_selector_drop_report)(const char *text, size_t len);
extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

static void on_sel(const char *t, size_t n) { printf("SELDROP\t%.*s\n", (int)n, t); }
static void on_decl(const char *name, size_t n, int reason) { printf("DECL\t%.*s\t%d\n", (int)n, name, reason); }

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{ (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK; }

static void parse_source(const char *data, size_t len)
{
    css_stylesheet_params p; css_stylesheet *s = NULL;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT; p.charset = "UTF-8";
    p.url = "http://logit/probe.css"; p.allow_quirks = false; p.resolve = resolve_url;
    if (css_stylesheet_create(&p, &s) != CSS_OK || !s) { printf("ERR\tcreate\n"); return; }
    css_stylesheet_append_data(s, (const uint8_t *)data, len);
    css_stylesheet_data_done(s);
    css_stylesheet_destroy(s);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    *len = fread(b, 1, (size_t)n, f); b[*len] = 0; fclose(f); return b;
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

static void do_html(const char *path)
{
    size_t len; char *d = slurp(path, &len); if (!d) return;
    const char *p = d, *e = d + len; int n = 0;
    while (p < e) {
        const char *open = ci_find(p, e, "<style"); if (!open) break;
        const char *tagend = memchr(open, '>', (size_t)(e - open)); if (!tagend) break;
        const char *body = tagend + 1;
        const char *close = ci_find(body, e, "</style"); if (!close) break;
        printf("SRC\t%s\t%d\n", path, n++);
        parse_source(body, (size_t)(close - body));
        p = close + 8;
    }
    free(d);
}

static void do_css(const char *path)
{
    size_t len; char *d = slurp(path, &len); if (!d) return;
    printf("SRC\t%s\t0\n", path);
    parse_source(d, len);
    free(d);
}

int main(int argc, char **argv)
{
    css__parse_selector_drop_report = on_sel;
    css__parse_drop_report = on_decl;
    for (int a = 1; a < argc; a++) {
        size_t n = strlen(argv[a]);
        if (n > 4 && !strcmp(argv[a] + n - 4, ".css")) do_css(argv[a]);
        else if (n > 5 && !strcmp(argv[a] + n - 5, ".html")) do_html(argv[a]);
        else {
            DIR *dir = opendir(argv[a]); if (!dir) { fprintf(stderr, "cannot open %s\n", argv[a]); continue; }
            struct dirent *ent; char path[2048];
            while ((ent = readdir(dir))) {
                if (ent->d_name[0] == '.') continue;
                snprintf(path, sizeof path, "%s/%s", argv[a], ent->d_name);
                size_t m = strlen(ent->d_name);
                if (m > 4 && !strcmp(ent->d_name + m - 4, ".css")) do_css(path);
                else if (m > 5 && !strcmp(ent->d_name + m - 5, ".html")) do_html(path);
            }
            closedir(dir);
        }
    }
    return 0;
}
