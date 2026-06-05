#include "aqs.h"
#include "lexer.h"
#include <stdio.h>      /* snprintf for errors */

/* Single-pass compiler: a Pratt expression parser + statement parser that emits
 * bytecode directly into an ObjFn (clox-style). Top-level assignments are
 * globals; assignments inside a function body are function-scoped locals that
 * live on the value stack (slot 0 = the callee, then params, then locals). */

static Token *T;          /* token array from the lexer */
static int    P;          /* current index */
static int    had_error;

typedef struct { const char *name; int len; } Local;

typedef struct Compiler {
    struct Compiler *enclosing;
    ObjFn *fn;
    Local locals[256];
    int local_count;
    int block_depth;       /* nesting of {} blocks within this function (body == 1) */
} Compiler;

static Compiler *current;

/* ---- token cursor ---- */
static Token tk_cur(void)  { return T[P]; }
static Token tk_prev(void) { return T[P - 1]; }
static Token tk_next(void) { return T[P + 1]; }
static void advance(void)  { if (T[P].type != T_EOF) P++; }
static int  check(TokType t){ return T[P].type == t; }
static int  match(TokType t){ if (check(t)) { advance(); return 1; } return 0; }

static void error_at(Token tk, const char *msg)
{
    if (had_error) return;
    snprintf(aqs_err, sizeof aqs_err, "%s (line %d)", msg, tk.line);
    had_error = 1;
}
static void error(const char *msg) { error_at(T[P], msg); }
static void consume(TokType t, const char *msg) { if (check(t)) advance(); else error(msg); }

/* ---- bytecode emit (into current->fn) ---- */
static void emit(uint8_t b)            { aqs_chunk_write(current->fn, b); }
static void emit2(uint8_t a, uint8_t b){ emit(a); emit(b); }
static int  makeConst(Value v)
{
    int k = aqs_chunk_const(current->fn, v);
    if (k > 255) { error("too many constants in one function"); return 0; }
    return k;
}
static void emitConst(Value v) { emit2(OP_CONST, (uint8_t)makeConst(v)); }
static int  identConst(const char *name, int len) { return makeConst(OBJ_VAL(aqs_str_copy(name, len))); }

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

/* ---- locals ---- */
static int resolve_local(Compiler *c, const char *name, int len)
{
    for (int i = c->local_count - 1; i >= 0; i--)
        if (c->locals[i].len == len && memcmp(c->locals[i].name, name, len) == 0) return i;
    return -1;
}
static void add_local(const char *name, int len)
{
    if (current->local_count >= 256) { error("too many locals in one function"); return; }
    current->locals[current->local_count].name = name;
    current->locals[current->local_count].len = len;
    current->local_count++;
}

/* ---- Pratt parser ---- */
typedef enum {
    PREC_NONE, PREC_OR, PREC_AND, PREC_EQ, PREC_CMP,
    PREC_TERM, PREC_FACTOR, PREC_UNARY, PREC_CALL, PREC_PRIMARY
} Prec;
typedef void (*ParseFn)(void);
typedef struct { ParseFn prefix, infix; Prec prec; } ParseRule;
static ParseRule *get_rule(TokType t);
static void parse_precedence(Prec prec);
static void expression(void);
static void declaration(void);
static void statement(void);
static void block(void);

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
    char *buf = (char *)malloc((size_t)t.len + 1);
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
    emitConst(OBJ_VAL(aqs_str_take(buf, n)));
}

static void grouping(void) { expression(); consume(T_RPAREN, "expected ')' after expression"); }

static void unary(void)
{
    TokType op = tk_prev().type;
    parse_precedence(PREC_UNARY);
    if (op == T_MINUS) emit(OP_NEG);
    else if (op == T_NOT) emit(OP_NOT);
}

static void variable(void)
{
    Token t = tk_prev();
    int slot = resolve_local(current, t.start, t.len);
    if (slot >= 0) emit2(OP_GET_LOCAL, (uint8_t)slot);
    else emit2(OP_GET_GLOBAL, (uint8_t)identConst(t.start, t.len));
}

static void binary(void)
{
    TokType op = tk_prev().type;
    parse_precedence((Prec)(get_rule(op)->prec + 1));   /* left-associative */
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
    default: break;
    }
}

static void and_(void)   /* short-circuit: if left is falsey, keep it and skip */
{
    int end = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    parse_precedence(PREC_AND);
    patchJump(end);
}
static void or_(void)    /* short-circuit: if left is truthy, keep it and skip */
{
    int else_j = emitJump(OP_JUMP_IF_FALSE);
    int end = emitJump(OP_JUMP);
    patchJump(else_j);
    emit(OP_POP);
    parse_precedence(PREC_OR);
    patchJump(end);
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
static void call(void) { uint8_t argc = arg_list(); emit2(OP_CALL, argc); }

static ParseRule rules[T_ERROR + 1];
static int rules_ready;
static void init_rules(void)
{
    for (int i = 0; i <= T_ERROR; i++) { rules[i].prefix = 0; rules[i].infix = 0; rules[i].prec = PREC_NONE; }
    rules[T_INT]    = (ParseRule){ number, 0, PREC_NONE };
    rules[T_FLOAT]  = (ParseRule){ number, 0, PREC_NONE };
    rules[T_STR]    = (ParseRule){ string, 0, PREC_NONE };
    rules[T_IDENT]  = (ParseRule){ variable, 0, PREC_NONE };
    rules[T_NIL]    = (ParseRule){ literal, 0, PREC_NONE };
    rules[T_TRUE]   = (ParseRule){ literal, 0, PREC_NONE };
    rules[T_FALSE]  = (ParseRule){ literal, 0, PREC_NONE };
    rules[T_LPAREN] = (ParseRule){ grouping, call, PREC_CALL };
    rules[T_MINUS]  = (ParseRule){ unary, binary, PREC_TERM };
    rules[T_PLUS]   = (ParseRule){ 0, binary, PREC_TERM };
    rules[T_STAR]   = (ParseRule){ 0, binary, PREC_FACTOR };
    rules[T_SLASH]  = (ParseRule){ 0, binary, PREC_FACTOR };
    rules[T_PERCENT]= (ParseRule){ 0, binary, PREC_FACTOR };
    rules[T_NOT]    = (ParseRule){ unary, 0, PREC_NONE };
    rules[T_EQ]     = (ParseRule){ 0, binary, PREC_EQ };
    rules[T_NE]     = (ParseRule){ 0, binary, PREC_EQ };
    rules[T_LT]     = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_LE]     = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_GT]     = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_GE]     = (ParseRule){ 0, binary, PREC_CMP };
    rules[T_AND]    = (ParseRule){ 0, and_, PREC_AND };
    rules[T_OR]     = (ParseRule){ 0, or_, PREC_OR };
    rules_ready = 1;
}
static ParseRule *get_rule(TokType t) { return &rules[t]; }

static void parse_precedence(Prec prec)
{
    advance();
    ParseFn prefix = get_rule(tk_prev().type)->prefix;
    if (!prefix) { error("expected an expression"); return; }
    prefix();
    while (!had_error && prec <= get_rule(tk_cur().type)->prec) {
        advance();
        get_rule(tk_prev().type)->infix();
    }
}
static void expression(void) { parse_precedence(PREC_OR); }

/* ---- statements ---- */
static void simple_statement(void)
{
    if (check(T_IDENT) && tk_next().type == T_ASSIGN) {
        Token name = tk_cur();
        advance(); advance();             /* IDENT '=' */
        expression();
        if (current->enclosing == NULL) {            /* top level -> global */
            emit2(OP_DEF_GLOBAL, (uint8_t)identConst(name.start, name.len));
        } else {
            int slot = resolve_local(current, name.start, name.len);
            if (slot >= 0) { emit2(OP_SET_LOCAL, (uint8_t)slot); emit(OP_POP); }
            else {
                if (current->block_depth > 1)        /* would allocate a slot conditionally */
                    error("a new variable must be first assigned at the function's top level (A1)");
                add_local(name.start, name.len);     /* the expr value is its home slot */
            }
        }
    } else {
        expression();
        emit(OP_POP);
    }
    consume(T_NEWLINE, "expected a newline after the statement");
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
    int loop_start = current->fn->count;
    expression();
    consume(T_COLON, "expected ':' after the condition");
    int exit_j = emitJump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    block();
    emitLoop(loop_start);
    patchJump(exit_j);
    emit(OP_POP);
}

static void return_statement(void)
{
    if (current->enclosing == NULL) { error("'return' outside a function"); return; }
    if (check(T_NEWLINE)) emit(OP_NIL);
    else expression();
    emit(OP_RET);
    consume(T_NEWLINE, "expected a newline after return");
}

static void fun_declaration(void)
{
    consume(T_IDENT, "expected a function name");
    Token name = tk_prev();

    Compiler comp;
    comp.enclosing = current;
    comp.fn = aqs_fn_new();
    comp.fn->name = aqs_str_copy(name.start, name.len);
    comp.local_count = 0;
    comp.block_depth = 0;
    comp.locals[comp.local_count].name = ""; comp.locals[comp.local_count].len = 0; comp.local_count++; /* slot 0 = callee */
    current = &comp;

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
    emit(OP_NIL); emit(OP_RET);          /* implicit `return nil` */

    ObjFn *fn = comp.fn;
    current = comp.enclosing;
    emitConst(OBJ_VAL(fn));
    if (current->enclosing == NULL) {
        emit2(OP_DEF_GLOBAL, (uint8_t)identConst(name.start, name.len));
    } else {
        if (current->block_depth > 1) error("nested functions must be defined at the function's top level (A1)");
        add_local(name.start, name.len);
    }
}

static void block(void)   /* NEWLINE INDENT declaration+ DEDENT */
{
    consume(T_NEWLINE, "expected a newline before the block");
    consume(T_INDENT, "expected an indented block");
    current->block_depth++;
    while (!check(T_DEDENT) && !check(T_EOF) && !had_error) declaration();
    current->block_depth--;
    consume(T_DEDENT, "expected the block to end (dedent)");
}

static void statement(void)
{
    if (match(T_IF)) if_statement();
    else if (match(T_WHILE)) while_statement();
    else if (match(T_RETURN)) return_statement();
    else simple_statement();
}

static void declaration(void)
{
    if (match(T_DEF)) fun_declaration();
    else statement();
}

ObjFn *aqs_compile(const char *src)
{
    int count;
    Token *toks = aqs_lex(src, &count);
    if (!toks) return NULL;                 /* aqs_err set by the lexer */
    if (!rules_ready) init_rules();

    T = toks; P = 0; had_error = 0;
    Compiler comp;
    comp.enclosing = NULL;
    comp.fn = aqs_fn_new();
    comp.fn->name = NULL;                    /* the top-level script */
    comp.local_count = 0;
    comp.block_depth = 0;
    comp.locals[comp.local_count].name = ""; comp.locals[comp.local_count].len = 0; comp.local_count++;
    current = &comp;

    while (!check(T_EOF) && !had_error) {
        if (match(T_NEWLINE)) continue;     /* tolerate stray blank lines */
        declaration();
    }
    emit(OP_NIL); emit(OP_RET);

    ObjFn *script = comp.fn;
    free(toks);
    return had_error ? NULL : script;
}
