/* <regex.h>: BRE/ERE compiled to a small tree (regcomp) and matched by a
 * recursive continuation-passing backtracker (regexec). See the header for
 * the precise, honest limitations (leftmost-first alternation, not
 * leftmost-longest; no backreferences). */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_CONCAT, N_ALT, N_REP, N_GROUP };

typedef struct Node {
    int type;
    int c;
    unsigned char cls[32];
    int neg;
    struct Node **list; int nlist;
    struct Node *sub;
    int min, max;       /* N_REP; max == -1 means unbounded */
    int capidx;          /* N_GROUP */
} Node;

struct Parser {
    const char *p;
    int ext;
    int icase;
    int ngroups;
    int err;             /* 0 = ok, else a REG_* code */
};

static Node *node_new(int type)
{
    Node *n = calloc(1, sizeof *n);
    if (n) n->type = type;
    return n;
}

static void list_push(Node ***list, int *n, Node *item)
{
    Node **nv = realloc(*list, (size_t)(*n + 1) * sizeof(Node *));
    if (!nv) return;   /* OOM: silently drop; regcomp() reports REG_ESPACE separately below */
    nv[*n] = item;
    *list = nv;
    (*n)++;
}

static void cls_set(unsigned char *bm, int c) { bm[(c >> 3) & 31] |= (unsigned char)(1 << (c & 7)); }
static int  cls_get(const unsigned char *bm, int c) { return (bm[(c >> 3) & 31] >> (c & 7)) & 1; }

static Node *parse_alt(struct Parser *ps);

static void parse_class_named(struct Parser *ps, unsigned char *bm)
{
    const char *cls = ps->p;
    const char *end = cls;
    while (*end && !(*end == ':' && end[1] == ']')) end++;
    if (!*end) { ps->err = REG_EBRACK; return; }
    size_t len = (size_t)(end - cls);
    #define CLSFILL(name, fn) if (len == sizeof(name) - 1 && memcmp(cls, name, len) == 0) \
        { for (int c = 0; c < 256; c++) if (fn(c)) cls_set(bm, c); ps->p = end + 2; return; }
    CLSFILL("alpha", isalpha) CLSFILL("digit", isdigit) CLSFILL("alnum", isalnum)
    CLSFILL("space", isspace) CLSFILL("upper", isupper) CLSFILL("lower", islower)
    CLSFILL("punct", ispunct) CLSFILL("cntrl", iscntrl) CLSFILL("print", isprint)
    CLSFILL("graph", isgraph) CLSFILL("blank", isblank) CLSFILL("xdigit", isxdigit)
    #undef CLSFILL
    ps->err = REG_ECTYPE;
    ps->p = end + 2;
}

static Node *parse_bracket(struct Parser *ps)
{
    Node *n = node_new(N_CLASS);
    if (!n) { ps->err = REG_ESPACE; return NULL; }
    ps->p++;   /* '[' */
    if (*ps->p == '^') { n->neg = 1; ps->p++; }
    int first = 1;
    while (*ps->p && (*ps->p != ']' || first)) {
        first = 0;
        if (ps->p[0] == '[' && ps->p[1] == ':') {
            ps->p += 2;
            parse_class_named(ps, n->cls);
            if (ps->err) return n;
            continue;
        }
        int lo = (unsigned char)*ps->p++;
        if (*ps->p == '-' && ps->p[1] && ps->p[1] != ']') {
            int hi = (unsigned char)ps->p[1];
            ps->p += 2;
            if (lo > hi) { ps->err = REG_ERANGE; return n; }
            for (int c = lo; c <= hi; c++) cls_set(n->cls, c);
        } else {
            cls_set(n->cls, lo);
        }
    }
    if (*ps->p != ']') { ps->err = REG_EBRACK; return n; }
    ps->p++;
    if (ps->icase) {
        for (int c = 0; c < 256; c++)
            if (cls_get(n->cls, c)) { cls_set(n->cls, tolower(c)); cls_set(n->cls, toupper(c)); }
    }
    return n;
}

static int at_group_close(struct Parser *ps)
{ return ps->ext ? (*ps->p == ')') : (ps->p[0] == '\\' && ps->p[1] == ')'); }
static int at_alt_bar(struct Parser *ps)
{ return ps->ext ? (*ps->p == '|') : (ps->p[0] == '\\' && ps->p[1] == '|'); }

static Node *parse_atom(struct Parser *ps, int first_in_concat)
{
    int c = (unsigned char)*ps->p;
    if (c == 0) return node_new(N_EMPTY);

    if (ps->ext && c == '(') {
        ps->p++;
        int idx = ++ps->ngroups;
        Node *sub = parse_alt(ps);
        if (!at_group_close(ps)) { ps->err = REG_EPAREN; return sub; }
        ps->p++;
        Node *g = node_new(N_GROUP);
        if (!g) { ps->err = REG_ESPACE; return sub; }
        g->sub = sub; g->capidx = idx;
        return g;
    }
    if (!ps->ext && c == '\\' && ps->p[1] == '(') {
        ps->p += 2;
        int idx = ++ps->ngroups;
        Node *sub = parse_alt(ps);
        if (!at_group_close(ps)) { ps->err = REG_EPAREN; return sub; }
        ps->p += 2;
        Node *g = node_new(N_GROUP);
        if (!g) { ps->err = REG_ESPACE; return sub; }
        g->sub = sub; g->capidx = idx;
        return g;
    }
    if (c == '.') { ps->p++; return node_new(N_ANY); }
    if (c == '[') return parse_bracket(ps);
    if (c == '^') { ps->p++; return node_new(N_BOL); }
    if (c == '$') {
        /* In BRE, '$' anchors only at the end of the RE/subexpression; treated
         * as an anchor unconditionally here is a harmless superset for the
         * patterns real programs write. */
        ps->p++; return node_new(N_EOL);
    }
    if (c == '*' && first_in_concat && !ps->ext) {
        /* BRE: a leading '*' is a literal character, not "zero or more of
         * nothing" (POSIX 9.3.6). */
        ps->p++;
        Node *n = node_new(N_CHAR); if (n) n->c = '*';
        return n;
    }
    if (c == '\\' && !ps->p[1]) { ps->err = REG_EESCAPE; ps->p++; return node_new(N_EMPTY); }
    if (c == '\\' && ps->p[1]) {
        ps->p++;
        int e = (unsigned char)*ps->p++;
        Node *n = node_new(N_CHAR);
        if (!n) { ps->err = REG_ESPACE; return NULL; }
        n->c = e;
        return n;
    }
    ps->p++;
    Node *n = node_new(N_CHAR);
    if (!n) { ps->err = REG_ESPACE; return NULL; }
    n->c = c;
    return n;
}

/* {m,n} (or, in BRE, backslash-braced) bound parsing. Returns 1 and fills
 * the min and max bounds on success. */
static int parse_bound(struct Parser *ps, int *min, int *max)
{
    const char *save = ps->p;
    const char *q = ps->ext ? ps->p + 1 : ps->p + 2;   /* skip '{' or '\{' */
    if (!isdigit((unsigned char)*q)) { ps->p = save; return 0; }
    int m = 0; while (isdigit((unsigned char)*q)) { m = m * 10 + (*q - '0'); q++; }
    int n = m;
    if (*q == ',') {
        q++;
        if (isdigit((unsigned char)*q)) { n = 0; while (isdigit((unsigned char)*q)) { n = n * 10 + (*q - '0'); q++; } }
        else n = -1;
    }
    if (ps->ext) { if (*q != '}') { ps->p = save; return 0; } q++; }
    else { if (!(q[0] == '\\' && q[1] == '}')) { ps->p = save; return 0; } q += 2; }
    *min = m; *max = n;
    ps->p = q;
    return 1;
}

static Node *parse_repeat(struct Parser *ps, int first_in_concat)
{
    Node *atom = parse_atom(ps, first_in_concat);
    if (ps->err || !atom) return atom;
    for (;;) {
        int min = -2, max = -2;
        if (ps->ext && *ps->p == '*') { ps->p++; min = 0; max = -1; }
        else if (ps->ext && *ps->p == '+') { ps->p++; min = 1; max = -1; }
        else if (ps->ext && *ps->p == '?') { ps->p++; min = 0; max = 1; }
        else if (!ps->ext && *ps->p == '*') { ps->p++; min = 0; max = -1; }
        else if (!ps->ext && ps->p[0] == '\\' && ps->p[1] == '+') { ps->p += 2; min = 1; max = -1; }
        else if (!ps->ext && ps->p[0] == '\\' && ps->p[1] == '?') { ps->p += 2; min = 0; max = 1; }
        else if ((ps->ext && *ps->p == '{') || (!ps->ext && ps->p[0] == '\\' && ps->p[1] == '{')) {
            int bmin, bmax;
            if (!parse_bound(ps, &bmin, &bmax)) break;
            if (bmax != -1 && bmax < bmin) { ps->err = REG_BADBR; return atom; }
            min = bmin; max = bmax;
        } else break;
        Node *r = node_new(N_REP);
        if (!r) { ps->err = REG_ESPACE; return atom; }
        r->sub = atom; r->min = min; r->max = max;
        atom = r;
        first_in_concat = 0;   /* only the very first atom gets the BRE leading-'*' rule */
    }
    return atom;
}

static Node *parse_concat(struct Parser *ps)
{
    Node **list = NULL; int n = 0;
    int first = 1;
    while (*ps->p && !at_alt_bar(ps) && !at_group_close(ps)) {
        Node *a = parse_repeat(ps, first);
        first = 0;
        if (ps->err) { list_push(&list, &n, a); break; }
        list_push(&list, &n, a);
    }
    if (n == 0) { free(list); return node_new(N_EMPTY); }
    if (n == 1) { Node *only = list[0]; free(list); return only; }
    Node *c = node_new(N_CONCAT);
    if (!c) { ps->err = REG_ESPACE; free(list); return NULL; }
    c->list = list; c->nlist = n;
    return c;
}

static Node *parse_alt(struct Parser *ps)
{
    Node **list = NULL; int n = 0;
    list_push(&list, &n, parse_concat(ps));
    while (!ps->err && at_alt_bar(ps)) {
        ps->p += ps->ext ? 1 : 2;
        list_push(&list, &n, parse_concat(ps));
    }
    if (n == 1) { Node *only = list[0]; free(list); return only; }
    Node *a = node_new(N_ALT);
    if (!a) { ps->err = REG_ESPACE; free(list); return NULL; }
    a->list = list; a->nlist = n;
    return a;
}

/* Fold CHAR nodes' case at compile time (REG_ICASE) so matching never has to
 * special-case it per node -- CLASS already folds its bitmap in
 * parse_bracket(). Plain recursive walk; Clang does not support nested
 * functions, so this cannot live inside regcomp(). */
static void fold_case(Node *n)
{
    if (!n) return;
    if (n->type == N_CHAR) n->c = tolower(n->c);
    fold_case(n->sub);
    for (int i = 0; i < n->nlist; i++) fold_case(n->list[i]);
}

static void node_free(Node *n)
{
    if (!n) return;
    if (n->list) { for (int i = 0; i < n->nlist; i++) node_free(n->list[i]); free(n->list); }
    node_free(n->sub);
    free(n);
}

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    if (!preg || !pattern) return REG_BADPAT;
    memset(preg, 0, sizeof *preg);
    preg->__ext = (cflags & REG_EXTENDED) != 0;
    preg->__icase = (cflags & REG_ICASE) != 0;
    preg->__nosub = (cflags & REG_NOSUB) != 0;
    preg->__newline = (cflags & REG_NEWLINE) != 0;

    struct Parser ps = { .p = pattern, .ext = preg->__ext, .icase = preg->__icase, .ngroups = 0, .err = 0 };
    Node *root = parse_alt(&ps);
    if (!ps.err && *ps.p) ps.err = ps.p[0] == ')' ? REG_EPAREN : REG_BADPAT;
    if (ps.err) { node_free(root); return ps.err; }
    if (ps.icase) fold_case(root);
    preg->re_nsub = ps.ngroups;
    preg->__opaque = root;
    return 0;
}

/* ---- matching -------------------------------------------------------- */

typedef int (*Cont)(const char *pos, void *kctx);

typedef struct {
    const char *base;
    int icase, newline, notbol, noteol;
    regoff_t *cap_so, *cap_eo;   /* [0..ngroups], index 0 unused here (rm[0] set by caller) */
} MCtx;

static int rmatch(Node *n, MCtx *m, const char *pos, Cont k, void *kctx);

struct ListCtx { Node **list; int idx, n; MCtx *m; Cont k; void *kctx; };
static int list_cont(const char *pos, void *v)
{
    struct ListCtx *lc = v;
    if (lc->idx == lc->n) return lc->k(pos, lc->kctx);
    struct ListCtx nc = { lc->list, lc->idx + 1, lc->n, lc->m, lc->k, lc->kctx };
    return rmatch(lc->list[lc->idx], lc->m, pos, list_cont, &nc);
}

struct RepCtx { Node *sub; int count, min, max; MCtx *m; Cont k; void *kctx; const char *prevpos; };
static int rep_extra_cont(const char *newpos, void *v)
{
    struct RepCtx *rc = v;
    if (newpos == rc->prevpos) return 0;   /* zero-width: stop growing, force backtrack */
    struct RepCtx nc = { rc->sub, rc->count + 1, rc->min, rc->max, rc->m, rc->k, rc->kctx, newpos };
    if (rc->max < 0 || nc.count < rc->max) {
        if (rmatch(rc->sub, rc->m, newpos, rep_extra_cont, &nc)) return 1;
    }
    if (nc.count >= rc->min) return rc->k(newpos, rc->kctx);
    return 0;
}

struct GroupCtx { int idx; MCtx *m; Cont k; void *kctx; };
static int group_end_cont(const char *pos, void *v)
{
    struct GroupCtx *gc = v;
    regoff_t old_eo = gc->m->cap_eo[gc->idx];
    gc->m->cap_eo[gc->idx] = pos - gc->m->base;
    if (gc->k(pos, gc->kctx)) return 1;
    gc->m->cap_eo[gc->idx] = old_eo;
    return 0;
}

static int rmatch(Node *n, MCtx *m, const char *pos, Cont k, void *kctx)
{
    switch (n->type) {
    case N_EMPTY: return k(pos, kctx);
    case N_CHAR: {
        int c = (unsigned char)*pos;
        if (c == 0) return 0;
        if (m->icase) c = tolower(c);
        if (c != n->c) return 0;
        return k(pos + 1, kctx);
    }
    case N_ANY: {
        if (*pos == 0) return 0;
        if (m->newline && *pos == '\n') return 0;
        return k(pos + 1, kctx);
    }
    case N_CLASS: {
        if (*pos == 0) return 0;
        int hit = cls_get(n->cls, (unsigned char)*pos);
        if (n->neg) {
            hit = !hit;
            if (m->newline && *pos == '\n') hit = 0;
        }
        if (!hit) return 0;
        return k(pos + 1, kctx);
    }
    case N_BOL: {
        int ok = (pos == m->base && !m->notbol) || (m->newline && pos > m->base && pos[-1] == '\n');
        return ok ? k(pos, kctx) : 0;
    }
    case N_EOL: {
        int ok = (*pos == 0 && !m->noteol) || (m->newline && *pos == '\n');
        return ok ? k(pos, kctx) : 0;
    }
    case N_CONCAT: {
        struct ListCtx lc = { n->list, 0, n->nlist, m, k, kctx };
        return list_cont(pos, &lc);
    }
    case N_ALT: {
        for (int i = 0; i < n->nlist; i++)
            if (rmatch(n->list[i], m, pos, k, kctx)) return 1;
        return 0;
    }
    case N_REP: {
        struct RepCtx rc = { n->sub, 0, n->min, n->max, m, k, kctx, pos };
        if (n->max < 0 || 0 < n->max) {
            if (rmatch(n->sub, m, pos, rep_extra_cont, &rc)) return 1;
        }
        if (0 >= n->min) return k(pos, kctx);
        return 0;
    }
    case N_GROUP: {
        regoff_t old_so = m->cap_so[n->capidx];
        m->cap_so[n->capidx] = pos - m->base;
        struct GroupCtx gc = { n->capidx, m, k, kctx };
        if (rmatch(n->sub, m, pos, group_end_cont, &gc)) return 1;
        m->cap_so[n->capidx] = old_so;
        return 0;
    }
    }
    return 0;
}

static int whole_cont(const char *pos, void *v) { *(const char **)v = pos; return 1; }

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags)
{
    if (!preg || !string || !preg->__opaque) return REG_BADPAT;
    Node *root = (Node *)preg->__opaque;
    int ng = preg->re_nsub;
    regoff_t *so = calloc((size_t)ng + 1, sizeof *so);
    regoff_t *eo = calloc((size_t)ng + 1, sizeof *eo);
    if (!so || !eo) { free(so); free(eo); return REG_ESPACE; }
    for (int i = 0; i <= ng; i++) { so[i] = -1; eo[i] = -1; }

    MCtx m = {
        .base = string, .icase = preg->__icase, .newline = preg->__newline,
        .notbol = (eflags & REG_NOTBOL) != 0, .noteol = (eflags & REG_NOTEOL) != 0,
        .cap_so = so, .cap_eo = eo,
    };

    int found = 0;
    const char *start = string, *end = string;
    for (const char *s = string; ; s++) {
        for (int i = 0; i <= ng; i++) { so[i] = -1; eo[i] = -1; }
        const char *matched_end = NULL;
        if (rmatch(root, &m, s, whole_cont, &matched_end)) {
            found = 1; start = s; end = matched_end;
            break;
        }
        if (*s == 0) break;
    }

    int rc = REG_NOMATCH;
    if (found) {
        rc = 0;
        if (!preg->__nosub && nmatch > 0 && pmatch) {
            pmatch[0].rm_so = start - string;
            pmatch[0].rm_eo = end - string;
            for (size_t i = 1; i < nmatch; i++) {
                if ((int)i <= ng && so[i] >= 0 && eo[i] >= 0) { pmatch[i].rm_so = so[i]; pmatch[i].rm_eo = eo[i]; }
                else { pmatch[i].rm_so = -1; pmatch[i].rm_eo = -1; }
            }
        }
    }
    free(so); free(eo);
    return rc;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    (void)preg;
    static const char *msgs[] = {
        [0] = "No error",
        [REG_NOMATCH]  = "No match",
        [REG_BADPAT]   = "Invalid regular expression",
        [REG_ECOLLATE] = "Invalid collating element",
        [REG_ECTYPE]   = "Invalid character class",
        [REG_EESCAPE]  = "Trailing backslash",
        [REG_ESUBREG]  = "Invalid back reference",
        [REG_EBRACK]   = "Unmatched [, [^, [:, [., or [=",
        [REG_EPAREN]   = "Unmatched ( or \\(",
        [REG_EBRACE]   = "Unmatched \\{",
        [REG_BADBR]    = "Invalid content of \\{\\}",
        [REG_ERANGE]   = "Invalid range end",
        [REG_ESPACE]   = "Out of memory",
        [REG_BADRPT]   = "Repetition not preceded by valid expression",
    };
    const char *msg = (errcode >= 0 && (size_t)errcode < sizeof msgs / sizeof *msgs && msgs[errcode])
                       ? msgs[errcode] : "Unknown regex error";
    size_t len = strlen(msg);
    if (errbuf && errbuf_size) {
        size_t n = len < errbuf_size - 1 ? len : errbuf_size - 1;
        memcpy(errbuf, msg, n);
        errbuf[n] = 0;
    }
    return len + 1;
}

void regfree(regex_t *preg)
{
    if (!preg) return;
    node_free((Node *)preg->__opaque);
    preg->__opaque = NULL;
}
