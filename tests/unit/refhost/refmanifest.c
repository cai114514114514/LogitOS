/* refmanifest -- see refmanifest.h. Text only; nothing here renders anything.
 *
 * The parsing is deliberately NOT done with our own DOM: this code has to be
 * able to say "that file is a reftest and it points there" about a page the
 * engine under test may fail to parse at all. A scanner that shares the
 * tokenizer with the thing being measured would hide exactly the failures worth
 * finding -- a test whose <link rel=match> is dropped by a parser bug would
 * silently become "not a reftest" and vanish from the denominator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "refmanifest.h"

static int ci_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

/* Read the value of attribute `name` out of a tag body [p,end). Handles single
 * quotes, double quotes and the unquoted form, all three of which the corpus
 * uses. Returns the length written, or -1 if the attribute is absent. */
static int tag_attr(const char *p, const char *end, const char *name,
                    char *out, int outmax)
{
    int nl = (int)strlen(name);
    for (const char *q = p; q + nl < end; q++) {
        if (!ci_eq(q, name, nl)) continue;
        /* must be at a token boundary, or href matches "xlink:href" */
        if (q > p && (isalnum((unsigned char)q[-1]) || q[-1] == '-' || q[-1] == ':')) continue;
        const char *r = q + nl;
        while (r < end && isspace((unsigned char)*r)) r++;
        if (r >= end || *r != '=') continue;
        r++;
        while (r < end && isspace((unsigned char)*r)) r++;
        if (r >= end) return -1;
        char quote = 0;
        if (*r == '"' || *r == '\'') { quote = *r; r++; }
        const char *s = r;
        while (r < end && (quote ? *r != quote : (!isspace((unsigned char)*r) && *r != '>'))) r++;
        int n = (int)(r - s);
        if (n >= outmax) n = outmax - 1;
        memcpy(out, s, (size_t)n); out[n] = 0;
        return n;
    }
    return -1;
}

int rm_resolve(const char *base, const char *href, char *out, int outmax)
{
    if (!href || !*href) return -1;
    if (href[0] == '#') return -1;                       /* fragment only */
    if (strstr(href, "://")) return -1;                  /* external */
    if (!strncmp(href, "data:", 5) || !strncmp(href, "javascript:", 11)) return -1;
    if (!strncmp(href, "//", 2)) return -1;              /* protocol-relative */

    char buf[RM_PATHMAX * 2];
    if (href[0] == '/') {
        snprintf(buf, sizeof buf, "%s", href + 1);       /* root-absolute */
    } else {
        const char *slash = strrchr(base, '/');
        int dlen = slash ? (int)(slash - base + 1) : 0;
        snprintf(buf, sizeof buf, "%.*s%s", dlen, base, href);
    }
    /* strip a query or fragment -- a reference is a file, and `ref.html?x` and
     * `ref.html` are the same bytes on disk */
    char *cut = strpbrk(buf, "?#"); if (cut) *cut = 0;

    /* normalise . and .. segment-wise */
    char *segs[128]; int ns = 0;
    for (char *t = strtok(buf, "/"); t; t = strtok(0, "/")) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (ns) ns--; continue; }
        if (ns < 128) segs[ns++] = t;
    }
    int o = 0;
    for (int i = 0; i < ns; i++)
        o += snprintf(out + o, (size_t)(outmax - o), "%s%s", i ? "/" : "", segs[i]);
    return (o > 0 && o < outmax) ? 0 : -1;
}

/* "0-2" -> 2 ; "3" -> 3. WPT writes a fuzzy bound as a range whose upper end is
 * the permitted maximum; the lower end is a floor asserting the difference is
 * real, which a pass/fail harness has no use for. */
static long range_max(const char *s)
{
    const char *dash = strchr(s, '-');
    return atol(dash ? dash + 1 : s);
}

/* Apply one `fuzzy` content string to the reference list.
 *
 * Two forms:
 *   "maxDiff;totalPixels"                      applies to every reference
 *   "path/to/ref.html:maxDiff;totalPixels"     applies to that one
 * and the scoped form may also name the pair "test.html==ref.html". Anything
 * before the last ':' that contains a '/' or ".htm" is treated as a scope. */
static void apply_fuzzy(struct rm_test *t, const char *content, const char *rel)
{
    const char *body = content;
    char scope[RM_PATHMAX] = "";

    /* Find a scope prefix: the last ':' that is followed by a digit or space,
     * and preceded by something that looks like a filename. */
    const char *colon = strrchr(content, ':');
    if (colon && colon > content) {
        const char *v = colon + 1;
        while (*v == ' ') v++;
        if (isdigit((unsigned char)*v)) {
            int n = (int)(colon - content);
            if (n > 0 && n < RM_PATHMAX) {
                memcpy(scope, content, (size_t)n); scope[n] = 0;
                /* "test==ref" -> keep the ref half */
                char *eq = strstr(scope, "==");
                if (eq) memmove(scope, eq + 2, strlen(eq + 2) + 1);
                body = v;
            }
        }
    }
    while (*body == ' ') body++;
    const char *semi = strchr(body, ';');
    long md = range_max(body);
    long mp = semi ? range_max(semi + 1) : 0;

    char resolved[RM_PATHMAX] = "";
    if (scope[0]) rm_resolve(rel, scope, resolved, sizeof resolved);

    for (int i = 0; i < t->nrefs; i++) {
        if (resolved[0] && strcmp(resolved, t->refs[i].path)) continue;
        t->refs[i].fuzz_maxdiff = (int)md;
        t->refs[i].fuzz_maxpixels = mp;
    }
}

static int flag_bits(const char *content)
{
    static const struct { const char *name; int bit; } tab[] = {
        { "ahem", RM_F_AHEM }, { "interact", RM_F_INTERACT },
        { "animated", RM_F_ANIMATED }, { "paged", RM_F_PAGED },
        { "http", RM_F_HTTP }, { "speech-only", RM_F_SPEECH },
        { "userstyle", RM_F_USERSTYLE }, { "asis", RM_F_ASIS },
        { "invalid", RM_F_INVALID }, { "may", RM_F_MAY }, { "should", RM_F_SHOULD },
    };
    int bits = 0;
    for (unsigned k = 0; k < sizeof tab / sizeof tab[0]; k++) {
        const char *n = tab[k].name; int nl = (int)strlen(n);
        for (const char *q = content; *q; q++) {
            if (!ci_eq(q, n, nl)) continue;
            char before = q == content ? ' ' : q[-1];
            char after = q[nl];
            if ((before == ' ' || before == ',' || before == '\t') &&
                (after == 0 || after == ' ' || after == ',' || after == '\t'))
                { bits |= tab[k].bit; break; }
        }
    }
    return bits;
}

int rm_parse(const char *src, long len, const char *rel, struct rm_test *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->path, sizeof out->path, "%s", rel);
    out->tentative = strstr(rel, ".tentative.") != 0;
    const char *dot = strrchr(rel, '.');
    out->xhtml = dot && (!strcmp(dot, ".xht") || !strcmp(dot, ".xhtml"));

    /* Only the head matters, but the corpus is not disciplined about that and
     * some files put the link late. Scan the whole file; it is cheap. */
    for (long i = 0; i < len; i++) {
        if (src[i] != '<') continue;
        const char *p = src + i + 1;
        const char *end = memchr(p, '>', (size_t)(len - i - 1));
        if (!end) break;

        if (ci_eq(p, "link", 4) && !isalnum((unsigned char)p[4])) {
            char rel_v[128], href[RM_PATHMAX];
            if (tag_attr(p, end, "rel", rel_v, sizeof rel_v) < 0) continue;
            int mism;
            if (ci_eq(rel_v, "match", 6)) mism = 0;
            else if (ci_eq(rel_v, "mismatch", 9)) mism = 1;
            else continue;
            if (tag_attr(p, end, "href", href, sizeof href) < 0) continue;
            if (out->nrefs >= RM_MAXREFS) continue;
            struct rm_ref *r = &out->refs[out->nrefs];
            if (rm_resolve(rel, href, r->path, sizeof r->path) != 0) continue;
            r->mismatch = mism;
            r->fuzz_maxdiff = -1; r->fuzz_maxpixels = -1;
            out->nrefs++;
        } else if (ci_eq(p, "meta", 4) && !isalnum((unsigned char)p[4])) {
            char name[64], content[512];
            if (tag_attr(p, end, "name", name, sizeof name) < 0) continue;
            if (tag_attr(p, end, "content", content, sizeof content) < 0) continue;
            if (ci_eq(name, "flags", 6)) out->flags |= flag_bits(content);
            /* fuzzy is applied AFTER the link scan below, because a `fuzzy`
             * meta may legally precede the link it scopes to. */
        }
        i = end - src;
    }

    /* second pass for fuzzy, now that the reference list exists */
    for (long i = 0; i < len; i++) {
        if (src[i] != '<') continue;
        const char *p = src + i + 1;
        const char *end = memchr(p, '>', (size_t)(len - i - 1));
        if (!end) break;
        if (ci_eq(p, "meta", 4) && !isalnum((unsigned char)p[4])) {
            char name[64], content[512];
            if (tag_attr(p, end, "name", name, sizeof name) >= 0 &&
                ci_eq(name, "fuzzy", 6) &&
                tag_attr(p, end, "content", content, sizeof content) >= 0)
                apply_fuzzy(out, content, rel);
        }
        i = end - src;
    }
    return out->nrefs;
}

int rm_parse_file(const char *root, const char *rel, struct rm_test *out)
{
    char full[RM_PATHMAX * 2];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    FILE *f = fopen(full, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 8 * 1024 * 1024) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    int rc = rm_parse(buf, n, rel, out);
    free(buf);
    return rc;
}

int rm_is_reference(const char *rel)
{
    if (strstr(rel, "/reference/") || strstr(rel, "/references/")) return 1;
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    const char *dot = strrchr(base, '.');
    if (!dot) return 0;
    int n = (int)(dot - base);
    if (n >= 4 && !strncmp(dot - 4, "-ref", 4)) return 1;
    if (n >= 7 && !strncmp(dot - 7, "-notref", 7)) return 1;
    return 0;
}

static int ext_ok(const char *rel)
{
    const char *dot = strrchr(rel, '.');
    if (!dot) return 0;
    return !strcmp(dot, ".html") || !strcmp(dot, ".htm") ||
           !strcmp(dot, ".xht")  || !strcmp(dot, ".xhtml");
}

static int walk_rec(const char *root, char *rel, int rlen, const char *filter,
                    void (*cb)(const char *, void *), void *ud)
{
    char full[RM_PATHMAX * 2];
    snprintf(full, sizeof full, "%s%s%s", root, rlen ? "/" : "", rel);
    DIR *d = opendir(full);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;                /* incl. . .. .git */
        /* WPT keeps non-test machinery in these; walking them costs time and
         * produces nothing a reftest could point at. */
        if (!strcmp(e->d_name, "tools") || !strcmp(e->d_name, "resources") ||
            !strcmp(e->d_name, "common") || !strcmp(e->d_name, "conformance-checkers"))
            continue;
        int nlen = snprintf(rel + rlen, (size_t)(RM_PATHMAX - rlen), "%s%s",
                            rlen ? "/" : "", e->d_name);
        if (nlen <= 0 || rlen + nlen >= RM_PATHMAX - 1) { rel[rlen] = 0; continue; }
        char sub[RM_PATHMAX * 2];
        snprintf(sub, sizeof sub, "%s/%s", root, rel);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            count += walk_rec(root, rel, rlen + nlen, filter, cb, ud);
        } else if (ext_ok(rel) && (!filter || strstr(rel, filter))) {
            cb(rel, ud); count++;
        }
        rel[rlen] = 0;
    }
    closedir(d);
    return count;
}

int rm_walk(const char *root, const char *filter,
            void (*cb)(const char *rel, void *ud), void *ud)
{
    char rel[RM_PATHMAX]; rel[0] = 0;
    return walk_rec(root, rel, 0, filter, cb, ud);
}
