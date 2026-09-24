/* lre -- see lre.h for what it is and why it exists.
 *
 * Two stages. The parser turns the pattern into a tree (recursive descent,
 * nesting bounded by LRE_MAXDEPTH); the compiler turns the tree into a Pike VM
 * program; lre_exec runs every thread of that program over the input in lock
 * step. Nothing recurses on the INPUT: adding a thread follows the program's
 * epsilon edges with an explicit stack, so a long line costs time, never
 * stack. */
#include <stdlib.h>
#include <string.h>
#include "lre.h"

#define LRE_MAXPROG  32768             /* instructions; x{255} of something big is refused */
#define LRE_MAXDEPTH 64                /* parenthesis nesting */
#define LRE_DUPMAX   255               /* RE_DUP_MAX */

enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_MATCH,
       I_BOL, I_EOL, I_WORDB, I_NWORDB, I_WBEG, I_WEND };

struct inst { unsigned char op; int x, y; };        /* x: char/class/target/slot; y: 2nd target */

/* ---------------------------------------------------------------- tree -- */

enum { N_CHAR, N_ANY, N_CLASS, N_ASSERT, N_CAT, N_ALT, N_REP, N_GROUP, N_EMPTY };

struct node {
    int kind;
    int v;                  /* char, class index, assert op, group number */
    int min, max;           /* N_REP; max < 0 = unbounded */
    struct node *a, *b;     /* children: CAT/ALT pair, REP/GROUP body */
};

struct lre {
    struct inst *prog;
    int nprog, cap;
    unsigned char (*cls)[32];
    int ncls, clscap;
    int ngroup;
    /* exec workspace, sized to nprog at compile time */
    int *tpc, *tcap, *npc, *ncap, *mark, *stk;
    int markgen;
};

struct parser {
    const char *p;
    int ere, icase, depth, ngroup, err;
    char *msg; int msglen;
    struct lre *re;
};

static void perr(struct parser *ps, const char *m)
{
    if (ps->err) return;
    ps->err = 1;
    if (ps->msg && ps->msglen > 0) {
        int i = 0;
        for (; m[i] && i < ps->msglen - 1; i++) ps->msg[i] = m[i];
        ps->msg[i] = 0;
    }
}

static struct node *mk(struct parser *ps, int kind)
{
    struct node *n = calloc(1, sizeof *n);
    if (!n) { perr(ps, "out of memory"); return 0; }
    n->kind = kind;
    return n;
}

static void freetree(struct node *n)
{
    if (!n) return;
    freetree(n->a); freetree(n->b); free(n);
}

static int newclass(struct parser *ps)
{
    struct lre *re = ps->re;
    if (re->ncls == re->clscap) {
        int c = re->clscap ? re->clscap * 2 : 8;
        void *p = realloc(re->cls, (size_t)c * 32);
        if (!p) { perr(ps, "out of memory"); return -1; }
        re->cls = p; re->clscap = c;
    }
    memset(re->cls[re->ncls], 0, 32);
    return re->ncls++;
}

static void setbit(unsigned char *set, int c) { set[(c & 255) >> 3] |= (unsigned char)(1 << (c & 7)); }
static int  hasbit(const unsigned char *set, int c) { return set[(c & 255) >> 3] >> (c & 7) & 1; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int isword(int c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static int posix_class(const char *name, int len, int c)
{
#define IS(s) (len == (int)sizeof(s) - 1 && !memcmp(name, s, (size_t)len))
    if (IS("alpha"))  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (IS("digit"))  return c >= '0' && c <= '9';
    if (IS("alnum"))  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (IS("upper"))  return c >= 'A' && c <= 'Z';
    if (IS("lower"))  return c >= 'a' && c <= 'z';
    if (IS("space"))  return c == ' ' || (c >= '\t' && c <= '\r');
    if (IS("blank"))  return c == ' ' || c == '\t';
    if (IS("punct"))  return c > 32 && c < 127 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    if (IS("print"))  return c >= 32 && c < 127;
    if (IS("graph"))  return c > 32 && c < 127;
    if (IS("cntrl"))  return c < 32 || c == 127;
    if (IS("xdigit")) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
#undef IS
    return -1;
}

static struct node *leaf_char(struct parser *ps, int c)
{
    if (ps->icase && lower(c) != upper(c)) {
        int k = newclass(ps);
        if (k < 0) return 0;
        setbit(ps->re->cls[k], lower(c)); setbit(ps->re->cls[k], upper(c));
        struct node *n = mk(ps, N_CLASS); if (n) n->v = k;
        return n;
    }
    struct node *n = mk(ps, N_CHAR); if (n) n->v = c & 255;
    return n;
}

static struct node *leaf_set(struct parser *ps, int (*pred)(int), int negate)
{
    int k = newclass(ps);
    if (k < 0) return 0;
    for (int c = 0; c < 256; c++) if (!!pred(c) != !!negate) setbit(ps->re->cls[k], c);
    struct node *n = mk(ps, N_CLASS); if (n) n->v = k;
    return n;
}
static int is_space_c(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

/* After the '['. */
static struct node *bracket(struct parser *ps)
{
    int k = newclass(ps);
    if (k < 0) return 0;
    unsigned char *set = ps->re->cls[k];
    int neg = 0, first = 1;
    if (*ps->p == '^') { neg = 1; ps->p++; }
    for (;;) {
        int c = (unsigned char)*ps->p;
        if (!c) { perr(ps, "unterminated [ ] bracket expression"); return 0; }
        if (c == ']' && !first) { ps->p++; break; }
        first = 0;
        if (c == '[' && ps->p[1] == ':') {
            const char *e = strstr(ps->p + 2, ":]");
            if (!e) { perr(ps, "unterminated [: :] class"); return 0; }
            int len = (int)(e - (ps->p + 2)), any = 0;
            for (int ch = 0; ch < 256; ch++) {
                int r = posix_class(ps->p + 2, len, ch);
                if (r < 0) { perr(ps, "unknown character class"); return 0; }
                if (r) { setbit(set, ch); any = 1; }
            }
            (void)any;
            ps->p = e + 2;
            continue;
        }
        if ((c == '[' && (ps->p[1] == '.' || ps->p[1] == '='))) {
            char close = ps->p[1];
            const char *e = ps->p + 2;
            if (!e[0] || e[1] != close || e[2] != ']') { perr(ps, "unsupported collating element"); return 0; }
            setbit(set, (unsigned char)e[0]);
            ps->p = e + 3;
            continue;
        }
        ps->p++;
        int hi = c;
        if (*ps->p == '-' && ps->p[1] && ps->p[1] != ']') {
            hi = (unsigned char)ps->p[1];
            ps->p += 2;
            if (hi < c) { perr(ps, "invalid range end"); return 0; }
        }
        for (int ch = c; ch <= hi; ch++) setbit(set, ch);
    }
    if (ps->icase)
        for (int ch = 0; ch < 256; ch++)
            if (hasbit(set, ch)) { setbit(set, lower(ch)); setbit(set, upper(ch)); }
    if (neg) for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
    struct node *n = mk(ps, N_CLASS); if (n) n->v = k;
    return n;
}

static struct node *alt(struct parser *ps);

static int at_branch_end(struct parser *ps, const char *q)
{
    if (!*q) return 1;
    if (ps->ere) return *q == '|' || *q == ')';
    return q[0] == '\\' && (q[1] == '|' || q[1] == ')');
}

/* Parse "{m}", "{m,}", "{m,n}" (after the brace). -1 if not an interval. */
static int interval(struct parser *ps, int *mn, int *mx)
{
    const char *q = ps->p;
    if (!(*q >= '0' && *q <= '9')) return -1;
    int m = 0, n;
    while (*q >= '0' && *q <= '9') { m = m * 10 + (*q - '0'); if (m > LRE_DUPMAX) break; q++; }
    if (*q == ',') {
        q++;
        if (*q >= '0' && *q <= '9') { n = 0; while (*q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); if (n > LRE_DUPMAX) break; q++; } }
        else n = -1;
    } else n = m;
    if (ps->ere) { if (*q != '}') return -1; q++; }
    else { if (q[0] != '\\' || q[1] != '}') return -1; q += 2; }
    if (m > LRE_DUPMAX || n > LRE_DUPMAX) { perr(ps, "repetition count too large"); return 0; }
    if (n >= 0 && n < m) { perr(ps, "invalid repetition count(s)"); return 0; }
    *mn = m; *mx = n;
    ps->p = q;
    return 1;
}

static struct node *atom(struct parser *ps, int branch_start)
{
    int c = (unsigned char)*ps->p;
    struct node *n;
    if (ps->ere && c == '(') {
        ps->p++;
        if (++ps->depth > LRE_MAXDEPTH) { perr(ps, "parentheses nested too deeply"); return 0; }
        int g = ++ps->ngroup;
        struct node *body = (*ps->p == ')') ? mk(ps, N_EMPTY) : alt(ps);
        ps->depth--;
        if (ps->err) { freetree(body); return 0; }
        if (*ps->p != ')') { freetree(body); perr(ps, "unmatched ( or \\("); return 0; }
        ps->p++;
        n = mk(ps, N_GROUP); if (!n) { freetree(body); return 0; }
        n->v = g; n->a = body;
        return n;
    }
    if (!ps->ere && c == '\\' && ps->p[1] == '(') {
        ps->p += 2;
        if (++ps->depth > LRE_MAXDEPTH) { perr(ps, "parentheses nested too deeply"); return 0; }
        int g = ++ps->ngroup;
        struct node *body = (ps->p[0] == '\\' && ps->p[1] == ')') ? mk(ps, N_EMPTY) : alt(ps);
        ps->depth--;
        if (ps->err) { freetree(body); return 0; }
        if (!(ps->p[0] == '\\' && ps->p[1] == ')')) { freetree(body); perr(ps, "unmatched ( or \\("); return 0; }
        ps->p += 2;
        n = mk(ps, N_GROUP); if (!n) { freetree(body); return 0; }
        n->v = g; n->a = body;
        return n;
    }
    ps->p++;
    switch (c) {
    case '.': return mk(ps, N_ANY);
    case '[': return bracket(ps);
    case '^':
        if (ps->ere || branch_start) { n = mk(ps, N_ASSERT); if (n) n->v = I_BOL; return n; }
        return leaf_char(ps, c);
    case '$':
        if (ps->ere || at_branch_end(ps, ps->p)) { n = mk(ps, N_ASSERT); if (n) n->v = I_EOL; return n; }
        return leaf_char(ps, c);
    case '\\': {
        int e = (unsigned char)*ps->p;
        if (!e) { perr(ps, "trailing backslash (\\)"); return 0; }
        ps->p++;
        if (e >= '1' && e <= '9') { perr(ps, "back-references in a pattern are not supported"); return 0; }
        switch (e) {
        case 'w': return leaf_set(ps, isword, 0);
        case 'W': return leaf_set(ps, isword, 1);
        case 's': return leaf_set(ps, is_space_c, 0);
        case 'S': return leaf_set(ps, is_space_c, 1);
        case 'b': n = mk(ps, N_ASSERT); if (n) n->v = I_WORDB;  return n;
        case 'B': n = mk(ps, N_ASSERT); if (n) n->v = I_NWORDB; return n;
        case '<': n = mk(ps, N_ASSERT); if (n) n->v = I_WBEG;   return n;
        case '>': n = mk(ps, N_ASSERT); if (n) n->v = I_WEND;   return n;
        case '`': n = mk(ps, N_ASSERT); if (n) n->v = I_BOL;    return n;
        case '\'': n = mk(ps, N_ASSERT); if (n) n->v = I_EOL;   return n;
        case 'n': return leaf_char(ps, '\n');
        default:  return leaf_char(ps, e);
        }
    }
    default:
        return leaf_char(ps, c);
    }
}

static int rep_op(struct parser *ps, int *mn, int *mx)
{
    const char *q = ps->p;
    if (*q == '*') { ps->p++; *mn = 0; *mx = -1; return 1; }
    if (ps->ere) {
        if (*q == '+') { ps->p++; *mn = 1; *mx = -1; return 1; }
        if (*q == '?') { ps->p++; *mn = 0; *mx = 1; return 1; }
        if (*q == '{') { ps->p++; int r = interval(ps, mn, mx); if (r < 0) ps->p--; return r > 0; }
        return 0;
    }
    if (q[0] == '\\' && q[1] == '+') { ps->p += 2; *mn = 1; *mx = -1; return 1; }
    if (q[0] == '\\' && q[1] == '?') { ps->p += 2; *mn = 0; *mx = 1; return 1; }
    if (q[0] == '\\' && q[1] == '{') {
        ps->p += 2;
        int r = interval(ps, mn, mx);
        if (r < 0) { perr(ps, "invalid content of \\{\\}"); return 0; }
        return r > 0;
    }
    return 0;
}

static struct node *branch(struct parser *ps)
{
    struct node *seq = 0;
    int start = 1;
    for (;;) {
        const char *q = ps->p;
        if (!*q) break;
        if (ps->ere && (*q == '|' || *q == ')')) break;
        if (!ps->ere && q[0] == '\\' && (q[1] == '|' || q[1] == ')')) break;
        struct node *a;
        /* A '*' where nothing precedes it is a literal (POSIX for BRE; GNU and
         * BSD agree for ERE). */
        if (*q == '*' && start) { ps->p++; a = leaf_char(ps, '*'); }
        else a = atom(ps, start);
        if (ps->err || !a) { freetree(seq); freetree(a); return 0; }
        int mn, mx;
        while (rep_op(ps, &mn, &mx)) {
            if (a->kind == N_ASSERT && !ps->ere) break;
            struct node *r = mk(ps, N_REP);
            if (!r) { freetree(a); freetree(seq); return 0; }
            r->a = a; r->min = mn; r->max = mx;
            a = r;
        }
        if (ps->err) { freetree(a); freetree(seq); return 0; }
        start = 0;
        if (!seq) seq = a;
        else {
            struct node *c = mk(ps, N_CAT);
            if (!c) { freetree(a); freetree(seq); return 0; }
            c->a = seq; c->b = a; seq = c;
        }
    }
    return seq ? seq : mk(ps, N_EMPTY);
}

static struct node *alt(struct parser *ps)
{
    struct node *l = branch(ps);
    while (!ps->err && l) {
        const char *q = ps->p;
        if (ps->ere && *q == '|') ps->p++;
        else if (!ps->ere && q[0] == '\\' && q[1] == '|') ps->p += 2;
        else break;
        struct node *r = branch(ps);
        if (!r) { freetree(l); return 0; }
        struct node *a = mk(ps, N_ALT);
        if (!a) { freetree(l); freetree(r); return 0; }
        a->a = l; a->b = r; l = a;
    }
    return l;
}

/* ------------------------------------------------------------- compile -- */

static int emit(struct parser *ps, int op, int x, int y)
{
    struct lre *re = ps->re;
    if (re->nprog >= LRE_MAXPROG) { perr(ps, "regular expression too big"); return -1; }
    if (re->nprog == re->cap) {
        int c = re->cap ? re->cap * 2 : 64;
        void *p = realloc(re->prog, (size_t)c * sizeof *re->prog);
        if (!p) { perr(ps, "out of memory"); return -1; }
        re->prog = p; re->cap = c;
    }
    re->prog[re->nprog].op = (unsigned char)op;
    re->prog[re->nprog].x = x;
    re->prog[re->nprog].y = y;
    return re->nprog++;
}

static void gen(struct parser *ps, const struct node *n)
{
    if (ps->err || !n) return;
    struct lre *re = ps->re;
    switch (n->kind) {
    case N_EMPTY: break;
    case N_CHAR:  emit(ps, I_CHAR, n->v, 0); break;
    case N_ANY:   emit(ps, I_ANY, 0, 0); break;
    case N_CLASS: emit(ps, I_CLASS, n->v, 0); break;
    case N_ASSERT: emit(ps, n->v, 0, 0); break;
    case N_CAT:   gen(ps, n->a); gen(ps, n->b); break;
    case N_GROUP:
        if (n->v < LRE_NSUB) emit(ps, I_SAVE, 2 * n->v, 0);
        gen(ps, n->a);
        if (n->v < LRE_NSUB) emit(ps, I_SAVE, 2 * n->v + 1, 0);
        break;
    case N_ALT: {
        int s = emit(ps, I_SPLIT, 0, 0);
        if (s < 0) return;
        re->prog[s].x = re->nprog;
        gen(ps, n->a);
        int j = emit(ps, I_JMP, 0, 0);
        if (j < 0) return;
        re->prog[s].y = re->nprog;
        gen(ps, n->b);
        re->prog[j].x = re->nprog;
        break;
    }
    case N_REP: {
        for (int i = 0; i < n->min; i++) gen(ps, n->a);
        if (n->max < 0) {                          /* greedy star over the rest */
            int s = emit(ps, I_SPLIT, 0, 0);
            if (s < 0) return;
            re->prog[s].x = re->nprog;
            gen(ps, n->a);
            emit(ps, I_JMP, s, 0);
            re->prog[s].y = re->nprog;
        } else {
            int holes[LRE_DUPMAX + 1], nh = 0;
            for (int i = n->min; i < n->max; i++) {
                int s = emit(ps, I_SPLIT, 0, 0);
                if (s < 0) return;
                re->prog[s].x = re->nprog;
                holes[nh++] = s;
                gen(ps, n->a);
            }
            for (int i = 0; i < nh; i++) re->prog[holes[i]].y = re->nprog;
        }
        break;
    }
    }
}

int lre_compile(struct lre **out, const char *pattern, int flags, char *err, int errlen)
{
    *out = 0;
    struct lre *re = calloc(1, sizeof *re);
    if (!re) return -1;
    struct parser ps = { pattern, !!(flags & LRE_EXTENDED), !!(flags & LRE_ICASE),
                         0, 0, 0, err, errlen, re };
    struct node *t = alt(&ps);
    if (!ps.err && *ps.p) perr(&ps, ps.ere ? "unmatched ) or \\)" : "unmatched ) or \\)");
    if (!ps.err) {
        emit(&ps, I_SAVE, 0, 0);
        gen(&ps, t);
        emit(&ps, I_SAVE, 1, 0);
        emit(&ps, I_MATCH, 0, 0);
    }
    freetree(t);
    re->ngroup = ps.ngroup;
    if (!ps.err) {
        int n = re->nprog;
        re->tpc = malloc(sizeof(int) * (size_t)n);
        re->npc = malloc(sizeof(int) * (size_t)n);
        re->tcap = malloc(sizeof(int) * (size_t)n * 2 * LRE_NSUB);
        re->ncap = malloc(sizeof(int) * (size_t)n * 2 * LRE_NSUB);
        re->mark = calloc((size_t)n, sizeof(int));
        re->stk = malloc(sizeof(int) * ((size_t)n * 6 + 64));   /* <= 2 frames of 3 per visited pc */
        if (!re->tpc || !re->npc || !re->tcap || !re->ncap || !re->mark || !re->stk)
            perr(&ps, "out of memory");
    }
    if (ps.err) { lre_free(re); return -1; }
    *out = re;
    return 0;
}

void lre_free(struct lre *re)
{
    if (!re) return;
    free(re->prog); free(re->cls);
    free(re->tpc); free(re->npc); free(re->tcap); free(re->ncap); free(re->mark); free(re->stk);
    free(re);
}

int lre_nsub(const struct lre *re) { return re->ngroup; }

/* ----------------------------------------------------------------- exec -- */

struct run {
    struct lre *re;
    const char *s;
    int n, notbol;
};

static int assert_ok(const struct run *r, int op, int i)
{
    int before = i > 0 && isword((unsigned char)r->s[i - 1]);
    int after = i < r->n && isword((unsigned char)r->s[i]);
    switch (op) {
    case I_BOL:    return i == 0 && !r->notbol;
    case I_EOL:    return i == r->n;
    case I_WORDB:  return before != after;
    case I_NWORDB: return before == after;
    case I_WBEG:   return !before && after;
    case I_WEND:   return before && !after;
    }
    return 0;
}

/* Add the thread (pc, cap) to list `lpc/lcap` at position i, following
 * epsilon edges. An explicit stack of (pc, slot, old) frames: a frame with
 * slot >= 0 restores cap[slot] after everything reached through a SAVE. */
static void addthread(struct run *r, int *lpc, int *lcap, int *ln, int pc0, int *cap, int i, int gen)
{
    struct lre *re = r->re;
    int *st = re->stk, sp = 0;
    st[sp++] = pc0; st[sp++] = -1; st[sp++] = 0;
    while (sp) {
        int old = st[--sp], slot = st[--sp], pc = st[--sp];
        if (slot >= 0) { cap[slot] = old; continue; }
        if (re->mark[pc] == gen) continue;
        re->mark[pc] = gen;
        const struct inst *in = &re->prog[pc];
        switch (in->op) {
        case I_JMP:
            st[sp++] = in->x; st[sp++] = -1; st[sp++] = 0;
            break;
        case I_SPLIT:                               /* x first: push y below it */
            st[sp++] = in->y; st[sp++] = -1; st[sp++] = 0;
            st[sp++] = in->x; st[sp++] = -1; st[sp++] = 0;
            break;
        case I_SAVE:
            st[sp++] = 0; st[sp++] = in->x; st[sp++] = cap[in->x];   /* restore after */
            cap[in->x] = i;
            st[sp++] = pc + 1; st[sp++] = -1; st[sp++] = 0;
            break;
        case I_BOL: case I_EOL: case I_WORDB: case I_NWORDB: case I_WBEG: case I_WEND:
            if (assert_ok(r, in->op, i)) { st[sp++] = pc + 1; st[sp++] = -1; st[sp++] = 0; }
            break;
        default:
            lpc[*ln] = pc;
            memcpy(lcap + (size_t)*ln * 2 * LRE_NSUB, cap, sizeof(int) * 2 * LRE_NSUB);
            (*ln)++;
            break;
        }
    }
}

int lre_exec(const struct lre *cre, const char *s, int n, int from, int notbol,
             int sub[2 * LRE_NSUB])
{
    struct lre *re = (struct lre *)cre;
    struct run r = { re, s, n, notbol };
    int *cpc = re->tpc, *ccap = re->tcap, *npc = re->npc, *ncap = re->ncap;
    int cn = 0, nn, matched = 0;
    int best[2 * LRE_NSUB], cap[2 * LRE_NSUB];
    int gen = ++re->markgen;
    for (int i = from; ; i++) {
        if (!matched) {
            for (int k = 0; k < 2 * LRE_NSUB; k++) cap[k] = -1;
            addthread(&r, cpc, ccap, &cn, 0, cap, i, gen);
        }
        if (!cn) { if (matched || i >= n) break; gen = ++re->markgen; continue; }
        gen = ++re->markgen;
        nn = 0;
        for (int t = 0; t < cn; t++) {
            int *tc = ccap + (size_t)t * 2 * LRE_NSUB;
            const struct inst *in = &re->prog[cpc[t]];
            if (matched && tc[0] > best[0]) continue;           /* cannot be leftmost */
            if (in->op == I_MATCH) {
#ifdef TEXTUTIL_NEGCTL
                /* THE CONTROL: leftmost-FIRST, a backtracker's answer -- the
                 * highest-priority thread's match wins and nothing longer is
                 * looked for. `a|ab` on "ab" then matches "a". */
                if (!matched) {
                    memcpy(best, tc, sizeof best); best[1] = i; matched = 1;
                    for (int k = 0; k < 2 * LRE_NSUB; k++) sub[k] = best[k];
                    re->tpc = cpc; re->tcap = ccap; re->npc = npc; re->ncap = ncap;
                    return 1;
                }
#endif
                if (!matched || tc[0] < best[0] || (tc[0] == best[0] && i > best[1])) {
                    memcpy(best, tc, sizeof best);
                    best[1] = i;
                    matched = 1;
                }
                continue;
            }
            if (i >= n) continue;
            int c = (unsigned char)s[i], ok;
            switch (in->op) {
            case I_CHAR:  ok = c == in->x; break;
            case I_ANY:   ok = 1; break;
            case I_CLASS: ok = hasbit(re->cls[in->x], c); break;
            default:      ok = 0; break;
            }
            if (ok) {
                memcpy(cap, tc, sizeof cap);
                addthread(&r, npc, ncap, &nn, cpc[t] + 1, cap, i + 1, gen);
            }
        }
        int *tp = cpc; cpc = npc; npc = tp;
        int *tq = ccap; ccap = ncap; ncap = tq;
        cn = nn;
        if (i >= n) {
            if (!cn) break;
            /* Threads added at i+1 = n+1 cannot exist; flush MATCHes at n. */
        }
        if (i > n) break;
    }
    re->tpc = cpc; re->tcap = ccap; re->npc = npc; re->ncap = ncap;
    if (!matched) return 0;
    for (int k = 0; k < 2 * LRE_NSUB; k++) sub[k] = best[k];
    return 1;
}
