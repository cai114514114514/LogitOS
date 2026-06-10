#include "as.h"
#include "lexer.h"
#include <stdio.h>      /* snprintf for errors */

/* Single-pass compiler: a Pratt expression parser + statement parser that emits
 * bytecode directly into an ObjFn (clox-style). Top-level names are globals;
 * inside a function, names are lexically-scoped locals living on the value stack
 * (slot 0 = the callee, then params, then block-scoped locals popped at scope end). */

static Token *T;          /* token array from the lexer */
static int    P;          /* current index */
static int    had_error;
static ObjModule *g_module;   /* module being compiled; stamped onto every ObjFn */

typedef struct { const char *name; int len; int depth; int is_captured; } Local;
typedef struct { uint8_t is_local; uint8_t index; } Upvalue;

typedef struct Compiler {
    struct Compiler *enclosing;
    ObjFn *fn;
    Local locals[256];
    int local_count;
    int scope_depth;
    Upvalue upvalues[256];          /* count lives in fn->upvalue_count */
} Compiler;

static Compiler *current;

/* The class being compiled (for `super` resolution); NULL outside a class body.
 * `name`/`namelen` is the class name token (an OP_GET_GLOBAL can re-read the class
 * by name inside a method); `has_super` gates `super`. Single-depth: AetherScript
 * has no nested class declarations. */
typedef struct { const char *name; int namelen; int has_super; int active; } ClassCtx;
static ClassCtx current_class;

/* ---- token cursor ---- */
static Token tk_cur(void)  { return T[P]; }
static Token tk_prev(void) { return T[P - 1]; }
static void advance(void)  { if (T[P].type != T_EOF) P++; }
static int  check(TokType t){ return T[P].type == t; }
static int  match(TokType t){ if (check(t)) { advance(); return 1; } return 0; }

static void error_at(Token tk, const char *msg)
{
    if (had_error) return;
    snprintf(as_err, sizeof as_err, "%s (line %d)", msg, tk.line);
    had_error = 1;
}
static void error(const char *msg) { error_at(T[P], msg); }
static void consume(TokType t, const char *msg) { if (check(t)) advance(); else error(msg); }

/* ---- bytecode emit (into current->fn) ---- */
static void emit(uint8_t b)            { as_chunk_write(current->fn, b); }
static void emit2(uint8_t a, uint8_t b){ emit(a); emit(b); }
/* Emit a 16-bit const-pool index, big-endian (hi then lo) -- matches READ_SHORT
 * in the VM and the emitJump/patchJump byte order. Const-pool operands are now a
 * uniform 2 bytes, so up to 65535 distinct constants per function are addressable. */
static void emit16(int k)              { emit((uint8_t)((k >> 8) & 0xff)); emit((uint8_t)(k & 0xff)); }
static int  makeConst(Value v)
{
    return as_chunk_const(current->fn, v);
}
static void emitConst(Value v) { emit(OP_CONST); emit16(makeConst(v)); }
static int  identConst(const char *name, int len) { return makeConst(OBJ_VAL(as_str_copy(name, len))); }

static int emitJump(uint8_t op) { emit(op); emit(0xff); emit(0xff); return current->fn->count - 2; }
static void patchJump(int off)
{
    int jump = current->fn->count - off - 2;
    if (jump > 0xffff) { error("branch too large"); return; }
    current->fn->code[off] = (uint8_t)((jump >> 8) & 0xff);
    current->fn->code[off + 1] = (uint8_t)(jump & 0xff);
}
static void emitLoop(int loop_start)
{
    emit(OP_LOOP);
    int off = current->fn->count - loop_start + 2;
    if (off > 0xffff) { error("loop body too large"); return; }
    emit((uint8_t)((off >> 8) & 0xff)); emit((uint8_t)(off & 0xff));
}

/* ---- locals / scopes ---- */
static int resolve_local(Compiler *c, const char *name, int len)
{
    for (int i = c->local_count - 1; i >= 0; i--)
        if (c->locals[i].len == len && len > 0 && memcmp(c->locals[i].name, name, len) == 0) return i;
    return -1;
}
static int add_local(const char *name, int len)   /* returns the slot, or -1 on overflow */
{
    if (current->local_count >= 256) { error("too many locals in one function"); return -1; }
    int slot = current->local_count;
    current->locals[slot].name = name;
    current->locals[slot].len = len;
    current->locals[slot].depth = current->scope_depth;
    current->locals[slot].is_captured = 0;
    current->local_count++;
    return slot;
}
static int add_upvalue(Compiler *c, uint8_t index, uint8_t is_local)
{
    int n = c->fn->upvalue_count;
    for (int i = 0; i < n; i++)
        if (c->upvalues[i].index == index && c->upvalues[i].is_local == is_local) return i;
    if (n == 256) { error("too many captured variables in one function"); return 0; }
    c->upvalues[n].is_local = is_local;
    c->upvalues[n].index = index;
    return c->fn->upvalue_count++;
}
/* Resolve `name` as an upvalue of compiler `c`: an enclosing local (is_local=1)
 * or, recursively, an enclosing upvalue (is_local=0). Returns the upvalue index
 * in `c`, or -1 if not found in any enclosing function. */
static int resolve_upvalue(Compiler *c, const char *name, int len)
{
    if (c->enclosing == NULL) return -1;
    int local = resolve_local(c->enclosing, name, len);
    if (local >= 0) {
        c->enclosing->locals[local].is_captured = 1;
        return add_upvalue(c, (uint8_t)local, 1);
    }
    int up = resolve_upvalue(c->enclosing, name, len);
    if (up >= 0) return add_upvalue(c, (uint8_t)up, 0);
    return -1;
}
static void begin_scope(void) { current->scope_depth++; }
static void end_scope(void)
{
    current->scope_depth--;
    while (current->local_count > 0 && current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) emit(OP_CLOSE_UPVALUE);
        else emit(OP_POP);
        current->local_count--;
    }
}

/* ---- loop context (break / continue) ----
 * break jumps forward to the loop end; continue jumps forward to the "step" point
 * (while: re-check condition; for: the index increment). Both must first pop the
 * loop-body locals they skip over -- break pops down to break_base (everything the
 * loop added, since it lands AFTER the loop's end_scope), continue pops to
 * cont_base (only body locals; for's iteration locals stay for the increment). The
 * stack is balanced at the patch targets so it matches the normal-exit path. */
typedef struct {
    int break_base, cont_base;
    int breaks[64], nbreak;     /* forward OP_JUMP offsets -> loop end  */
    int conts[64], ncont;       /* forward OP_JUMP offsets -> step point */
} LoopCtx;
static LoopCtx loop_stack[16];
static int loop_depth = 0;      /* active loops in the CURRENT function */

static void pop_locals_to(int base)   /* emit runtime pops down to `base`; does NOT change local_count */
{
    for (int i = current->local_count - 1; i >= base; i--)
        emit(current->locals[i].is_captured ? OP_CLOSE_UPVALUE : OP_POP);
}
static LoopCtx *loop_begin(int break_base, int cont_base)
{
    if (loop_depth >= 16) { error("loops nested too deep"); return &loop_stack[15]; }
    LoopCtx *L = &loop_stack[loop_depth++];
    L->break_base = break_base; L->cont_base = cont_base; L->nbreak = 0; L->ncont = 0;
    return L;
}
static void loop_patch_conts(LoopCtx *L)  { for (int i = 0; i < L->ncont; i++)  patchJump(L->conts[i]); }
static void loop_patch_breaks(LoopCtx *L) { for (int i = 0; i < L->nbreak; i++) patchJump(L->breaks[i]); }
static void loop_finish(void) { if (loop_depth > 0) loop_depth--; }

/* ---- Pratt parser ---- */
typedef enum {
    PREC_NONE,
    PREC_TERNARY,                                 /* a if c else b (M23; lowest expr level) */
    PREC_OR, PREC_AND, PREC_EQ, PREC_CMP,
    PREC_BOR, PREC_BXOR, PREC_BAND, PREC_SHIFT,   /* | ^ & <<>> (Python order, below +/-) */
    PREC_TERM, PREC_FACTOR, PREC_UNARY,
    PREC_POW,                                     /* ** : right-assoc, binds tighter than unary */
    PREC_CALL, PREC_PRIMARY
} Prec;
typedef void (*ParseFn)(void);
typedef struct { ParseFn prefix, infix; Prec prec; } ParseRule;
static ParseRule *get_rule(TokType t);
static void parse_precedence(Prec prec);
static void expression(void);
static void declaration(void);
static void statement(void);
static void block(void);
static void lambda_(void);
static void class_declaration(void);
static void super_(void);
static void ternary_(void);

static void number(void)
{
    Token t = tk_prev();
    if (t.type == T_INT) {
        int64_t v;
        if (t.len > 2 && t.start[0] == '0' && (t.start[1] == 'x' || t.start[1] == 'X'))
            v = (int64_t)strtoll(t.start, NULL, 16);
        else
            v = (int64_t)strtoll(t.start, NULL, 10);
        emitConst(INT_VAL(v));
    } else {
        emitConst(FLOAT_VAL(strtod(t.start, NULL)));
    }
}

static void literal(void)
{
    switch (tk_prev().type) {
    case T_NIL:   emit(OP_NIL); break;
    case T_TRUE:  emit(OP_TRUE); break;
    case T_FALSE: emit(OP_FALSE); break;
    default: break;
    }
}

static void string(void)
{
    Token t = tk_prev();
    char *buf = (char *)as_malloc((size_t)t.len + 1);
    if (!buf) { error("out of memory"); return; }
    int n = 0;
    for (int i = 0; i < t.len; i++) {
        char c = t.start[i];
        if (c == '\\' && i + 1 < t.len) {
            char e = t.start[++i];
            switch (e) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case '0': c = '\0'; break; case '\\': c = '\\'; break;
            case '"': c = '"';  break; case '\'': c = '\''; break;
            default:  c = e; break;
            }
        }
        buf[n++] = c;
    }
    buf[n] = 0;
    emitConst(OBJ_VAL(as_str_take(buf, n)));
}

static void grouping(void) { expression(); consume(T_RPAREN, "expected ')' after expression"); }

/* M23 f-string: the lexer delivered the raw interior as one T_FSTR token.
 * Lower `f"a{x}b"` to  "a" + str(x) + "b" : text runs become string constants
 * ({{ }} -> literal braces, backslash escapes decoded like string()); each
 * {hole} is re-lexed + compiled as an expression wrapped in the str() builtin.
 * Pieces chain left-to-right with OP_ADD. */
static void fstring(void)
{
    Token t = tk_prev();
    const char *src = t.start;
    int len = t.len, i = 0, piece = 0;

    while (i < len && !had_error) {
        if (src[i] != '{' || (i + 1 < len && src[i + 1] == '{')) {
            /* ---- text run (ends at a hole-opening '{' or the end) ---- */
            char *seg = (char *)as_malloc((size_t)(len - i) + 1);
            if (!seg) { error("out of memory"); return; }
            int n = 0;
            while (i < len) {
                char c = src[i];
                if (c == '{') {
                    if (i + 1 < len && src[i + 1] == '{') { seg[n++] = '{'; i += 2; continue; }
                    break;                                   /* hole begins */
                }
                if (c == '}' && i + 1 < len && src[i + 1] == '}') { seg[n++] = '}'; i += 2; continue; }
                if (c == '\\' && i + 1 < len) {              /* same escapes as string() */
                    char e = src[++i];
                    switch (e) {
                    case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
                    case '0': c = '\0'; break; case '\\': c = '\\'; break;
                    case '"': c = '"';  break; case '\'': c = '\''; break;
                    default:  c = e; break;
                    }
                }
                seg[n++] = c; i++;
            }
            seg[n] = 0;
            emitConst(OBJ_VAL(as_str_take(seg, n)));
            if (piece++) emit(OP_ADD);
            continue;
        }

        /* ---- hole: src[i] == '{' ---- */
        i++;                                                  /* past '{' */
        int hs = i, depth = 0, colon = 0;
        while (i < len) {                                     /* find the matching '}' */
            char c = src[i];
            if (c == '\'' || c == '"') {                      /* skip string literals */
                char q = c; i++;
                while (i < len && src[i] != q) { if (src[i] == '\\' && i + 1 < len) i += 2; else i++; }
                if (i < len) i++;
                continue;
            }
            if (c == '{' || c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') depth--;
            else if (c == '}') { if (depth == 0) break; depth--; }
            else if (c == ':' && depth == 0) colon = 1;
            i++;
        }
        if (i >= len) { error_at(t, "unterminated '{' in f-string"); return; }
        int he = i; i++;                                      /* past '}' */
        if (colon) { error_at(t, "format spec not supported in f-string"); return; }
        while (hs < he && (src[hs] == ' ' || src[hs] == '\t')) hs++;      /* strip (else the */
        while (he > hs && (src[he - 1] == ' ' || src[he - 1] == '\t')) he--; /* re-lex sees INDENT) */
        if (hs == he) { error_at(t, "empty expression in f-string"); return; }

        char *hole = (char *)as_malloc((size_t)(he - hs) + 1);
        if (!hole) { error("out of memory"); return; }
        memcpy(hole, src + hs, (size_t)(he - hs)); hole[he - hs] = 0;

        emit(OP_GET_GLOBAL); emit16(identConst("str", 3));    /* str(<hole>) */
        int hc;
        Token *ht = as_lex(hole, &hc);
        if (!ht) { had_error = 1; free(hole); return; }       /* as_err set by the lexer */
        Token *savedT = T; int savedP = P;
        T = ht; P = 0;
        expression();
        if (!had_error && T[P].type != T_NEWLINE && T[P].type != T_EOF)
            error_at(t, "unexpected text after the f-string expression");
        T = savedT; P = savedP;
        free(ht); free(hole);
        if (had_error) return;
        emit2(OP_CALL, 1);
        if (piece++) emit(OP_ADD);
    }

    if (piece == 0) emitConst(OBJ_VAL(as_str_copy("", 0)));   /* f"" */
}

/* From the cursor (just past '['), find a depth-0 T_FOR before the matching
 * depth-0 ']' -> this is a comprehension; return the T_FOR token index, else -1. */
static int comprehension_ahead(void)
{
    int depth = 0;
    for (int k = P; T[k].type != T_EOF; k++) {
        TokType tt = T[k].type;
        if (tt == T_LBRACKET || tt == T_LPAREN || tt == T_LBRACE) depth++;
        else if (tt == T_RBRACKET) { if (depth == 0) return -1; depth--; }
        else if (tt == T_RPAREN || tt == T_RBRACE) depth--;
        else if (tt == T_FOR && depth == 0) return k;
    }
    return -1;
}

/* M23 list comprehension `[elem for var in iter]` (+ optional trailing `if cond`).
 * Compiled as an IMMEDIATELY-INVOKED zero-arg closure (Python does the same): a
 * comprehension is an EXPRESSION, so at its site the value stack may already hold
 * temporaries (e.g. `print` in `print([...])`) -- hidden slot-indexed locals in
 * the ENCLOSING frame would be offset by those temporaries. A fresh function frame
 * starts with a clean stack, so the slots are exact; outer variables are reached
 * through the normal upvalue capture. The element expression precedes `for` in
 * source but must run per iteration, so we parse out of source order with saved
 * cursors: jump to the `for` clause to set up the loop, then re-enter the element
 * (and guard) text inside the loop body. The loop variable is a local of the
 * closure: it cannot leak. */
static void compile_comprehension(int forP)
{
    int saved_loop_depth = loop_depth;               /* no break/continue across the boundary */
    loop_depth = 0;
    Compiler comp;
    comp.enclosing = current;
    comp.fn = as_fn_new();
    comp.fn->name = as_str_copy("<listcomp>", 10);
    comp.fn->module = g_module;
    comp.local_count = 0;
    comp.scope_depth = 0;
    comp.locals[0].name = ""; comp.locals[0].len = 0;
    comp.locals[0].depth = 0; comp.locals[0].is_captured = 0;
    comp.local_count = 1;                            /* slot 0 = callee */
    current = &comp;

    emit2(OP_MAKE_LIST, 0);                          /* the accumulator */
    int acc = add_local("", 0);

    int elem_P = P;                                  /* element expr starts here */
    P = forP;
    advance();                                       /* consume T_FOR */
    consume(T_IDENT, "expected a loop variable");
    Token var = tk_prev();
    consume(T_IN, "expected 'in' after the loop variable");
    parse_precedence(PREC_OR);                       /* iterable (PREC_OR: leave `if` for the guard) */
    emit(OP_ITER);
    int seq = add_local("", 0);
    emitConst(INT_VAL(0));
    int idx = add_local("", 0);
    emit(OP_NIL);
    int vslot = add_local(var.start, var.len);

    int guard_P = -1;
    if (match(T_IF)) {                               /* skip the guard text to the ']' */
        guard_P = P;
        int d = 0;
        while (!(T[P].type == T_RBRACKET && d == 0) && T[P].type != T_EOF) {
            TokType tt = T[P].type;
            if (tt == T_LBRACKET || tt == T_LPAREN || tt == T_LBRACE) d++;
            else if (tt == T_RBRACKET || tt == T_RPAREN || tt == T_RBRACE) d--;
            P++;
        }
    }
    int after_P = P;                                 /* at the ']' */

    int loop_start = current->fn->count;
    emit2(OP_GET_LOCAL, (uint8_t)idx);               /* idx < len(seq) ? */
    emit2(OP_GET_LOCAL, (uint8_t)seq);
    emit(OP_LEN);
    emit(OP_LT);
    int exit_j = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    emit2(OP_GET_LOCAL, (uint8_t)seq);               /* var = seq[idx] */
    emit2(OP_GET_LOCAL, (uint8_t)idx);
    emit(OP_INDEX_GET);
    emit2(OP_SET_LOCAL, (uint8_t)vslot);
    emit(OP_POP);

    int skip_j = -1;
    if (guard_P >= 0) {                              /* if cond -> skip the append */
        int sP = P; P = guard_P;
        parse_precedence(PREC_OR);
        P = sP;
        skip_j = emitJump(OP_JUMP_IF_FALSE);
        emit(OP_POP);
    }

    emit2(OP_GET_LOCAL, (uint8_t)acc);               /* acc.append(elem) */
    {
        int sP = P; P = elem_P;
        expression();                                /* the element (ternary allowed) */
        if (!had_error && P != forP) error("unexpected text after the comprehension element");
        P = sP;
    }
    emit(OP_INVOKE); emit16(identConst("append", 6)); emit(1);
    emit(OP_POP);                                    /* drop append's nil */

    if (skip_j >= 0) {
        int cont = emitJump(OP_JUMP);
        patchJump(skip_j);
        emit(OP_POP);                                /* drop the false guard value */
        patchJump(cont);
    }

    emit2(OP_GET_LOCAL, (uint8_t)idx);               /* idx += 1 */
    emitConst(INT_VAL(1));
    emit(OP_ADD);
    emit2(OP_SET_LOCAL, (uint8_t)idx);
    emit(OP_POP);
    emitLoop(loop_start);
    patchJump(exit_j);
    emit(OP_POP);                                    /* drop the false loop condition */

    P = after_P;
    consume(T_RBRACKET, "expected ']' after the comprehension");

    emit2(OP_GET_LOCAL, (uint8_t)acc);               /* return the accumulator */
    emit(OP_RET);

    ObjFn *fn = comp.fn;
    current = comp.enclosing;
    loop_depth = saved_loop_depth;
    emit(OP_CLOSURE); emit16(makeConst(OBJ_VAL(fn)));
    for (int i = 0; i < fn->upvalue_count; i++) { emit(comp.upvalues[i].is_local); emit(comp.upvalues[i].index); }
    emit2(OP_CALL, 0);                               /* invoke immediately -> the list */
}

static void list_literal(void)   /* prefix for '[' : a list display or comprehension */
{
    int forP = comprehension_ahead();
    if (forP >= 0) { compile_comprehension(forP); return; }
    int n = 0;
    if (!check(T_RBRACKET)) {
        do { expression(); n++; } while (match(T_COMMA));
    }
    consume(T_RBRACKET, "expected ']' after list elements");
    if (n > 255) { error("list literal too large"); return; }
    emit2(OP_MAKE_LIST, (uint8_t)n);
}

static void dict_literal(void)   /* prefix for '{' : a dict display */
{
    int n = 0;
    if (!check(T_RBRACE)) {
        do {
            expression();                                  /* key */
            consume(T_COLON, "expected ':' between dict key and value");
            expression();                                  /* value */
            n++;
        } while (match(T_COMMA));
    }
    consume(T_RBRACE, "expected '}' after dict entries");
    if (n > 255) { error("dict literal too large"); return; }
    emit2(OP_MAKE_DICT, (uint8_t)n);
}

static void index_(void)   /* infix for '[' : subscript read */
{
    expression();
    consume(T_RBRACKET, "expected ']' after index");
    emit(OP_INDEX_GET);
}

static uint8_t arg_list(void)
{
    uint8_t argc = 0;
    if (!check(T_RPAREN)) {
        do {
            expression();
            if (argc == 255) error("too many arguments");
            argc++;
        } while (match(T_COMMA));
    }
    consume(T_RPAREN, "expected ')' after arguments");
    return argc;
}

static void dot(void)   /* infix for '.' : field/method/module-attr access, or an invoke */
{
    consume(T_IDENT, "expected a name after '.'");
    Token attr = tk_prev();
    int name = identConst(attr.start, attr.len);
    if (match(T_LPAREN)) { uint8_t argc = arg_list(); emit(OP_INVOKE); emit16(name); emit(argc); }
    else { emit(OP_GET_PROPERTY); emit16(name); }
}

static void unary(void)
{
    TokType op = tk_prev().type;
    parse_precedence(PREC_UNARY);
    if (op == T_MINUS) emit(OP_NEG);
    else if (op == T_NOT) emit(OP_NOT);
    else if (op == T_TILDE) emit(OP_BNOT);
}

static void named_variable(Token t)
{
    int slot = resolve_local(current, t.start, t.len);
    if (slot >= 0) { emit2(OP_GET_LOCAL, (uint8_t)slot); return; }
    int up = resolve_upvalue(current, t.start, t.len);
    if (up >= 0) { emit2(OP_GET_UPVALUE, (uint8_t)up); return; }
    emit(OP_GET_GLOBAL); emit16(identConst(t.start, t.len));
}
static void variable(void) { named_variable(tk_prev()); }

static void binary(void)
{
    TokType op = tk_prev().type;
    Prec rp = get_rule(op)->prec;
    parse_precedence((Prec)(op == T_POW ? rp : rp + 1));   /* ** right-assoc; rest left-assoc */
    switch (op) {
    case T_PLUS:    emit(OP_ADD); break;
    case T_MINUS:   emit(OP_SUB); break;
    case T_STAR:    emit(OP_MUL); break;
    case T_SLASH:   emit(OP_DIV); break;
    case T_PERCENT: emit(OP_MOD); break;
    case T_EQ:      emit(OP_EQ); break;
    case T_NE:      emit(OP_NE); break;
    case T_LT:      emit(OP_LT); break;
    case T_LE:      emit(OP_LE); break;
    case T_GT:      emit(OP_GT); break;
    case T_GE:      emit(OP_GE); break;
    case T_AMP:     emit(OP_BAND); break;
    case T_PIPE:    emit(OP_BOR); break;
    case T_CARET:   emit(OP_BXOR); break;
    case T_SHL:     emit(OP_SHL); break;
    case T_SHR:     emit(OP_SHR); break;
    case T_POW:     emit(OP_POW); break;
    default: break;
    }
}

static void and_(void)
{
    int end = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    parse_precedence(PREC_AND);
    patchJump(end);
}
static void or_(void)
{
    int else_j = emitJump(OP_JUMP_IF_FALSE);
    int end = emitJump(OP_JUMP);
    patchJump(else_j);
    emit(OP_POP);
    parse_precedence(PREC_OR);
    patchJump(end);
}
static void in_(void)   /* infix for 'in' : membership test */
{
    parse_precedence((Prec)(get_rule(T_IN)->prec + 1));
    emit(OP_IN);
}
static void call(void) { uint8_t argc = arg_list(); emit2(OP_CALL, argc); }

static ParseRule rules[T_ERROR + 1];
static int rules_ready;
static void init_rules(void)
{
    for (int i = 0; i <= T_ERROR; i++) { rules[i].prefix = 0; rules[i].infix = 0; rules[i].prec = PREC_NONE; }
    rules[T_INT]     = (ParseRule){ number, 0, PREC_NONE };
    rules[T_FLOAT]   = (ParseRule){ number, 0, PREC_NONE };
    rules[T_STR]     = (ParseRule){ string, 0, PREC_NONE };
    rules[T_FSTR]    = (ParseRule){ fstring, 0, PREC_NONE };
    rules[T_IDENT]   = (ParseRule){ variable, 0, PREC_NONE };
    rules[T_NIL]     = (ParseRule){ literal, 0, PREC_NONE };
    rules[T_TRUE]    = (ParseRule){ literal, 0, PREC_NONE };
    rules[T_FALSE]   = (ParseRule){ literal, 0, PREC_NONE };
    rules[T_LPAREN]  = (ParseRule){ grouping, call, PREC_CALL };
    rules[T_LBRACKET]= (ParseRule){ list_literal, index_, PREC_CALL };
    rules[T_LBRACE]  = (ParseRule){ dict_literal, 0, PREC_NONE };
    rules[T_DOT]     = (ParseRule){ 0, dot, PREC_CALL };
    rules[T_MINUS]   = (ParseRule){ unary, binary, PREC_TERM };
    rules[T_PLUS]    = (ParseRule){ 0, binary, PREC_TERM };
    rules[T_STAR]    = (ParseRule){ 0, binary, PREC_FACTOR };
    rules[T_SLASH]   = (ParseRule){ 0, binary, PREC_FACTOR };
    rules[T_PERCENT] = (ParseRule){ 0, binary, PREC_FACTOR };
    rules[T_NOT]     = (ParseRule){ unary, 0, PREC_NONE };
    rules[T_EQ]      = (ParseRule){ 0, binary, PREC_EQ };
    rules[T_NE]      = (ParseRule){ 0, binary, PREC_EQ };
    rules[T_LT]      = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_LE]      = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_GT]      = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_GE]      = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_AND]     = (ParseRule){ 0, and_, PREC_AND };
    rules[T_OR]      = (ParseRule){ 0, or_, PREC_OR };
    rules[T_IN]      = (ParseRule){ 0, in_, PREC_CMP };
    rules[T_PIPE]    = (ParseRule){ 0, binary, PREC_BOR };    /* | */
    rules[T_CARET]   = (ParseRule){ 0, binary, PREC_BXOR };   /* ^ */
    rules[T_AMP]     = (ParseRule){ 0, binary, PREC_BAND };   /* & */
    rules[T_SHL]     = (ParseRule){ 0, binary, PREC_SHIFT };  /* << */
    rules[T_SHR]     = (ParseRule){ 0, binary, PREC_SHIFT };  /* >> */
    rules[T_POW]     = (ParseRule){ 0, binary, PREC_POW };    /* ** (right-assoc, see binary) */
    rules[T_TILDE]   = (ParseRule){ unary, 0, PREC_NONE };    /* ~ */
    rules[T_LAMBDA]  = (ParseRule){ lambda_, 0, PREC_NONE };
    rules[T_SUPER]   = (ParseRule){ super_, 0, PREC_NONE };
    rules[T_IF]      = (ParseRule){ 0, ternary_, PREC_TERNARY };   /* a if c else b */
    rules_ready = 1;
}
static ParseRule *get_rule(TokType t) { return &rules[t]; }

/* Where the expression currently being parsed at each nesting depth STARTED in
 * the code stream -- ternary_ rewinds to this point so the consequent only runs
 * when the condition is true (Python semantics: `a if c else b` must not
 * evaluate `a` eagerly -- `d[k] if k in d else x` would throw). */
static int expr_start[64];
static int expr_depth;

static void parse_precedence(Prec prec)
{
    if (expr_depth < 64) expr_start[expr_depth] = current->fn->count;
    expr_depth++;
    advance();
    ParseFn prefix = get_rule(tk_prev().type)->prefix;
    if (!prefix) { error("expected an expression"); expr_depth--; return; }
    prefix();
    while (!had_error && prec <= get_rule(tk_cur().type)->prec) {
        advance();
        get_rule(tk_prev().type)->infix();
    }
    expr_depth--;
}
static void expression(void) { parse_precedence(PREC_TERNARY); }

/* M23 ternary `a if c else b` -- an infix on T_IF, with PYTHON evaluation order:
 * condition first, then exactly one arm. By the time the infix fires, the
 * consequent's bytecode is already emitted -- so lift it out: rewind the code
 * stream to the expression start (expr_start), compile the condition in its
 * place, and re-append the saved consequent bytes behind the branch. Safe to
 * relocate because jumps are RELATIVE and the block is self-contained (an
 * expression cannot contain break/continue patch targets), so internal offsets
 * survive the move; constant indexes are position-independent. */
static void ternary_(void)
{
    int cstart = (expr_depth > 0 && expr_depth <= 64) ? expr_start[expr_depth - 1]
                                                       : current->fn->count;
    int clen = current->fn->count - cstart;
    uint8_t saved[1024];
    uint8_t *cons = saved;
    if (clen > (int)sizeof saved) {
        cons = (uint8_t *)as_malloc((size_t)clen);
        if (!cons) { error("out of memory"); return; }
    }
    for (int i = 0; i < clen; i++) cons[i] = current->fn->code[cstart + i];
    current->fn->count = cstart;               /* rewind: condition goes here */

    parse_precedence(PREC_OR);                 /* condition -> [c] */
    int else_j = emitJump(OP_JUMP_IF_FALSE);   /* peeks c */
    emit(OP_POP);                               /* true path: drop c */
    for (int i = 0; i < clen; i++) emit(cons[i]);   /* now run the consequent -> [a] */
    if (cons != saved) free(cons);
    int end_j = emitJump(OP_JUMP);
    patchJump(else_j);
    emit(OP_POP);                               /* false path: drop c */
    consume(T_ELSE, "expected 'else' in conditional expression");
    parse_precedence(PREC_TERNARY);             /* alternative -> [b] */
    patchJump(end_j);
}

/* ---- statements ---- */

/* store the value currently on top of the stack into `name` (consumes it) */
static void store_name(Token name)
{
    int slot = resolve_local(current, name.start, name.len);
    if (slot >= 0) { emit2(OP_SET_LOCAL, (uint8_t)slot); emit(OP_POP); return; }
    int up = resolve_upvalue(current, name.start, name.len);
    if (up >= 0) { emit2(OP_SET_UPVALUE, (uint8_t)up); emit(OP_POP); return; }
    if (current->enclosing == NULL) { emit(OP_DEF_GLOBAL); emit16(identConst(name.start, name.len)); }
    else add_local(name.start, name.len);    /* new local: the value becomes its home slot */
}

/* a statement ends at a newline OR ';' (so `a = 1; b = 2` works on one line). */
static void consume_stmt_end(void)
{
    if (match(T_SEMI)) { match(T_NEWLINE); return; }   /* ';' may be the last thing on the line */
    consume(T_NEWLINE, "expected a newline or ';' after the statement");
}

/* compound-assignment token (+= -= *= /= %=) -> the binary opcode, or -1 */
static int compound_op(TokType t)
{
    switch (t) {
    case T_PLUSEQ:    return OP_ADD;
    case T_MINUSEQ:   return OP_SUB;
    case T_STAREQ:    return OP_MUL;
    case T_SLASHEQ:   return OP_DIV;
    case T_PERCENTEQ: return OP_MOD;
    default:          return -1;
    }
}

/* is the upcoming statement `target = ...` or `target OP= ...`? (NAME, then a
 * top-level '=' or compound-assign op before the statement end) */
static int assign_ahead(void)
{
    if (T[P].type != T_IDENT) return 0;
    int depth = 0;
    for (int k = P; T[k].type != T_NEWLINE && T[k].type != T_SEMI && T[k].type != T_EOF; k++) {
        TokType t = T[k].type;
        if (t == T_LPAREN || t == T_LBRACKET) depth++;
        else if (t == T_RPAREN || t == T_RBRACKET) depth--;
        else if (depth == 0 && (t == T_ASSIGN || compound_op(t) >= 0)) return 1;
    }
    return 0;
}

static void assignment(void)
{
    consume(T_IDENT, "expected a name");
    Token name = tk_prev();
    if (check(T_COMMA)) {                          /* M23 multiple assignment / unpack */
        Token targets[64]; int nt = 0; targets[nt++] = name;
        while (match(T_COMMA)) {
            consume(T_IDENT, "expected a name in multiple assignment");
            if (nt >= 64) { error("too many assignment targets"); return; }
            targets[nt++] = tk_prev();
        }
        consume(T_ASSIGN, "expected '=' in multiple assignment");
        /* Inside a function, pre-declare any NEW targets as nil locals FIRST: the
         * stores below run right-to-left (consuming the stacked values top-down),
         * and store_name's new-local path binds the value in place WITHOUT a pop --
         * in reverse order that would bind the slots backwards. Pre-declared, every
         * target takes the uniform SET+POP path. (Top level: DEF_GLOBAL consumes.) */
        if (current->enclosing != NULL)
            for (int i = 0; i < nt; i++)
                if (resolve_local(current, targets[i].start, targets[i].len) < 0 &&
                    resolve_upvalue(current, targets[i].start, targets[i].len) < 0) {
                    emit(OP_NIL);
                    add_local(targets[i].start, targets[i].len);
                }
        expression();                              /* first RHS value */
        if (check(T_COMMA)) {                      /* tuple form: a,b,... = e1,e2,... */
            int nv = 1;
            while (match(T_COMMA)) { expression(); nv++; }
            if (nv != nt) { error("assignment count mismatch"); return; }
            /* values v0..v_{n-1} on the stack, v_{n-1} on top: store last->first */
            for (int i = nt - 1; i >= 0; i--) store_name(targets[i]);
        } else {                                   /* list form: a,b,... = <list> */
            begin_scope();
            int tmp = add_local("", 0);            /* the RHS list value = this slot */
            for (int i = 0; i < nt; i++) {
                emit2(OP_GET_LOCAL, (uint8_t)tmp);
                emitConst(INT_VAL(i));
                emit(OP_INDEX_GET);                /* bounds-checked: too-few -> runtime error */
                store_name(targets[i]);
            }
            end_scope();                           /* pops tmp */
        }
        return;
    }
    /* name.field OP= rhs (compound on a single property -- self.i += 1). The
     * receiver is a bare name, so re-reading it is pure (no dup opcode needed). */
    if (check(T_DOT) && T[P + 1].type == T_IDENT && compound_op(T[P + 2].type) >= 0) {
        advance();                                 /* '.' */
        consume(T_IDENT, "expected a field name after '.'");
        int fk = identConst(tk_prev().start, tk_prev().len);
        int cop = compound_op(tk_cur().type); advance();
        named_variable(name);                      /* recv (for the SET) */
        named_variable(name);                      /* recv (for the GET) */
        emit(OP_GET_PROPERTY); emit16(fk);         /* -> [recv, oldval] */
        expression();                              /* -> [recv, oldval, rhs] */
        emit((uint8_t)cop);                        /* -> [recv, newval] */
        emit(OP_SET_PROPERTY); emit16(fk);         /* -> [newval] */
        emit(OP_POP);
        return;
    }
    if (check(T_DOT) || check(T_LBRACKET)) {       /* general lvalue chain, plain '=':
                                                    * name (.field | [idx])* (.field | [idx]) = v.
                                                    * Every accessor but the LAST emits a GET
                                                    * (descend into the container); the last
                                                    * emits SET_PROPERTY / INDEX_SET. */
        named_variable(name);                      /* base on the stack */
        for (;;) {
            if (match(T_DOT)) {
                consume(T_IDENT, "expected a field name after '.'");
                int fk = identConst(tk_prev().start, tk_prev().len);
                if (check(T_DOT) || check(T_LBRACKET)) { emit(OP_GET_PROPERTY); emit16(fk); continue; }
                consume(T_ASSIGN, "expected '=' in property assignment");
                expression();
                emit(OP_SET_PROPERTY); emit16(fk); /* [recv val] -> val */
                emit(OP_POP);
                return;
            }
            match(T_LBRACKET);                     /* the '[' (guaranteed by the loop entry) */
            expression();                          /* index -> stack */
            consume(T_RBRACKET, "expected ']'");
            if (check(T_DOT) || check(T_LBRACKET)) { emit(OP_INDEX_GET); continue; }
            consume(T_ASSIGN, "expected '=' in indexed assignment");
            expression();
            emit(OP_INDEX_SET);                    /* (obj, idx, val) -> set */
            return;
        }
    }
    {
        int cop = compound_op(tk_cur().type);
        if (cop >= 0) {                            /* name += / -= / *= / /= / %= rhs */
            advance();                             /* consume the compound operator */
            named_variable(name);                  /* push name's current value */
            expression();                          /* rhs */
            emit((uint8_t)cop);                    /* (name) OP (rhs) */
            store_name(name);                      /* store back into name (consumes top) */
        } else {
            consume(T_ASSIGN, "expected '='");
            expression();
            store_name(name);
        }
    }
}

static void if_statement(void)   /* entered after `if` or `elif` */
{
    expression();
    consume(T_COLON, "expected ':' after the condition");
    int then_j = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    block();
    int else_j = emitJump(OP_JUMP);
    patchJump(then_j);
    emit(OP_POP);
    if (match(T_ELIF)) if_statement();
    else if (match(T_ELSE)) { consume(T_COLON, "expected ':' after else"); block(); }
    patchJump(else_j);
}

static void while_statement(void)
{
    LoopCtx *L = loop_begin(current->local_count, current->local_count);
    int loop_start = current->fn->count;
    expression();
    consume(T_COLON, "expected ':' after the condition");
    int exit_j = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    block();
    loop_patch_conts(L);                  /* continue -> re-check the condition */
    emitLoop(loop_start);
    patchJump(exit_j);
    emit(OP_POP);
    loop_patch_breaks(L);                 /* break -> here */
    loop_finish();
}

/* for VAR in SEQ: BODY  -- desugars to an index walk over the sequence/list. */
static void for_statement(void)
{
    int break_base = current->local_count;         /* before the iteration locals */
    begin_scope();
    consume(T_IDENT, "expected a loop variable");
    Token var = tk_prev();
    consume(T_IN, "expected 'in' after the loop variable");
    expression();                                  /* the sequence -> stack */
    emit(OP_ITER);                                 /* dict -> its keys list; others unchanged */
    int seq_slot = add_local("", 0);
    emitConst(INT_VAL(0));                          /* idx = 0 */
    int idx_slot = add_local("", 0);
    int var_slot = -1;
    if (current->enclosing != NULL) { emit(OP_NIL); var_slot = add_local(var.start, var.len); }
    consume(T_COLON, "expected ':' after the sequence");
    int cont_base = current->local_count;          /* after iteration locals, before body */
    LoopCtx *L = loop_begin(break_base, cont_base);

    int loop_start = current->fn->count;
    emit2(OP_GET_LOCAL, (uint8_t)idx_slot);
    emit2(OP_GET_LOCAL, (uint8_t)seq_slot);
    emit(OP_LEN);
    emit(OP_LT);
    int exit_j = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);                                   /* drop the condition (true path) */
    emit2(OP_GET_LOCAL, (uint8_t)seq_slot);
    emit2(OP_GET_LOCAL, (uint8_t)idx_slot);
    emit(OP_INDEX_GET);                             /* seq[idx] */
    if (var_slot >= 0) { emit2(OP_SET_LOCAL, (uint8_t)var_slot); emit(OP_POP); }
    else { emit(OP_DEF_GLOBAL); emit16(identConst(var.start, var.len)); }
    block();
    loop_patch_conts(L);                            /* continue -> the index increment */
    emit2(OP_GET_LOCAL, (uint8_t)idx_slot);         /* idx = idx + 1 */
    emitConst(INT_VAL(1));
    emit(OP_ADD);
    emit2(OP_SET_LOCAL, (uint8_t)idx_slot);
    emit(OP_POP);
    emitLoop(loop_start);
    patchJump(exit_j);
    emit(OP_POP);                                   /* drop the condition (false path) */
    end_scope();
    loop_patch_breaks(L);                           /* break -> here (iteration locals already popped) */
    loop_finish();
}

static void import_statement(void)   /* import NAME */
{
    consume(T_IDENT, "expected a module name after 'import'");
    Token name = tk_prev();
    emit(OP_IMPORT); emit16(identConst(name.start, name.len));     /* -> module obj on stack */
    store_name(name);                                              /* bind NAME = module */
    consume(T_NEWLINE, "expected a newline after import");
}

static void from_statement(void)     /* from NAME import a, b, ... */
{
    consume(T_IDENT, "expected a module name after 'from'");
    Token mod = tk_prev();
    int modk = identConst(mod.start, mod.len);
    consume(T_IMPORT, "expected 'import' after the module name");
    do {
        consume(T_IDENT, "expected a name to import");
        Token nm = tk_prev();
        emit(OP_IMPORT); emit16(modk);                            /* push module (cached) */
        emit(OP_GET_PROPERTY); emit16(identConst(nm.start, nm.len)); /* -> module.nm */
        store_name(nm);                                           /* bind nm */
    } while (match(T_COMMA));
    consume(T_NEWLINE, "expected a newline after import");
}

static void return_statement(void)
{
    if (current->enclosing == NULL) { error("'return' outside a function"); return; }
    if (check(T_NEWLINE)) emit(OP_NIL);
    else expression();
    emit(OP_RET);
    consume(T_NEWLINE, "expected a newline after return");
}

/* Compile a function body into a nested ObjFn and emit OP_CLOSURE (+ upvalue
 * operand bytes) into the enclosing fn, leaving the closure on the stack. For a
 * `def` (is_lambda=0) params are in (...) and the body is a block; for a lambda
 * (is_lambda=1) params run to ':' and the body is one expression. */
/* Compile a function/method body.
 * is_lambda: body is a single expression instead of an indented block.
 * is_method: the function is a class method — `self` (the first declared param)
 *   is placed in local slot 0 (the callee slot) instead of the normal "" placeholder,
 *   and is NOT counted in arity (arity = number of params AFTER self).
 *   This lets call_closure(method, argc) where argc = user-supplied args, with
 *   the receiver already at the callee slot (sp[-argc-1]), work cleanly. */
static void compile_function(const char *name, int namelen, int is_lambda, int is_method)
{
    int saved_loop_depth = loop_depth;   /* a nested function body has its own loops; */
    loop_depth = 0;                       /* break/continue here can't target an outer loop */
    Compiler comp;
    comp.enclosing = current;
    comp.fn = as_fn_new();
    comp.fn->name = as_str_copy(name, namelen);
    comp.fn->module = g_module;
    comp.local_count = 0;
    comp.scope_depth = 0;
    /* slot 0: for regular/lambda functions = callee placeholder (""); for methods = `self`. */
    if (is_method) {
        /* `self` is the FIRST param, bound to slot 0 -- the receiver, which the caller
         * leaves at the callee position (sp[-argc-1] becomes slot 0). self is NOT counted
         * in arity; arity = the params AFTER self, so call_closure(method, argc) matches
         * with argc = user args. (current=&comp BEFORE add_local so post-self params land
         * in this method's compiler, not the enclosing one.) */
        consume(T_LPAREN, "expected '(' after the method name");
        consume(T_IDENT, "expected 'self' as the first method parameter");
        Token s = tk_prev();
        comp.locals[0].name = s.start; comp.locals[0].len = s.len;
        comp.locals[0].depth = 0; comp.locals[0].is_captured = 0;
        comp.local_count = 1;                          /* self = slot 0 */
        current = &comp;
        while (match(T_COMMA)) {
            consume(T_IDENT, "expected a parameter name");
            comp.fn->arity++;
            add_local(tk_prev().start, tk_prev().len);
        }
        consume(T_RPAREN, "expected ')' after method parameters");
        consume(T_COLON, "expected ':' after the parameter list");
        block();
        emit(OP_NIL); emit(OP_RET);                /* implicit `return nil` */
    } else {
        comp.locals[0].name = ""; comp.locals[0].len = 0; comp.locals[0].depth = 0; comp.locals[0].is_captured = 0;
        comp.local_count = 1;                      /* slot 0 = callee */
        current = &comp;

    if (is_lambda) {
        if (!check(T_COLON)) {
            do {
                consume(T_IDENT, "expected a parameter name");
                comp.fn->arity++;
                add_local(tk_prev().start, tk_prev().len);
            } while (match(T_COMMA));
        }
        consume(T_COLON, "expected ':' after lambda parameters");
        expression();                              /* single-expression body */
        emit(OP_RET);
    } else {
        consume(T_LPAREN, "expected '(' after the function name");
        if (!check(T_RPAREN)) {
            do {
                consume(T_IDENT, "expected a parameter name");
                comp.fn->arity++;
                add_local(tk_prev().start, tk_prev().len);
            } while (match(T_COMMA));
        }
        consume(T_RPAREN, "expected ')' after parameters");
        consume(T_COLON, "expected ':' after the parameter list");
        block();
        emit(OP_NIL); emit(OP_RET);                /* implicit `return nil` */
    }
    }

    ObjFn *fn = comp.fn;
    current = comp.enclosing;
    loop_depth = saved_loop_depth;       /* restore the enclosing function's loop context */
    emit(OP_CLOSURE); emit16(makeConst(OBJ_VAL(fn)));
    /* upvalue {is_local,index} pairs stay 1-byte each -- they are slot indices, not const indices. */
    for (int i = 0; i < fn->upvalue_count; i++) { emit(comp.upvalues[i].is_local); emit(comp.upvalues[i].index); }
}

static void fun_declaration(void)
{
    consume(T_IDENT, "expected a function name");
    Token name = tk_prev();
    /* For a nested def, declare the name in the enclosing scope BEFORE compiling the
     * body, so the function can recurse (the body captures its own name as an upvalue).
     * The OP_CLOSURE that compile_function emits pushes the closure into exactly this
     * reserved local slot, so no separate store is needed. Top-level defs stay globals. */
    if (current->enclosing != NULL) add_local(name.start, name.len);
    compile_function(name.start, name.len, 0, 0);  /* not a lambda, not a method */
    if (current->enclosing == NULL) { emit(OP_DEF_GLOBAL); emit16(identConst(name.start, name.len)); }
}

static void lambda_(void)   /* prefix for 'lambda' : an anonymous closure expression */
{
    compile_function("<lambda>", 8, 1, 0);  /* lambda, not a method */
}

/* class NAME [ '(' SUPER ')' ] ':' NEWLINE INDENT (def method)+ DEDENT
 * Emits OP_CLASS (build empty class) -> bind the name -> optional OP_INHERIT
 * (copy-down super's methods) -> per-method: push class, compile the method body
 * as a closure, OP_METHOD to register it, pop the leftover class copy. */
/* `super` . NAME  ->  the superclass's method, bound to the current self (implicit,
 * same as `obj.m()`), so `super.greet()` dispatches to the parent's greet. Resolves
 * at run time by loading the enclosing class (by its bound name) and reading ->super. */
static void super_(void)
{
    if (!current_class.active) { error("'super' used outside a class"); return; }
    if (!current_class.has_super) { error("'super' used in a class with no superclass"); return; }
    consume(T_DOT, "expected '.' after 'super'");
    consume(T_IDENT, "expected a superclass method name after 'super.'");
    int name = identConst(tk_prev().start, tk_prev().len);
    /* Push the enclosing class. Resolve via upvalue-or-global, NOT named_variable:
     * the class is never a local of the *method* itself (it's bound in the scope that
     * contains the class declaration -> a global at top level, an upvalue when nested),
     * so we must skip resolve_local on the current frame. Otherwise a method parameter
     * or local named the same as the class would shadow it and OP_GET_SUPER would read
     * a non-class value. resolve_upvalue starts at current->enclosing, so it does exactly
     * that skip; -1 (top-level class) falls through to the global table. */
    int up = resolve_upvalue(current, current_class.name, current_class.namelen);
    if (up >= 0) emit2(OP_GET_UPVALUE, (uint8_t)up);   /* upvalue index: slot, stays 1-byte */
    else { emit(OP_GET_GLOBAL); emit16(identConst(current_class.name, current_class.namelen)); }
    emit(OP_GET_SUPER); emit16(name);             /* -> the parent method bound to self */
}

static void class_declaration(void)
{
    consume(T_IDENT, "expected a class name");
    Token name = tk_prev();
    int namek = identConst(name.start, name.len);

    emit(OP_CLASS); emit16(namek);                   /* -> new empty class on the stack */
    /* Bind the name immediately so methods can reference the class (and so `super`'s
     * OP_GET_GLOBAL <classname> resolves at method-run time). */
    if (current->enclosing == NULL) { emit(OP_DEF_GLOBAL); emit16(namek); }
    else { add_local(name.start, name.len); }        /* nested: the class lives in this local slot */

    ClassCtx saved = current_class;
    current_class.name = name.start; current_class.namelen = name.len;
    current_class.has_super = 0; current_class.active = 1;

    int has_super = 0;
    if (match(T_LPAREN)) {
        consume(T_IDENT, "expected a superclass name");
        Token sup = tk_prev();
        if (sup.len == name.len && memcmp(sup.start, name.start, name.len) == 0)
            error("a class cannot inherit from itself");
        /* push superclass, then the class, then OP_INHERIT (copy-down) leaving the class. */
        named_variable(sup);                         /* read the superclass value */
        named_variable(name);                        /* read the class we just bound */
        emit(OP_INHERIT);                            /* [.. super class] -> [.. class] */
        consume(T_RPAREN, "expected ')' after the superclass name");
        has_super = 1; current_class.has_super = 1;
    } else {
        named_variable(name);                        /* leave the class on the stack for methods */
    }

    consume(T_COLON, "expected ':' after the class header");
    consume(T_NEWLINE, "expected a newline before the class body");
    consume(T_INDENT, "expected an indented class body");
    while (!check(T_DEDENT) && !check(T_EOF) && !had_error) {
        consume(T_DEF, "class body may only contain method definitions");
        consume(T_IDENT, "expected a method name");
        Token m = tk_prev();
        int mk = identConst(m.start, m.len);
        compile_function(m.start, m.len, 0, 1);      /* a class method: bind `self` to slot 0 */
        emit(OP_METHOD); emit16(mk);                 /* [.. class closure] -> [.. class] */
    }
    consume(T_DEDENT, "expected the class body to end (dedent)");
    emit(OP_POP);                                    /* pop the working copy of the class */
    (void)has_super;
    current_class = saved;
}

static void block(void)   /* NEWLINE INDENT declaration+ DEDENT */
{
    consume(T_NEWLINE, "expected a newline before the block");
    consume(T_INDENT, "expected an indented block");
    begin_scope();
    while (!check(T_DEDENT) && !check(T_EOF) && !had_error) declaration();
    end_scope();
    consume(T_DEDENT, "expected the block to end (dedent)");
}

static void raise_statement(void)   /* entered after `raise` */
{
    expression();                                   /* the value to throw */
    emit(OP_RAISE);
    consume(T_NEWLINE, "expected a newline after 'raise'");
}

static void try_statement(void)   /* entered after `try` */
{
    consume(T_COLON, "expected ':' after 'try'");
    int setup = emitJump(OP_SETUP_TRY);   /* operand = forward offset to the except block */
    block();                              /* try body in its own scope (locals popped at block end) */
    int done = emitJump(OP_POP_TRY);      /* body ok: pop handler, jump past the except block */
    patchJump(setup);                     /* SETUP_TRY -> here = start of the except block */

    consume(T_EXCEPT, "expected 'except' after the 'try' block");
    /* At runtime the thrown value sits on the stack at handler->sp (i.e. the top). */
    int bound = 0;
    if (check(T_IDENT)) {                 /* except NAME:  -> bind the value to a local */
        Token name = tk_cur(); advance();
        begin_scope();
        add_local(name.start, name.len);  /* the value already occupies this new slot */
        bound = 1;
    } else {
        emit(OP_POP);                     /* except:  -> discard the value */
    }
    consume(T_COLON, "expected ':' after 'except'");
    block();                              /* the handler body */
    if (bound) end_scope();              /* pop the bound exception local (OP_POP/OP_CLOSE_UPVALUE) */

    patchJump(done);                      /* POP_TRY -> here = past the whole try/except */
}

static void break_statement(void)
{
    if (loop_depth == 0) { error("'break' outside a loop"); consume(T_NEWLINE, "newline"); return; }
    LoopCtx *L = &loop_stack[loop_depth - 1];
    pop_locals_to(L->break_base);
    if (L->nbreak < 64) L->breaks[L->nbreak++] = emitJump(OP_JUMP);
    consume_stmt_end();
}
static void continue_statement(void)
{
    if (loop_depth == 0) { error("'continue' outside a loop"); consume(T_NEWLINE, "newline"); return; }
    LoopCtx *L = &loop_stack[loop_depth - 1];
    pop_locals_to(L->cont_base);
    if (L->ncont < 64) L->conts[L->ncont++] = emitJump(OP_JUMP);
    consume_stmt_end();
}

static void statement(void)
{
    if (match(T_IF)) if_statement();
    else if (match(T_WHILE)) while_statement();
    else if (match(T_FOR)) for_statement();
    else if (match(T_BREAK)) break_statement();
    else if (match(T_CONTINUE)) continue_statement();
    else if (match(T_RETURN)) return_statement();
    else if (match(T_IMPORT)) import_statement();
    else if (match(T_FROM)) from_statement();
    else if (match(T_TRY)) try_statement();
    else if (match(T_RAISE)) raise_statement();
    else if (assign_ahead()) { assignment(); consume_stmt_end(); }
    else { expression(); emit(OP_POP); consume_stmt_end(); }
}

static void declaration(void)
{
    if (match(T_DEF)) fun_declaration();
    else if (match(T_CLASS)) class_declaration();
    else statement();
}

ObjFn *as_compile_module(const char *src, ObjModule *module)
{
    int count;
    Token *toks = as_lex(src, &count);     /* lexer allocates only the raw token array, no GC objects */
    if (!toks) return NULL;                 /* as_err set by the lexer */
    as_gc_push_disable();                  /* compiler-built objects aren't VM-rooted until run/stored */
    if (!rules_ready) init_rules();

    g_module = module;
    T = toks; P = 0; had_error = 0; g_oom = 0;
    Compiler comp;
    comp.enclosing = NULL;
    comp.fn = as_fn_new();
    comp.fn->name = NULL;                    /* the top-level script */
    comp.fn->module = module;
    comp.local_count = 0;
    comp.scope_depth = 0;
    comp.locals[comp.local_count].name = ""; comp.locals[comp.local_count].len = 0; comp.locals[comp.local_count].depth = 0; comp.locals[comp.local_count].is_captured = 0;
    comp.local_count++;
    current = &comp;

    while (!check(T_EOF) && !had_error) {
        if (match(T_NEWLINE)) continue;     /* tolerate stray blank lines */
        declaration();
    }
    emit(OP_NIL); emit(OP_RET);

    ObjFn *script = comp.fn;
    free(toks);
    as_gc_pop_disable();
    /* g_oom: a realloc failed mid-emit -> the bytecode is incomplete; reject the
     * compile (the partial ObjFn is reclaimed by as_free_objects at run end). */
    return (had_error || g_oom) ? NULL : script;
}

ObjFn *as_compile(const char *src)   /* standalone: compile into a fresh __main__ module */
{
    /* Disable GC before allocating the module: as_compile_module enables it
     * at the top of its own body. Without this guard, a stress-mode GC fires
     * during as_module_new with a stale (freed) value stack and crashes. */
    as_gc_push_disable();
    ObjModule *m = as_module_new("__main__", 8);
    as_gc_pop_disable();
    return as_compile_module(src, m);
}
