/* SPDX-License-Identifier: MIT */
#include "include/project.h"
#include "ir/model.h"
#include "frontend/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Guest Studio has no LLVM. After the A2 VM left, Run died with AS3501 on
 * every program, including `print("hello world")`. This evaluates the already
 * checked tree with the same print formatting as runtime/print.c. It is not a
 * second language: unsupported nodes refuse instead of inventing a VM. */

enum {
    EV_NONE,
    EV_INT,
    EV_UINT,
    EV_BOOL,
    EV_FLOAT,
    EV_STR
};

typedef struct {
    int kind;
    int64_t i;
    uint64_t u;
    double f;
    const char *s;
    int64_t n;
    char *owned;
} Ev;

typedef struct {
    AsTypedProject *p;
    AtFunction *f;
    Ev locals[AT_LOCALS];
    int failed;
    int64_t status;
} Ex;

static void ev_clear(Ev *v)
{
    free(v->owned);
    memset(v, 0, sizeof *v);
}

static Ev ev_int(int64_t i)
{
    Ev v = {.kind = EV_INT, .i = i};
    return v;
}

static Ev ev_str(const char *s, int64_t n, char *owned)
{
    Ev v = {.kind = EV_STR, .s = s, .n = n, .owned = owned};
    return v;
}

static int fail(Ex *x, const char *name, const char *detail)
{
    fprintf(stderr, "%s: %s\n", name, detail);
    x->failed = 1;
    x->status = 1;
    return 0;
}

/* Name the refused construct. The old generic strings made a Studio Run
 * failure look like a crash; naming the node turns the same refusal into a
 * pointer at the boundary and at the remedy (the host runs the full language
 * through LLVM). */
static const char *stmt_name(AtNode *n)
{
    switch (n->kind) {
    case AN_BREAK:
        return "break";
    case AN_CONTINUE:
        return "continue";
    case AN_WITH:
        return "with";
    case AN_ASSIGN:
        /* Plain `name = value` never reaches the refusal branch. */
        return n->op == T_ASSIGN ? "field assignment" : "compound assignment";
    default:
        return "this statement";
    }
}

static const char *op_name(int op)
{
    switch (op) {
    case T_AMP:
        return "&";
    case T_PIPE:
        return "|";
    case T_CARET:
        return "^";
    case T_SHL:
        return "<<";
    case T_SHR:
        return ">>";
    case T_POW:
        return "**";
    case T_MINUS:
        return "-";
    case T_TILDE:
        return "~";
    case T_NOT:
        return "not";
    default:
        return "this operator";
    }
}

static Ev eval_expr(Ex *x, AtNode *n);
static int eval_stmts(Ex *x, AtNode *n);

static Ev eval_string(AtNode *n)
{
    int bytes = as_token_decode(n->token, NULL);
    char *s = malloc((size_t)bytes + 1);
    if (!s) {
        Ev v = {0};
        return v;
    }
    as_token_decode(n->token, s);
    s[bytes] = 0;
    return ev_str(s, bytes, s);
}

static void print_value(Ev v)
{
    if (v.kind == EV_STR) {
        if (v.n > 0) {
            fwrite(v.s, 1, (size_t)v.n, stdout);
        }
    } else if (v.kind == EV_INT) {
        printf("%lld", (long long)v.i);
    } else if (v.kind == EV_UINT) {
        printf("%llu", (unsigned long long)v.u);
    } else if (v.kind == EV_BOOL) {
        fputs(v.i ? "true" : "false", stdout);
    } else if (v.kind == EV_FLOAT) {
        printf("%.17g", v.f);
    } else {
        fputs("None", stdout);
    }
}

static Ev eval_print(Ex *x, AtNode *n)
{
    for (int i = 0; i < n->count; i++) {
        if (i) {
            fputc(' ', stdout);
        }
        Ev a = eval_expr(x, n->args[i]);
        if (!x->failed) {
            print_value(a);
        }
        ev_clear(&a);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return (Ev){0};
}

static Ev eval_binary(Ex *x, AtNode *n)
{
    Ev left = eval_expr(x, n->a);
    Ev right = eval_expr(x, n->b);
    Ev out = {0};
    if (x->failed) {
        ev_clear(&left);
        ev_clear(&right);
        return out;
    }
    if (left.kind == EV_STR && right.kind == EV_STR && n->op == T_PLUS) {
        int64_t nlen = left.n + right.n;
        char *s = malloc((size_t)nlen + 1);
        if (!s) {
            fail(x, "MemoryError", "string concatenation");
        } else {
            memcpy(s, left.s, (size_t)left.n);
            memcpy(s + left.n, right.s, (size_t)right.n);
            s[nlen] = 0;
            out = ev_str(s, nlen, s);
        }
        ev_clear(&left);
        ev_clear(&right);
        return out;
    }
    if (left.kind != EV_INT || right.kind != EV_INT) {
        fail(x, "RuntimeError", "guest execution handles integers, bools and strings here");
        ev_clear(&left);
        ev_clear(&right);
        return out;
    }
    int64_t a = left.i, b = right.i, r = 0;
    switch (n->op) {
    case T_PLUS:
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
            fail(x, "OverflowError", "integer +");
            break;
        }
        r = a + b;
        break;
    case T_MINUS:
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
            fail(x, "OverflowError", "integer -");
            break;
        }
        r = a - b;
        break;
    case T_STAR:
        if (a && (b > INT64_MAX / a || b < INT64_MIN / a)) {
            fail(x, "OverflowError", "integer *");
            break;
        }
        r = a * b;
        break;
    case T_SLASH:
        if (!b) {
            fail(x, "ZeroDivisionError", "integer /");
            break;
        }
        if (a == INT64_MIN && b == -1) {
            fail(x, "OverflowError", "integer /");
            break;
        }
        r = a / b;
        break;
    case T_PERCENT:
        if (!b) {
            fail(x, "ZeroDivisionError", "integer %");
            break;
        }
        if (a == INT64_MIN && b == -1) {
            fail(x, "OverflowError", "integer %");
            break;
        }
        r = a % b;
        break;
    case T_EQ:
        r = a == b;
        out.kind = EV_BOOL;
        out.i = r;
        ev_clear(&left);
        ev_clear(&right);
        return out;
    case T_NE:
        out.kind = EV_BOOL;
        out.i = a != b;
        ev_clear(&left);
        ev_clear(&right);
        return out;
    case T_LT:
        out.kind = EV_BOOL;
        out.i = a < b;
        ev_clear(&left);
        ev_clear(&right);
        return out;
    case T_LE:
        out.kind = EV_BOOL;
        out.i = a <= b;
        ev_clear(&left);
        ev_clear(&right);
        return out;
    case T_GT:
        out.kind = EV_BOOL;
        out.i = a > b;
        ev_clear(&left);
        ev_clear(&right);
        return out;
    case T_GE:
        out.kind = EV_BOOL;
        out.i = a >= b;
        ev_clear(&left);
        ev_clear(&right);
        return out;
    default: {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "guest execution does not implement `%s` here — run the program on the host",
                 op_name(n->op));
        fail(x, "RuntimeError", buf);
        ev_clear(&left);
        ev_clear(&right);
        return out;
    }
    }
    ev_clear(&left);
    ev_clear(&right);
    return ev_int(r);
}

static Ev eval_expr(Ex *x, AtNode *n)
{
    Ev v = {0};
    if (!n || x->failed) {
        return v;
    }
    if (n->kind == AN_INT) {
        return ev_int((int64_t)n->integer);
    }
    if (n->kind == AN_BOOL) {
        v.kind = EV_BOOL;
        v.i = n->token.type == T_TRUE || (n->token.len > 0 && n->token.start[0] == 'T');
        return v;
    }
    if (n->kind == AN_FLOAT) {
        v.kind = EV_FLOAT;
        v.f = strtod(n->token.start, NULL);
        return v;
    }
    if (n->kind == AN_STR) {
        return eval_string(n);
    }
    if (n->kind == AN_NONE) {
        return v;
    }
    if (n->kind == AN_NAME) {
        if (n->symbol >= 0 && n->symbol < AT_LOCALS) {
            Ev local = x->locals[n->symbol];
            local.owned = NULL;
            return local;
        }
        fail(x, "RuntimeError", "unbound name in guest execution");
        return v;
    }
    if (n->kind == AN_UNARY) {
        Ev a = eval_expr(x, n->a);
        if (n->op == T_MINUS && a.kind == EV_INT) {
            if (a.i == INT64_MIN) {
                fail(x, "OverflowError", "integer negate");
            } else {
                a.i = -a.i;
            }
        } else if (n->op == T_MINUS && a.kind == EV_FLOAT) {
            a.f = -a.f;
        } else if (n->op == T_NOT) {
            int truth = a.kind == EV_BOOL ? (int)a.i : a.kind == EV_INT ? a.i != 0 : a.kind == EV_STR ? a.n != 0 : 0;
            ev_clear(&a);
            a.kind = EV_BOOL;
            a.i = !truth;
        } else if (!x->failed) {
            /* Refuse by name, like every other gap here: returning the
             * operand unchanged made `print(-1.5)` print 1.5 and `print(~5)`
             * print 5 -- a wrong answer is worse than a refusal
             * (2026-09-16 audit). */
            char buf[96];
            snprintf(buf, sizeof buf,
                     "guest execution does not implement unary `%s` here",
                     op_name(n->op));
            fail(x, "RuntimeError", buf);
        }
        return a;
    }
    if (n->kind == AN_BINARY) {
        return eval_binary(x, n);
    }
    if (n->kind == AN_CONDITIONAL) {
        Ev c = eval_expr(x, n->a);
        int truth = c.kind == EV_BOOL ? (int)c.i : c.kind == EV_INT ? c.i != 0 : 0;
        ev_clear(&c);
        return eval_expr(x, truth ? n->b : n->c);
    }
    if (n->kind == AN_CALL) {
        if (n->symbol == AT_CALL_PRINT) {
            return eval_print(x, n);
        }
        char buf[160];
        if (n->symbol >= 0 && n->symbol < x->p->nfunctions) {
            snprintf(buf, sizeof buf,
                     "guest execution does not implement the call to `%s` here — run the program on the host",
                     x->p->functions[n->symbol].name);
        } else {
            snprintf(buf, sizeof buf,
                     "guest execution does not implement this call here — run the program on the host");
        }
        fail(x, "RuntimeError", buf);
        return v;
    }
    fail(x, "RuntimeError", "guest execution does not implement this expression yet");
    return v;
}

static int eval_stmts(Ex *x, AtNode *n)
{
    while (n && !x->failed) {
        if (n->kind == AN_EXPR) {
            Ev v = eval_expr(x, n->a);
            ev_clear(&v);
        } else if (n->kind == AN_PASS) {
            /* nothing */
        } else if (n->kind == AN_ASSIGN && n->a && n->a->kind == AN_NAME && n->op == T_ASSIGN) {
            Ev v = eval_expr(x, n->b);
            /* A string read from another local arrives as a borrow
             * (owned == NULL): deep-copy it before it enters the slot, or the
             * next reassignment of the SOURCE slot would free a buffer this
             * slot still points at. ASan-confirmed UAF before this copy
             * (2026-09-16 audit). */
            if (!x->failed && v.kind == EV_STR && !v.owned && v.s) {
                char *copy = malloc((size_t)v.n + 1);
                if (!copy) {
                    fail(x, "MemoryError", "string assignment");
                } else {
                    memcpy(copy, v.s, (size_t)v.n);
                    copy[v.n] = 0;
                    v.s = copy;
                    v.owned = copy;
                }
            }
            if (!x->failed && n->a->symbol >= 0 && n->a->symbol < AT_LOCALS) {
                ev_clear(&x->locals[n->a->symbol]);
                x->locals[n->a->symbol] = v;
                v.owned = NULL;
            }
            ev_clear(&v);
        } else if (n->kind == AN_IF) {
            Ev c = eval_expr(x, n->a);
            int truth = c.kind == EV_BOOL ? (int)c.i : c.kind == EV_INT ? c.i != 0 : 0;
            ev_clear(&c);
            if (eval_stmts(x, truth ? n->b : n->c)) {
                return 1;
            }
        } else if (n->kind == AN_WHILE) {
            for (;;) {
                Ev c = eval_expr(x, n->a);
                int truth = c.kind == EV_BOOL ? (int)c.i : c.kind == EV_INT ? c.i != 0 : 0;
                ev_clear(&c);
                if (!truth || x->failed) {
                    break;
                }
                if (eval_stmts(x, n->b)) {
                    return 1;
                }
            }
        } else if (n->kind == AN_FOR && n->b && n->b->kind == AN_CALL &&
                   n->b->symbol == AT_CALL_RANGE && n->a && n->a->kind == AN_NAME) {
            int64_t start = 0, stop = 0, step = 1;
            if (n->b->count == 1) {
                Ev a = eval_expr(x, n->b->args[0]);
                stop = a.i;
                ev_clear(&a);
            } else if (n->b->count >= 2) {
                Ev a = eval_expr(x, n->b->args[0]);
                Ev b = eval_expr(x, n->b->args[1]);
                start = a.i;
                stop = b.i;
                ev_clear(&a);
                ev_clear(&b);
                if (n->b->count >= 3) {
                    Ev c = eval_expr(x, n->b->args[2]);
                    step = c.i;
                    ev_clear(&c);
                }
            }
            if (!step) {
                fail(x, "ValueError", "range() step is zero");
                return 0;
            }
            int slot = n->a->symbol;
            for (int64_t i = start; !x->failed && (step > 0 ? i < stop : i > stop); i += step) {
                if (slot >= 0 && slot < AT_LOCALS) {
                    ev_clear(&x->locals[slot]);
                    x->locals[slot] = ev_int(i);
                }
                if (eval_stmts(x, n->c)) {
                    return 1;
                }
            }
        } else if (n->kind == AN_RETURN) {
            if (n->a) {
                Ev v = eval_expr(x, n->a);
                if (v.kind == EV_INT) {
                    x->status = v.i;
                }
                ev_clear(&v);
            }
            return 1;
        } else {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "guest execution does not implement `%s` here — run the program on the host",
                     stmt_name(n));
            fail(x, "RuntimeError", buf);
            return 0;
        }
        n = n->next;
    }
    return 0;
}

int as_typed_eval(AsTypedProject *p)
{
    int main = -1;
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *f = &p->functions[i];
        if (f->module_initializer || f->method_owner || f->lexical_parent || f->generic_count ||
            f->template_id >= 0) {
            continue;
        }
        if (f->module == 0 && !strcmp(f->name, "main")) {
            main = i;
        }
    }
    if (main < 0) {
        fprintf(stderr, "Native executable requires main() -> i64 or main() -> None\n");
        return 1;
    }
    Ex x = {.p = p, .f = &p->functions[main]};
    eval_stmts(&x, x.f->body);
    for (int i = 0; i < AT_LOCALS; i++) {
        ev_clear(&x.locals[i]);
    }
    return x.failed ? 1 : (int)x.status;
}
