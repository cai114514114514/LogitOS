/* SPDX-License-Identifier: MIT */
#include "include/project.h"
#include "common/numeric.h"
#include "ir/model.h"
#include "frontend/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

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

/* HOW A STATEMENT LIST ENDS. This used to be 0 = fell through, 1 = returned,
 * and `break` had nowhere to go. Two more answers is the whole mechanism: a
 * loop consumes UN_BREAK and UN_CONTINUE, everything else passes them
 * outward, and UN_RETURN keeps travelling until a function body absorbs it.
 * A caller that treated any non-zero as "returned" would carry a break out
 * through its own function, which is the bug this enum exists to make
 * unrepresentable. */
enum { UN_FALL = 0, UN_RETURN, UN_BREAK, UN_CONTINUE };

/* One per PROGRAM, as against Ex which is one per CALL.
 *
 * Globals live here because two frames share them: a module's top-level
 * statements are compiled into __as_initialize_module and every other
 * function reads what they wrote. `failed` and `status` moved here for the
 * same reason -- a failure inside a callee has to stop the caller, and before
 * user calls existed there was only ever one frame to hold them. */
typedef struct {
    AsTypedProject *p;
    int depth;
    int failed;
    int64_t status;
    Ev globals[AT_GLOBALS];
} Run;

typedef struct {
    Run *r;
    AtFunction *f;
    Ev locals[AT_LOCALS];
    Ev ret;
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
    x->r->failed = 1;
    x->r->status = 1;
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
static Ev eval_call(Ex *x, AtNode *n);

/* A value leaving the frame that owns it. A borrowed string (owned == NULL)
 * points into a local slot the callee is about to clear, so it is copied; an
 * owned one is moved. This is the same rule assignment already states, at the
 * other boundary. */
static Ev ev_export(Ex *x, Ev v)
{
    if (v.kind != EV_STR || v.owned || !v.s) {
        return v;
    }
    char *copy = malloc((size_t)v.n + 1);
    if (!copy) {
        fail(x, "MemoryError", "returned string");
        return (Ev){0};
    }
    memcpy(copy, v.s, (size_t)v.n);
    copy[v.n] = 0;
    v.s = copy;
    v.owned = copy;
    return v;
}

static Ev eval_string(Ex *x, AtNode *n)
{
    int bytes = as_token_decode(n->token, NULL);
    char *s = malloc((size_t)bytes + 1);
    if (!s) {
        /* OOM used to return a silent zero-Ev, which print() rendered as
         * the string "None" -- a wrong answer instead of a refusal
         * (2026-09-16 audit). */
        fail(x, "MemoryError", "string literal");
        return (Ev){0};
    }
    as_token_decode(n->token, s);
    s[bytes] = 0;
    return ev_str(s, bytes, s);
}

/* ONE SPELLING OF A VALUE AS TEXT, shared by print, f-string holes and
 * str() (A4 F0/F1). Three producers with one table each is the one-jar-two-
 * doors drift the A4 plan names explicitly: f"{x}" and print(x) disagreeing
 * on the same value is a wrong answer in the one engine a Studio user runs.
 * The spelling is runtime/print.c's, byte for byte: int %lld, uint %llu,
 * bool true/false, float %.17g, None None -- and backend emit.c lowers
 * AN_FORMAT by "reusing precisely the str(value) conversion", so a single
 * function here mirrors both native surfaces. The result is heap-owned; the
 * caller clears it. A refused hole refuses the WHOLE f-string (never a
 * partial line), so this never fabricates a spelling for a kind it does not
 * hold: it fails, and absent beats present-and-wrong. */
static Ev ev_to_str(Ex *x, Ev v)
{
    if (v.kind == EV_STR) {
        return ev_export(x, v);
    }
    char buf[32];
    int n = 0;
    if (v.kind == EV_INT) {
        n = snprintf(buf, sizeof buf, "%lld", (long long)v.i);
    } else if (v.kind == EV_UINT) {
        n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v.u);
    } else if (v.kind == EV_BOOL) {
        n = snprintf(buf, sizeof buf, "%s", v.i ? "true" : "false");
    } else if (v.kind == EV_FLOAT) {
        n = snprintf(buf, sizeof buf, "%.17g", v.f);
    } else if (v.kind == EV_NONE) {
        n = snprintf(buf, sizeof buf, "None");
    } else {
        fail(x, "RuntimeError", "guest execution cannot format this value");
        return (Ev){0};
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        fail(x, "MemoryError", "value to string");
        return (Ev){0};
    }
    memcpy(s, buf, (size_t)n);
    s[n] = 0;
    return ev_str(s, n, s);
}

/* F-STRING HOLES AND str() SHARE THIS BUILD, and it exists as a function --
 * rather than a call to eval_binary's string-concat branch -- because the
 * tree text.c builds for an f-string IS ordinary `+` nodes around AN_FORMAT
 * holes, so a plain eval_expr of those nodes would print each line as the
 * concatenation finished: correct bytes, but a refused hole would leave a
 * PARTIAL line on stdout, which the A4 plan rules out (a truncated line is a
 * wrong answer). Building the text first and printing once the whole string
 * exists is what makes "a refused hole refuses the whole f-string by name"
 * true in output, not just in status. Whole-program exit status was never
 * enough: the case print(f"{xs[0]}") must print NOTHING before it fails. */
static Ev eval_str_of(Ex *x, AtNode *arg)
{
    Ev a = eval_expr(x, arg);
    Ev s = x->r->failed ? (Ev){0} : ev_to_str(x, a);
    ev_clear(&a);
    return s;
}

static Ev eval_format(Ex *x, AtNode *n)
{
    return eval_str_of(x, n->a);
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
        if (!x->r->failed) {
            print_value(a);
        }
        ev_clear(&a);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return (Ev){0};
}

/* Keep the integer's signedness until the conversion itself. Routing u64
 * through i64 makes values above INT64_MAX negative; routing an integer
 * through double before f32 can round twice instead of LLVM's direct cast. */
static double ev_float(Ev value, int type)
{
    if (type == AT_F32) {
        return value.kind == EV_UINT ? (double)(float)value.u
               : value.kind == EV_INT ? (double)(float)value.i
                                      : (double)(float)value.f;
    }
    return value.kind == EV_UINT ? (double)value.u
           : value.kind == EV_INT ? (double)value.i
                                  : value.f;
}

static Ev eval_float(Ex *x, AtNode *n, Ev left, Ev right, int type)
{
    double a = ev_float(left, type), b = ev_float(right, type);
    Ev out = {.kind = EV_FLOAT};
    /* The operand type comes from the children: a comparison's own type is
     * bool. f32 rounds BOTH operands and each result, including compound
     * assignment, rather than carrying hidden f64 precision in Ev.f. */
    if (n->op >= T_EQ && n->op <= T_GE) {
        out.kind = EV_BOOL;
        out.i = n->op == T_EQ ? a == b : n->op == T_NE ? a != b
                : n->op == T_LT ? a < b : n->op == T_LE ? a <= b
                : n->op == T_GT ? a > b : a >= b;
        return out;
    }
    if ((n->op == T_SLASH || n->op == T_PERCENT) && b == 0.0) {
        fail(x, "ZeroDivisionError", "float division or remainder");
        return out;
    }
    switch (n->op) {
    case T_PLUS:
        out.f = type == AT_F32 ? (double)((float)a + (float)b) : a + b;
        break;
    case T_MINUS:
        out.f = type == AT_F32 ? (double)((float)a - (float)b) : a - b;
        break;
    case T_STAR:
        out.f = type == AT_F32 ? (double)((float)a * (float)b) : a * b;
        break;
    case T_SLASH:
        out.f = type == AT_F32 ? (double)((float)a / (float)b) : a / b;
        break;
    case T_PERCENT:
        out.f = fmod(a, b);
        break;
    case T_POW:
        /* The native f32 path also widens to f64 pow then narrows: the guest
         * libc ships pow, not powf. IEEE NaN/inf remain values, not errors. */
        out.f = pow(a, b);
        break;
    default:
        fail(x, "RuntimeError", "guest execution refuses this float operator");
        return out;
    }
    if (type == AT_F32) {
        out.f = (double)(float)out.f;
    }
    return out;
}

static Ev eval_binary(Ex *x, AtNode *n)
{
    Ev left = eval_expr(x, n->a);
    Ev right = eval_expr(x, n->b);
    Ev out = {0};
    if (x->r->failed) {
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
    int numeric = at_numeric_type(x->r->p, n->a->type, n->b->type);
#ifdef AS_EVAL_NO_NUMERIC
    /* Restore the old float refusal so the printed-output gate can prove it
     * watches Studio's evaluator as well as the native compiler. */
    numeric = 0;
#endif
    if (numeric) {
        out = eval_float(x, n, left, right, numeric);
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
    /* THE BITWISE AND SHIFT OPERATORS, AND WHY THEY CARRY THE BACKEND'S
     * CHECKS RATHER THAN C's SEMANTICS. This evaluator and the LLVM backend
     * share no arithmetic, so every rule the backend spends instructions on
     * has to be restated here or the two disagree in the direction that is
     * hardest to see: a plausible number. at_ir_shift() (backend/llvm/
     * numeric.c) rejects a count outside [0, bits) with an UNSIGNED compare,
     * so a negative count is refused by the same test; and for `<<` it shifts
     * the result back and requires the original, which is how it catches a
     * value shifted off the top -- including a changed sign. C would give
     * undefined behaviour for both. */
    case T_AMP:
        r = a & b;
        break;
    case T_PIPE:
        r = a | b;
        break;
    case T_CARET:
        r = a ^ b;
        break;
    case T_SHL:
        if ((uint64_t)b >= 64) {
            fail(x, "ValueError", "shift count is outside the width of the type");
            break;
        }
        r = (int64_t)((uint64_t)a << b);
        if ((r >> b) != a) {
            fail(x, "OverflowError", "integer <<");
            r = 0;
        }
        break;
    case T_SHR:
        if ((uint64_t)b >= 64) {
            fail(x, "ValueError", "shift count is outside the width of the type");
            break;
        }
        r = a >> b;
        break;
    case T_POW: {
        /* Exponentiation by squaring with a CHECKED multiply, which is
         * checked_power() in the backend -- and its comment names the trap
         * this loop copies: never square the base after the last exponent
         * bit, because 100**1 fits while the unused 100*100 would not. */
        if (b < 0) {
            fail(x, "ValueError", "negative exponent for integer **");
            break;
        }
        int64_t acc = 1, factor = a;
        uint64_t rem = (uint64_t)b;
        while (rem) {
            if (rem & 1) {
                if (factor && (acc > INT64_MAX / factor || acc < INT64_MIN / factor)) {
                    fail(x, "OverflowError", "integer **");
                    break;
                }
                acc *= factor;
            }
            rem >>= 1;
            if (!rem) {
                break;
            }
            if (factor && (factor > INT64_MAX / factor || factor < INT64_MIN / factor)) {
                fail(x, "OverflowError", "integer **");
                break;
            }
            factor *= factor;
        }
        r = acc;
        break;
    }
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
    if (!n || x->r->failed) {
        return v;
    }
    if (n->kind == AN_INT) {
        if (at_integer(x->r->p, n->type) && !at_signed(x->r->p, n->type)) {
            v.kind = EV_UINT;
            v.u = n->integer;
            return v;
        }
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
        if (n->type == AT_F32) {
            v.f = (double)(float)v.f;
        }
        return v;
    }
    if (n->kind == AN_STR) {
        return eval_string(x, n);
    }
    if (n->kind == AN_FORMAT) {
#ifdef AS_EVAL_NO_FSTR
        /* NEGATIVE CONTROL (tests/aseval.mk): the refusal this node got
         * before f-strings were mirrored. Falls through to the generic
         * "does not implement this expression" at the bottom, and every
         * f-string case in the battery must go red against it. */
#else
        return eval_format(x, n);
#endif
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
    /* A MODULE-SCOPE VARIABLE IS NOT A LOCAL, and nothing here read one until
     * now. The four lines a person types first --
     *
     *     a = 1
     *     b = 2
     *     print(a / b)
     *
     * -- have no `def` at all, so the checker puts the assignments in
     * __as_initialize_module and the names become GLOBALS (AS3900 says so:
     * "top-level statements that become main()"). Reading one fell through to
     * the bottom of this function and reported `guest execution does not
     * implement this expression yet`, which named a node kind rather than the
     * thing the program did. Borrowed, not owned, exactly like a local. */
    if (n->kind == AN_GLOBAL) {
        if (n->symbol >= 0 && n->symbol < AT_GLOBALS) {
            Ev g = x->r->globals[n->symbol];
            g.owned = NULL;
            return g;
        }
        fail(x, "RuntimeError", "unbound global in guest execution");
        return v;
    }
    if (n->kind == AN_UNARY) {
        if (n->op == T_MINUS && n->a->kind == AN_INT) {
            /* The checker admits magnitude 2^63 only under literal minus.
             * Negating an already signed INT64_MIN would falsely overflow;
             * mirror native emit.c's bit-pattern construction instead. */
            return ev_int(as_i64_bits(0 - n->a->integer));
        }
        Ev a = eval_expr(x, n->a);
        if (n->op == T_PLUS &&
            (a.kind == EV_INT || a.kind == EV_UINT || a.kind == EV_FLOAT)) {
            return a;
        } else if (n->op == T_MINUS && a.kind == EV_INT) {
            if (a.i == INT64_MIN) {
                fail(x, "OverflowError", "integer negate");
            } else {
                a.i = -a.i;
            }
        } else if (n->op == T_MINUS && a.kind == EV_FLOAT) {
            a.f = -a.f;
        } else if (n->op == T_TILDE && a.kind == EV_INT) {
            /* ~x is -(x+1) and cannot overflow for any int64: the backend
             * emits `xor -1` with no guard, so neither does this. */
            a.i = ~a.i;
        } else if (n->op == T_NOT) {
            int truth = a.kind == EV_BOOL ? (int)a.i : a.kind == EV_INT ? a.i != 0 : a.kind == EV_STR ? a.n != 0 : 0;
            ev_clear(&a);
            a.kind = EV_BOOL;
            a.i = !truth;
        } else if (!x->r->failed) {
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
        /* str(v) is the same conversion f-string holes use, routed through
         * the one function so the two spellings cannot drift (the A4 plan's
         * reason for ev_to_str existing at all). Backend emit.c lowers
         * AN_FORMAT as exactly this call, so mirroring both through
         * eval_format is not a shortcut -- it is what the host compiles. */
        if (n->symbol == AT_CALL_STR && n->count == 1) {
#ifdef AS_EVAL_NO_STRCALL
            /* NEGATIVE CONTROL (tests/aseval.mk): restore the refusal that
             * AT_CALL_STR got before; the str() battery must go red. */
#else
            return eval_str_of(x, n->args[0]);
#endif
        }
        return eval_call(x, n);
    }
    fail(x, "RuntimeError", "guest execution does not implement this expression yet");
    return v;
}

/* CALLING A FUNCTION THE PROGRAM DECLARED.
 *
 * THE FRAME IS ON THE HEAP, NOT THE C STACK, and that is a property of where
 * this runs rather than a style choice: Ex carries AT_LOCALS (256) Ev slots,
 * about 14 KiB, and Studio's Run executes /bin/as as an ordinary CLI program
 * with a 1 MiB stack (CLI_STACK_PAGES in c/kernel/exec/load/exec.c). Frames on
 * the C stack would put the recursion ceiling at roughly seventy calls and
 * spend it as a #PF with no message; on the heap the ceiling is a number this
 * file chooses and reports.
 *
 * EV_DEPTH_MAX is that number. Infinite recursion is a program bug the
 * evaluator has to survive, because the alternative on this machine is a
 * silent fault inside the editor the program was launched from. */
#define EV_DEPTH_MAX 256

static Ev eval_call(Ex *x, AtNode *n)
{
    Ev out = {0};
    if (n->symbol < 0 || n->symbol >= x->r->p->nfunctions) {
        fail(x, "RuntimeError",
             "guest execution does not implement this call here — run the program on the host");
        return out;
    }
    AtFunction *f = &x->r->p->functions[n->symbol];
    /* The refusals that remain are the ones whose CALLING CONVENTION this does
     * not model, named individually so a refusal still points at a construct
     * rather than at "a call". */
    if (f->method_owner || f->lexical_parent || f->capture_count) {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "guest execution does not implement the call to `%s` here (methods and closures "
                 "need a receiver) — run the program on the host",
                 f->name);
        fail(x, "RuntimeError", buf);
        return out;
    }
    if (n->count != f->nparams || n->count > AT_LOCALS) {
        fail(x, "RuntimeError", "argument count does not match the declaration");
        return out;
    }
    if (x->r->depth >= EV_DEPTH_MAX) {
        fail(x, "RecursionError", "call depth exceeded in guest execution");
        return out;
    }
    Ex *callee = calloc(1, sizeof *callee);
    if (!callee) {
        fail(x, "MemoryError", "call frame");
        return out;
    }
    callee->r = x->r;
    callee->f = f;
    /* "Parameters occupy the first nparams entries" -- AtFunction's own
     * comment in ir/model.h, which is what makes this binding a fact about the
     * checker rather than an assumption. Arguments are evaluated in the
     * CALLER's frame and exported into the callee's, so a string argument read
     * out of a caller local does not point into a slot the callee may
     * overwrite. */
    for (int i = 0; i < n->count && !x->r->failed; i++) {
        callee->locals[i] = ev_export(x, eval_expr(x, n->args[i]));
    }
    if (!x->r->failed) {
        x->r->depth++;
        eval_stmts(callee, f->body);
        x->r->depth--;
        out = ev_export(callee, callee->ret);
        callee->ret.owned = NULL;
    }
    for (int i = 0; i < AT_LOCALS; i++) {
        ev_clear(&callee->locals[i]);
    }
    ev_clear(&callee->ret);
    free(callee);
    return out;
}

/* Store into a local or a global slot, taking ownership of `v`. Returns the
 * slot so a compound assignment can read it back. */
static void ev_store(Ex *x, AtNode *target, Ev v)
{
    Ev *slot = NULL;
    if (target->kind == AN_NAME && target->symbol >= 0 && target->symbol < AT_LOCALS) {
        slot = &x->locals[target->symbol];
    } else if (target->kind == AN_GLOBAL && target->symbol >= 0 && target->symbol < AT_GLOBALS) {
        slot = &x->r->globals[target->symbol];
    }
    if (!slot) {
        ev_clear(&v);
        fail(x, "RuntimeError", "guest execution does not implement this assignment target");
        return;
    }
    ev_clear(slot);
    *slot = v;
}

static int eval_stmts(Ex *x, AtNode *n)
{
    while (n && !x->r->failed) {
        if (n->kind == AN_EXPR) {
            Ev v = eval_expr(x, n->a);
            ev_clear(&v);
        } else if (n->kind == AN_PASS) {
            /* nothing */
        } else if (n->kind == AN_ASSIGN && n->a &&
                   (n->a->kind == AN_NAME || n->a->kind == AN_GLOBAL)) {
            /* `name op= value` is `name = name op value` and nothing more --
             * the checker has already proved the target readable, which is why
             * the read below needs no separate initialization test. Splitting
             * the two spellings apart here would be a second arithmetic
             * implementation for the operators eval_binary already owns. */
            Ev v;
            if (n->op == T_ASSIGN) {
                v = eval_expr(x, n->b);
            } else {
                AtNode compound = *n;
                compound.kind = AN_BINARY;
                compound.op = as_compound_operator(n->op);
                if (compound.op == n->op) {
                    char buf[96];
                    snprintf(buf, sizeof buf,
                             "guest execution does not implement compound `%s` here",
                             op_name(n->op));
                    fail(x, "RuntimeError", buf);
                    return UN_FALL;
                }
                v = eval_binary(x, &compound);
            }
            /* A string read from another local arrives as a borrow
             * (owned == NULL): deep-copy it before it enters the slot, or the
             * next reassignment of the SOURCE slot would free a buffer this
             * slot still points at. ASan-confirmed UAF before this copy
             * (2026-09-16 audit). */
            v = ev_export(x, v);
            if (!x->r->failed) {
                ev_store(x, n->a, v);
            } else {
                ev_clear(&v);
            }
        } else if (n->kind == AN_BREAK) {
            return UN_BREAK;
        } else if (n->kind == AN_CONTINUE) {
            return UN_CONTINUE;
        } else if (n->kind == AN_IF) {
            Ev c = eval_expr(x, n->a);
            int truth = c.kind == EV_BOOL ? (int)c.i : c.kind == EV_INT ? c.i != 0 : 0;
            ev_clear(&c);
            /* PROPAGATE, do not translate: a `break` inside the branch belongs
             * to the enclosing LOOP, and an `if` that answered UN_RETURN for
             * all three would end the function instead. */
            int unwind = eval_stmts(x, truth ? n->b : n->c);
            if (unwind) {
                return unwind;
            }
        } else if (n->kind == AN_WHILE) {
            for (;;) {
                Ev c = eval_expr(x, n->a);
                int truth = c.kind == EV_BOOL ? (int)c.i : c.kind == EV_INT ? c.i != 0 : 0;
                ev_clear(&c);
                if (!truth || x->r->failed) {
                    break;
                }
                int unwind = eval_stmts(x, n->b);
#ifdef AS_EVAL_NO_UNWIND
                /* NEGATIVE CONTROL (tests/aseval.mk): the shape this code had
                 * before break existed -- any non-zero answer ends the
                 * function. `break` then returns from the enclosing function
                 * instead of leaving the loop, and `continue` ends the program.
                 * The gate's break/continue cases must go red against this. */
                if (unwind) {
                    return UN_RETURN;
                }
#else
                if (unwind == UN_BREAK) {
                    break;
                }
                if (unwind == UN_RETURN) {
                    return unwind;
                }
                /* UN_CONTINUE is the loop's to absorb, and absorbing it IS the
                 * next iteration -- there is no separate step to skip. */
#endif
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
            for (int64_t i = start; !x->r->failed && (step > 0 ? i < stop : i > stop); i += step) {
                if (slot >= 0 && slot < AT_LOCALS) {
                    ev_clear(&x->locals[slot]);
                    x->locals[slot] = ev_int(i);
                }
                int unwind = eval_stmts(x, n->c);
#ifdef AS_EVAL_NO_UNWIND
                if (unwind) {
                    return UN_RETURN;
                }
#else
                if (unwind == UN_BREAK) {
                    break;
                }
                if (unwind == UN_RETURN) {
                    return unwind;
                }
#endif
            }
        } else if (n->kind == AN_RETURN) {
            /* The value goes in the FRAME, not straight into the process exit
             * status. It used to go into the status because there was only one
             * frame and it was main's; with user calls, `return n * n` inside
             * sq() would otherwise overwrite the program's exit code with 49.
             * as_typed_eval() takes the status from main's frame alone. */
            if (n->a) {
                ev_clear(&x->ret);
                x->ret = ev_export(x, eval_expr(x, n->a));
            }
            return UN_RETURN;
        } else {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "guest execution does not implement `%s` here — run the program on the host",
                     stmt_name(n));
            fail(x, "RuntimeError", buf);
            return UN_FALL;
        }
        n = n->next;
    }
    return UN_FALL;
}

/* Run one function to completion in a fresh frame. The frame is heap-allocated
 * for eval_call()'s reason: Ex is ~14 KiB and this runs on a 1 MiB stack. */
static int run_function(Run *r, AtFunction *f, Ev *result)
{
    Ex *x = calloc(1, sizeof *x);
    if (!x) {
        fprintf(stderr, "MemoryError: call frame\n");
        r->failed = 1;
        return -1;
    }
    x->r = r;
    x->f = f;
    eval_stmts(x, f->body);
    if (result) {
        *result = ev_export(x, x->ret);
        x->ret.owned = NULL;
    }
    for (int i = 0; i < AT_LOCALS; i++) {
        ev_clear(&x->locals[i]);
    }
    ev_clear(&x->ret);
    free(x);
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
    /* Run is heap-allocated because it carries AT_GLOBALS (1024) Ev slots --
     * about 57 KiB, which is not a thing to put on the stack of a program
     * running with 1 MiB of it. */
    Run *r = calloc(1, sizeof *r);
    if (!r) {
        fprintf(stderr, "MemoryError: program state\n");
        return 1;
    }
    r->p = p;
    /* THE MODULE INITIALIZERS RUN FIRST, and skipping them was not a missing
     * feature so much as a missing HALF of the program: the checker compiles a
     * file's top-level statements into __as_initialize_module, so for a script
     * with no `def` at all, every line the person wrote is in there and main()
     * holds only what followed. Running main alone executed the second half of
     * a program against globals that were never assigned. The order is the
     * checker's (initialization_order), not source order, because imports must
     * initialize before the module that imports them -- the same order
     * emit.c's @global_initialized flags are built from. */
#ifdef AS_EVAL_NO_INIT
    /* NEGATIVE CONTROL (tests/aseval.mk): run main without the module
     * initializers, which is exactly what this function did before. Every
     * program whose variables live at module scope -- the shape a person
     * writes first -- then executes against globals nothing assigned. */
    p->ninitializers = 0;
#endif
    for (int i = 0; i < p->ninitializers && !r->failed; i++) {
        int module = p->initialization_order[i];
        for (int j = 0; j < p->nfunctions; j++) {
            if (p->functions[j].module_initializer && p->functions[j].module == module) {
                run_function(r, &p->functions[j], NULL);
                break;
            }
        }
    }
    Ev status = {0};
    if (!r->failed) {
        run_function(r, &p->functions[main], &status);
    }
    if (status.kind == EV_INT) {
        r->status = status.i;
    }
    ev_clear(&status);
    for (int i = 0; i < AT_GLOBALS; i++) {
        ev_clear(&r->globals[i]);
    }
    int code = r->failed ? 1 : (int)r->status;
    free(r);
    return code;
}
