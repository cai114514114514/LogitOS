/* <glob.h>: filename expansion, component by component, over the real
 * <dirent.h> (SYS_DIR_COUNT/SYS_DIR_NAME) and the real <fnmatch.h> above. No
 * brace expansion (GLOB_BRACE is refused, not silently dropped -- a caller
 * that asked for {a,b} and got only literal-{a,b} handling back would be
 * silently wrong, which is worse than an honest GLOB_NOMATCH) and no
 * GLOB_ALTDIRFUNC. Tilde is real but trivial: there is one user (see
 * <pwd.h>), so "~" and "~root" both expand to "/". */
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

struct strvec { char **v; size_t n, cap; };

static int sv_push(struct strvec *sv, char *s)
{
    if (sv->n == sv->cap) {
        size_t nc = sv->cap ? sv->cap * 2 : 16;
        char **nv = realloc(sv->v, nc * sizeof *nv);
        if (!nv) return -1;
        sv->v = nv; sv->cap = nc;
    }
    sv->v[sv->n++] = s;
    return 0;
}
static void sv_free(struct strvec *sv) { for (size_t i = 0; i < sv->n; i++) free(sv->v[i]); free(sv->v); }

static int has_wild(const char *s, int noescape)
{
    for (; *s; s++) {
        if (!noescape && *s == '\\' && s[1]) { s++; continue; }
        if (*s == '*' || *s == '?' || *s == '[') return 1;
    }
    return 0;
}

static char *join(const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    int need_slash = (dl > 0 && dir[dl - 1] != '/');
    char *r = malloc(dl + (size_t)need_slash + nl + 1);
    if (!r) return NULL;
    memcpy(r, dir, dl);
    size_t p = dl;
    if (need_slash) r[p++] = '/';
    memcpy(r + p, name, nl + 1);
    return r;
}

/* "is this a directory": answered directly by SYS_DIR_COUNT (same primitive
 * <dirent.h>'s opendir() uses); defined at the bottom of this file. */
static int sys_is_dir_hook(const char *path);

/* Expand one path component (which may contain wildcards) against every
 * directory currently in `cur`, producing the next generation in `out`. */
static int expand_component(struct strvec *cur, const char *comp, int flags,
                             int (*errfunc)(const char *, int), struct strvec *out)
{
    int fold = 0;
    int fnflags = FNM_NOESCAPE * ((flags & GLOB_NOESCAPE) != 0);
    if (!(flags & GLOB_PERIOD)) fnflags |= FNM_PERIOD;
    (void)fold;

    int wild = has_wild(comp, (flags & GLOB_NOESCAPE) != 0);

    for (size_t i = 0; i < cur->n; i++) {
        const char *base = cur->v[i];
        if (!wild) {
            char *p = join(base, comp);
            if (!p) return GLOB_NOSPACE;
            if (sv_push(out, p) < 0) { free(p); return GLOB_NOSPACE; }
            continue;
        }
        DIR *d = opendir(*base ? base : ".");
        if (!d) {
            if (errfunc && errfunc(base, errno)) return GLOB_ABORTED;
            if (flags & GLOB_ERR) return GLOB_ABORTED;
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (fnmatch(comp, e->d_name, fnflags) != 0) continue;
            char *p = join(base, e->d_name);
            if (!p) { closedir(d); return GLOB_NOSPACE; }
            if (sv_push(out, p) < 0) { free(p); closedir(d); return GLOB_NOSPACE; }
        }
        closedir(d);
    }
    return 0;
}

static int cmp_str(const void *a, const void *b)
{ return strcmp(*(char *const *)a, *(char *const *)b); }

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob)
{
    if (!pattern || !pglob) { errno = EINVAL; return GLOB_ABORTED; }
    if (flags & GLOB_BRACE) return GLOB_NOMATCH;   /* refused, not faked -- see header note */

    char pbuf[1024];
    const char *pat = pattern;
    if ((flags & GLOB_TILDE) && pattern[0] == '~') {
        /* one user: "~" and "~root" both mean "/" (see <pwd.h>); anything
         * else after ~ up to the next '/' is an unknown user -> GLOB_NOMATCH */
        const char *slash = pattern + 1;
        while (*slash && *slash != '/') slash++;
        size_t namelen = (size_t)(slash - (pattern + 1));
        if (namelen == 0 || (namelen == 4 && strncmp(pattern + 1, "root", 4) == 0)) {
            size_t restlen = strlen(slash);
            if (restlen + 2 > sizeof pbuf) return GLOB_NOMATCH;
            pbuf[0] = '/';
            memcpy(pbuf + 1, slash, restlen + 1);
            pat = pbuf;
        } else {
            return GLOB_NOMATCH;
        }
    }

    struct strvec cur = {0}, next = {0};
    char *start = strdup(pat[0] == '/' ? "/" : "");
    if (!start || sv_push(&cur, start) < 0) { free(start); return GLOB_NOSPACE; }

    const char *p = pat + (pat[0] == '/' ? 1 : 0);
    int rc = 0;
    while (*p) {
        char comp[256];
        size_t ci = 0;
        while (*p && *p != '/' && ci < sizeof comp - 1) {
            if (*p == '\\' && !(flags & GLOB_NOESCAPE) && p[1]) { comp[ci++] = *p++; if (ci < sizeof comp - 1) comp[ci++] = *p++; continue; }
            comp[ci++] = *p++;
        }
        comp[ci] = 0;
        while (*p == '/') p++;

        next.n = 0;
        rc = expand_component(&cur, comp, flags, errfunc, &next);
        sv_free(&cur);
        cur = next;
        memset(&next, 0, sizeof next);
        if (rc) { sv_free(&cur); return rc; }
        if (cur.n == 0) break;
    }

    if (flags & GLOB_MARK) {
        for (size_t i = 0; i < cur.n; i++) {
            if (sys_is_dir_hook(cur.v[i])) {
                size_t l = strlen(cur.v[i]);
                char *nv = realloc(cur.v[i], l + 2);
                if (nv) { nv[l] = '/'; nv[l + 1] = 0; cur.v[i] = nv; }
            }
        }
    }
    if (!(flags & GLOB_NOSORT) && cur.n > 1)
        qsort(cur.v, cur.n, sizeof *cur.v, cmp_str);

    if (cur.n == 0) {
        sv_free(&cur);
        if (flags & GLOB_NOCHECK) {
            char *lit = strdup(pattern);
            if (!lit) return GLOB_NOSPACE;
            struct strvec one = {0};
            if (sv_push(&one, lit) < 0) { free(lit); return GLOB_NOSPACE; }
            cur = one;
        } else {
            pglob->gl_pathc = 0;
            pglob->gl_pathv = NULL;
            return GLOB_NOMATCH;
        }
    }

    size_t offs = (flags & GLOB_DOOFFS) ? pglob->gl_offs : 0;
    size_t base = (flags & GLOB_APPEND) ? pglob->gl_pathc : 0;
    size_t total = base + cur.n;
    char **nv = realloc((flags & GLOB_APPEND) ? pglob->gl_pathv : NULL, (offs + total + 1) * sizeof(char *));
    if (!nv) { sv_free(&cur); return GLOB_NOSPACE; }
    if (!(flags & GLOB_APPEND)) for (size_t i = 0; i < offs; i++) nv[i] = NULL;
    for (size_t i = 0; i < cur.n; i++) nv[offs + base + i] = cur.v[i];
    nv[offs + total] = NULL;
    free(cur.v);

    pglob->gl_pathv = nv;
    pglob->gl_pathc = total;
    pglob->gl_offs = offs;   /* real offset used, so globfree() can find the strings */
    return 0;
}

void globfree(glob_t *pglob)
{
    if (!pglob || !pglob->gl_pathv) return;
    for (size_t i = 0; i < pglob->gl_pathc; i++) free(pglob->gl_pathv[pglob->gl_offs + i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}

/* opendir()/SYS_DIR_COUNT already answer "is this a directory"; this just
 * names that check for GLOB_MARK without a second header dependency. */
#include "logit_abi.h"
static long glob_sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }
static int sys_is_dir_hook(const char *path) { return glob_sys(SYS_DIR_COUNT, (long)path, 0, 0) >= 0; }
