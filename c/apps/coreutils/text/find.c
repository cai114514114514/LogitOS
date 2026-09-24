/* find PATH... [EXPRESSION]
 *
 * Primaries: -name PAT -iname PAT -path PAT -type f|d|l -newer FILE
 * -mindepth N -maxdepth N -empty -print -prune -exec CMD {} ;
 * Operators: ! / -not, -a / -and (also implicit), -o / -or, ( ).
 * With no action the whole expression is followed by -print, as POSIX says.
 * Patterns are fnmatch(3). Symbolic links are not followed. Directory
 * entries are visited in NAME ORDER, not the filesystem's -- LogitFS lists in
 * insertion order and a host filesystem in its own, and a find whose output
 * depends on that is a find two machines disagree about. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { E_TRUE, E_NAME, E_INAME, E_PATH, E_TYPE, E_NEWER, E_EMPTY, E_PRINT, E_PRUNE, E_EXEC,
       E_NOT, E_AND, E_OR };
struct expr { int k; const char *s; char t; long long mtime; char **argv; int argc; struct expr *a, *b; };

static char **av; static int ac, ap;
static int mindepth = 0, maxdepth = -1, has_action, pruned, status;

static struct expr *mk(int k) { struct expr *e = calloc(1, sizeof *e); if (!e) exit(1); e->k = k; return e; }
static int is(const char *s) { return ap < ac && !strcmp(av[ap], s); }

static struct expr *p_or(void);
static struct expr *p_prim(void)
{
    if (ap >= ac) { fputs("find: expected an expression\n", stderr); exit(1); }
    const char *t = av[ap++];
    struct expr *e;
    if (!strcmp(t, "(")) { e = p_or(); if (!is(")")) { fputs("find: missing ')'\n", stderr); exit(1); } ap++; return e; }
    if (!strcmp(t, "!") || !strcmp(t, "-not")) { e = mk(E_NOT); e->a = p_prim(); return e; }
    if (!strcmp(t, "-print")) { has_action = 1; return mk(E_PRINT); }
    if (!strcmp(t, "-prune")) return mk(E_PRUNE);
    if (!strcmp(t, "-empty")) return mk(E_EMPTY);
    if (!strcmp(t, "-exec")) {
        has_action = 1;
        e = mk(E_EXEC); e->argv = av + ap;
        while (ap < ac && strcmp(av[ap], ";")) { ap++; e->argc++; }
        if (ap >= ac) { fputs("find: missing argument to `-exec'\n", stderr); exit(1); }
        ap++;
        return e;
    }
    if (ap >= ac) { fprintf(stderr, "find: missing argument to `%s'\n", t); exit(1); }
    const char *v = av[ap++];
    if (!strcmp(t, "-name")) { e = mk(E_NAME); e->s = v; return e; }
    if (!strcmp(t, "-iname")) { e = mk(E_INAME); e->s = v; return e; }
    if (!strcmp(t, "-path")) { e = mk(E_PATH); e->s = v; return e; }
    if (!strcmp(t, "-type")) { e = mk(E_TYPE); e->t = v[0]; return e; }
    if (!strcmp(t, "-maxdepth")) { maxdepth = atoi(v); return mk(E_TRUE); }
    if (!strcmp(t, "-mindepth")) { mindepth = atoi(v); return mk(E_TRUE); }
    if (!strcmp(t, "-newer")) {
        struct stat st;
        if (stat(v, &st) < 0) { fprintf(stderr, "find: '%s': No such file or directory\n", v); exit(1); }
        e = mk(E_NEWER); e->mtime = (long long)st.st_mtime; return e;
    }
    fprintf(stderr, "find: unknown predicate `%s'\n", t);
    exit(1);
}
static struct expr *p_and(void)
{
    struct expr *l = p_prim();
    while (ap < ac && !is("-o") && !is("-or") && !is(")")) {
        if (is("-a") || is("-and")) ap++;
        struct expr *e = mk(E_AND); e->a = l; e->b = p_prim(); l = e;
    }
    return l;
}
static struct expr *p_or(void)
{
    struct expr *l = p_and();
    while (is("-o") || is("-or")) {
        ap++;
        struct expr *e = mk(E_OR); e->a = l; e->b = p_and(); l = e;
    }
    return l;
}

static int lcmatch(const char *p, const char *s)
{
    char a[512], b[512];
    int i;
    for (i = 0; p[i] && i < 511; i++) a[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]); a[i] = 0;
    for (i = 0; s[i] && i < 511; i++) b[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]); b[i] = 0;
    return !fnmatch(a, b, 0);
}

static int run_exec(struct expr *e, const char *path)
{
    char **argv = calloc((size_t)e->argc + 1, sizeof *argv);
    for (int i = 0; i < e->argc; i++) argv[i] = !strcmp(e->argv[i], "{}") ? (char *)path : e->argv[i];
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { execvp(argv[0], argv); fprintf(stderr, "find: %s: not found\n", argv[0]); _exit(127); }
    int st = 1;
    if (pid > 0) waitpid(pid, &st, 0);
    free(argv);
    return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int eval(struct expr *e, const char *path, const char *name, const struct stat *st)
{
    switch (e->k) {
    case E_TRUE:  return 1;
    case E_NAME:  return !fnmatch(e->s, name, 0);
    case E_INAME: return lcmatch(e->s, name);
    case E_PATH:  return !fnmatch(e->s, path, 0);
    case E_TYPE:
        return (e->t == 'f' && S_ISREG(st->st_mode)) || (e->t == 'd' && S_ISDIR(st->st_mode)) ||
               (e->t == 'l' && S_ISLNK(st->st_mode));
    case E_NEWER: return (long long)st->st_mtime > e->mtime;
    case E_EMPTY:
        if (S_ISREG(st->st_mode)) return st->st_size == 0;
        if (S_ISDIR(st->st_mode)) {
            DIR *d = opendir(path); int n = 0; struct dirent *de;
            if (!d) return 0;
            while ((de = readdir(d))) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { n++; break; }
            closedir(d);
            return n == 0;
        }
        return 0;
    case E_PRINT: puts(path); return 1;
    case E_PRUNE: pruned = 1; return 1;
    case E_EXEC:  return run_exec(e, path);
    case E_NOT:   return !eval(e->a, path, name, st);
    case E_AND:   return eval(e->a, path, name, st) && eval(e->b, path, name, st);
    case E_OR:    return eval(e->a, path, name, st) || eval(e->b, path, name, st);
    }
    return 0;
}

static struct expr *top;

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(const char *path, const char *name, int depth)
{
    struct stat st;
    if (lstat(path, &st) < 0) { fprintf(stderr, "find: '%s': No such file or directory\n", path); status = 1; return; }
    pruned = 0;
    if (depth >= mindepth) {
        int r = eval(top, path, name, &st);
        if (r && !has_action) puts(path);
    }
#ifdef TEXTUTIL_NEGCTL
    if (!S_ISDIR(st.st_mode) || pruned || (maxdepth >= 0 && depth > maxdepth)) return;   /* THE CONTROL: off by one */
#else
    if (!S_ISDIR(st.st_mode) || pruned || (maxdepth >= 0 && depth >= maxdepth)) return;
#endif
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "find: '%s': Permission denied\n", path); status = 1; return; }
    char **names = 0; int n = 0; struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        names = realloc(names, sizeof *names * (size_t)(n + 1));
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmpstr);
    for (int i = 0; i < n; i++) {
        char sub[4096];
        size_t l = strlen(path);
        snprintf(sub, sizeof sub, "%s%s%s", path, (l && path[l - 1] == '/') ? "" : "/", names[i]);
        walk(sub, names[i], depth + 1);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv)
{
    int i = 1;
    while (i < argc && argv[i][0] != '-' && strcmp(argv[i], "(") && strcmp(argv[i], "!")) i++;
    int npaths = i - 1;
    av = argv; ac = argc; ap = i;
    top = ap < ac ? p_or() : mk(E_TRUE);
    if (ap < ac) { fprintf(stderr, "find: unexpected `%s'\n", av[ap]); return 1; }
    if (!npaths) walk(".", ".", 0);
    for (int k = 1; k <= npaths; k++) {
        const char *base = strrchr(argv[k], '/');
        walk(argv[k], base && base[1] ? base + 1 : argv[k], 0);
    }
    return status;
}
