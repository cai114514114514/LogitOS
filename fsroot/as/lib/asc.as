# asc -- the AetherScript compiler, in AetherScript (M21-P3 self-hosting, S2/S3).
# A faithful translation of c/apps/as/compiler.c: a single-pass Pratt parser that
# emits the SAME bytecode the C compiler does, serialized to the SAME .la format
# (LAQ1 + AS_BC_VERSION) the C VM loads. VM stays C; this is the source->bytecode
# step -- the real meaning of self-hosting.
#
#   from asc import compile_file
#   compile_file("in.as", "out.la")     # -> nil, or raises a string on error
#
# Bytecode is built as a list of ints; constants as [tag, value] pairs (the
# language has no type(), so the compiler tags every constant as it adds it).
# A function-in-progress is a dict {arity, upcount, name, code, consts}.

from aslex import lex

# ---- opcodes (mirror as.h OpCode enum, in order) ----
OP_CONST = 0
OP_NIL = 1
OP_TRUE = 2
OP_FALSE = 3
OP_POP = 4
OP_GET_LOCAL = 5
OP_SET_LOCAL = 6
OP_GET_GLOBAL = 7
OP_SET_GLOBAL = 8
OP_DEF_GLOBAL = 9
OP_ADD = 10
OP_SUB = 11
OP_MUL = 12
OP_DIV = 13
OP_MOD = 14
OP_NEG = 15
OP_EQ = 16
OP_NE = 17
OP_LT = 18
OP_LE = 19
OP_GT = 20
OP_GE = 21
OP_NOT = 22
OP_JUMP = 23
OP_JUMP_IF_FALSE = 24
OP_LOOP = 25
OP_CALL = 26
OP_RET = 27
OP_MAKE_LIST = 28
OP_INDEX_GET = 29
OP_INDEX_SET = 30
OP_LEN = 31
OP_INVOKE = 32
OP_GET_ATTR = 33
OP_IMPORT = 34
OP_MAKE_DICT = 35
OP_IN = 36
OP_ITER = 37
OP_CLOSURE = 38
OP_GET_UPVALUE = 39
OP_SET_UPVALUE = 40
OP_CLOSE_UPVALUE = 41
OP_CLASS = 42
OP_INHERIT = 43
OP_METHOD = 44
OP_GET_PROPERTY = 45
OP_SET_PROPERTY = 46
OP_GET_SUPER = 47
OP_SETUP_TRY = 48
OP_POP_TRY = 49
OP_RAISE = 50
OP_BAND = 51
OP_BOR = 52
OP_BXOR = 53
OP_BNOT = 54
OP_SHL = 55
OP_SHR = 56
OP_POW = 57
# M27 ports -- appended, never inserted: the order IS the .la ABI.
OP_PIPE = 58
OP_REDIR_OUT = 59
OP_REDIR_IN = 60
OP_WITH_BEGIN = 61
OP_WITH_END = 62
# M28: the one opcode the capability milestone adds (locked at one; see
# c/apps/as/as.h's OpCode enum comment). OP__COUNT (a C-only dispatch-table
# sentinel added alongside it in as.h) is NOT mirrored here on purpose: it
# carries no wire-format meaning and asc.as never dispatches bytecode, only
# emits it -- gen_as_opcodes.py --check knows to skip it for that reason.
OP_SLICE = 63

AS_BC_VERSION = 5

# constant tags (mirror as_bc.c K_*)
K_NIL = 0
K_BOOL = 1
K_INT = 2
K_FLOAT = 3
K_STR = 4
K_FN = 5

# token types (must match aslex.as / lexer.h)
T_NEWLINE = 0
T_INDENT = 1
T_DEDENT = 2
T_INT = 3
T_FLOAT = 4
T_STR = 5
T_IDENT = 6
T_DEF = 7
T_RETURN = 8
T_IF = 9
T_ELIF = 10
T_ELSE = 11
T_CLASS = 12
T_SUPER = 13
T_TRY = 14
T_EXCEPT = 15
T_RAISE = 16
T_WHILE = 17
T_FOR = 18
T_IN = 19
T_AND = 20
T_OR = 21
T_NOT = 22
T_LAMBDA = 23
T_IMPORT = 24
T_FROM = 25
T_TRUE = 26
T_FALSE = 27
T_NIL = 28
T_LPAREN = 29
T_RPAREN = 30
T_LBRACKET = 31
T_RBRACKET = 32
T_LBRACE = 33
T_RBRACE = 34
T_COMMA = 35
T_COLON = 36
T_DOT = 37
T_PLUS = 38
T_MINUS = 39
T_STAR = 40
T_SLASH = 41
T_PERCENT = 42
T_ASSIGN = 43
T_EQ = 44
T_NE = 45
T_LT = 46
T_LE = 47
T_GT = 48
T_GE = 49
T_BREAK = 50
T_CONTINUE = 51
T_AMP = 52
T_PIPE = 53
T_CARET = 54
T_TILDE = 55
T_SHL = 56
T_SHR = 57
T_POW = 58
T_PLUSEQ = 59
T_MINUSEQ = 60
T_STAREQ = 61
T_SLASHEQ = 62
T_PERCENTEQ = 63
T_SEMI = 64
T_FSTR = 65
T_PIPEOP = 66
T_ARROW = 67
T_LARROW = 68
T_WITH = 69
T_EOF = 70

# precedence levels (mirror compiler.c Prec enum)
P_NONE = 0
P_TERNARY = 1
P_PIPE = 2          # M27: |>  ->  <-   (binds looser than everything but ternary)
P_OR = 3
P_AND = 4
P_EQ = 5
P_CMP = 6
P_BOR = 7
P_BXOR = 8
P_BAND = 9
P_SHIFT = 10
P_TERM = 11
P_FACTOR = 12
P_UNARY = 13
P_POW = 14
P_CALL = 15
P_PRIMARY = 16

# ---- little-endian byte emitters into a list of ints ----
def u32(out, v):
    out.append(v & 255)
    out.append((v >> 8) & 255)
    out.append((v >> 16) & 255)
    out.append((v >> 24) & 255)

def i64(out, v):
    for k in range(8):
        out.append((v >> (8 * k)) & 255)

def put_str(out, s):
    u32(out, len(s))
    for k in range(len(s)):
        out.append(ord(s[k]))

# A compiled function (dict). code = list of ints; consts = list of [tag, value].
def new_fn(name, arity):
    return {"arity": arity, "upcount": 0, "name": name, "code": [], "consts": [],
            "upvals": []}

# ---- serialize one fn-dict tree to .la bytes (mirrors as_bc.c dump_fn) ----
def dump_fn(out, fn):
    u32(out, fn["arity"])
    u32(out, fn["upcount"])
    put_str(out, fn["name"])
    code = fn["code"]
    u32(out, len(code))
    for b in code:
        out.append(b & 255)
    consts = fn["consts"]
    u32(out, len(consts))
    for kv in consts:
        tag = kv[0]
        v = kv[1]
        out.append(tag)
        if tag == K_NIL:
            _ = 0
        elif tag == K_BOOL:
            out.append(1 if v else 0)
        elif tag == K_INT:
            i64(out, v)
        elif tag == K_FLOAT:
            i64(out, f64bits(v))
        elif tag == K_STR:
            put_str(out, v)
        elif tag == K_FN:
            dump_fn(out, v)

def dump_module(fn):
    out = []
    for ch in "LAQ1":
        out.append(ord(ch))
    u32(out, AS_BC_VERSION)
    dump_fn(out, fn)
    parts = []
    for b in out:
        parts.append(chr(b & 255))
    return "".join(parts)

# ---- per-function compile state (mirrors struct Compiler) ----
class Comp:
    def init(self, enclosing, name, arity):
        self.enclosing = enclosing
        self.fn = new_fn(name, arity)
        self.locals = []          # [name, depth, captured]
        self.scope_depth = 0
        self.upvals = []          # [is_local, index]

# ---- the parser/compiler (mirrors compiler.c; all state on self) ----
class Parser:
    def init(self, toks):
        self.toks = toks
        self.p = 0
        self.cur = nil
        self.cls_active = false
        self.cls_name = ""
        self.cls_has_super = false
        self.loops = []           # {bbase, cbase, breaks, conts}
        self.expr_starts = []
        self.rules = {}
        r = self.rules
        r[T_INT] = [self.number, nil, P_NONE]
        r[T_FLOAT] = [self.number, nil, P_NONE]
        r[T_STR] = [self.string_, nil, P_NONE]
        r[T_FSTR] = [self.fstring, nil, P_NONE]
        r[T_IDENT] = [self.variable, nil, P_NONE]
        r[T_NIL] = [self.literal, nil, P_NONE]
        r[T_TRUE] = [self.literal, nil, P_NONE]
        r[T_FALSE] = [self.literal, nil, P_NONE]
        r[T_LPAREN] = [self.grouping, self.call_, P_CALL]
        r[T_LBRACKET] = [self.list_literal, self.index_, P_CALL]
        r[T_LBRACE] = [self.dict_literal, nil, P_NONE]
        r[T_DOT] = [nil, self.dot, P_CALL]
        r[T_MINUS] = [self.unary, self.binary, P_TERM]
        r[T_PLUS] = [nil, self.binary, P_TERM]
        r[T_STAR] = [nil, self.binary, P_FACTOR]
        r[T_SLASH] = [nil, self.binary, P_FACTOR]
        r[T_PERCENT] = [nil, self.binary, P_FACTOR]
        r[T_NOT] = [self.unary, nil, P_NONE]
        r[T_EQ] = [nil, self.binary, P_EQ]
        r[T_NE] = [nil, self.binary, P_EQ]
        r[T_LT] = [nil, self.binary, P_CMP]
        r[T_LE] = [nil, self.binary, P_CMP]
        r[T_GT] = [nil, self.binary, P_CMP]
        r[T_GE] = [nil, self.binary, P_CMP]
        r[T_AND] = [nil, self.and_, P_AND]
        r[T_OR] = [nil, self.or_, P_OR]
        r[T_IN] = [nil, self.in_, P_CMP]
        r[T_PIPE] = [nil, self.binary, P_BOR]
        r[T_CARET] = [nil, self.binary, P_BXOR]
        r[T_AMP] = [nil, self.binary, P_BAND]
        r[T_SHL] = [nil, self.binary, P_SHIFT]
        r[T_SHR] = [nil, self.binary, P_SHIFT]
        r[T_POW] = [nil, self.binary, P_POW]
        r[T_TILDE] = [self.unary, nil, P_NONE]
        r[T_LAMBDA] = [self.lambda_, nil, P_NONE]
        r[T_SUPER] = [self.super_, nil, P_NONE]
        r[T_IF] = [nil, self.ternary_, P_TERNARY]
        r[T_PIPEOP] = [nil, self.pipeline_, P_PIPE]
        r[T_ARROW] = [nil, self.pipeline_, P_PIPE]
        r[T_LARROW] = [nil, self.pipeline_, P_PIPE]

    # ---- cursor ----
    def cur_t(self):
        return self.toks[self.p]
    def prev_t(self):
        return self.toks[self.p - 1]
    def advance(self):
        if self.toks[self.p][0] != T_EOF:
            self.p += 1
    def check(self, t):
        return self.toks[self.p][0] == t
    def match(self, t):
        if self.check(t):
            self.advance()
            return true
        return false
    def err(self, msg):
        raise f"{msg} (line {self.toks[self.p][2]})"
    def consume(self, t, msg):
        if self.check(t):
            self.advance()
        else:
            self.err(msg)

    # ---- emit ----
    def emit(self, b):
        self.cur.fn["code"].append(b)
    def emit2(self, a, b):
        self.emit(a)
        self.emit(b)
    def emit16(self, k):
        self.emit((k >> 8) & 255)
        self.emit(k & 255)
    def code_len(self):
        return len(self.cur.fn["code"])

    def add_const(self, tag, v):
        consts = self.cur.fn["consts"]
        if tag != K_FN:
            for i in range(len(consts)):
                if consts[i][0] == tag and consts[i][1] == v:
                    return i
        consts.append([tag, v])
        return len(consts) - 1

    def emit_const(self, tag, v):
        self.emit(OP_CONST)
        self.emit16(self.add_const(tag, v))

    def ident_const(self, name):
        return self.add_const(K_STR, name)

    def emit_jump(self, op):
        self.emit(op)
        self.emit(255)
        self.emit(255)
        return self.code_len() - 2

    def patch_jump(self, off):
        jump = self.code_len() - off - 2
        if jump > 0xFFFF:
            self.err("branch too large")
        code = self.cur.fn["code"]
        code[off] = (jump >> 8) & 255
        code[off + 1] = jump & 255

    def emit_loop(self, loop_start):
        self.emit(OP_LOOP)
        off = self.code_len() - loop_start + 2
        if off > 0xFFFF:
            self.err("loop body too large")
        self.emit((off >> 8) & 255)
        self.emit(off & 255)

    # ---- locals / scopes ----
    def resolve_local(self, c, name):
        i = len(c.locals) - 1
        while i >= 0:
            if len(c.locals[i][0]) > 0 and c.locals[i][0] == name:
                return i
            i -= 1
        return -1

    def add_local(self, name):
        if len(self.cur.locals) >= 256:
            self.err("too many locals in one function")
        self.cur.locals.append([name, self.cur.scope_depth, false, false])
        return len(self.cur.locals) - 1

    def add_upvalue(self, c, index, is_local):
        ups = c.upvals
        for i in range(len(ups)):
            if ups[i][1] == index and ups[i][0] == is_local:
                return i
        if len(ups) == 256:
            self.err("too many captured variables in one function")
        ups.append([is_local, index])
        c.fn["upcount"] = len(ups)
        return len(ups) - 1

    def resolve_upvalue(self, c, name):
        if c.enclosing == nil:
            return -1
        loc = self.resolve_local(c.enclosing, name)
        if loc >= 0:
            c.enclosing.locals[loc][2] = true
            return self.add_upvalue(c, loc, true)
        up = self.resolve_upvalue(c.enclosing, name)
        if up >= 0:
            return self.add_upvalue(c, up, false)
        return -1

    def begin_scope(self):
        self.cur.scope_depth = self.cur.scope_depth + 1

    def end_scope(self):
        self.cur.scope_depth = self.cur.scope_depth - 1
        while len(self.cur.locals) > 0 and self.cur.locals[len(self.cur.locals) - 1][1] > self.cur.scope_depth:
            if self.cur.locals[len(self.cur.locals) - 1][3]:
                self.emit(OP_WITH_END)
            if self.cur.locals[len(self.cur.locals) - 1][2]:
                self.emit(OP_CLOSE_UPVALUE)
            else:
                self.emit(OP_POP)
            self.drop_local()

    def drop_local(self):
        cut = []
        for k in range(len(self.cur.locals) - 1):
            cut.append(self.cur.locals[k])
        self.cur.locals = cut

    def pop_locals_to(self, base):
        i = len(self.cur.locals) - 1
        while i >= base:
            if self.cur.locals[i][3]:
                self.emit(OP_WITH_END)
            if self.cur.locals[i][2]:
                self.emit(OP_CLOSE_UPVALUE)
            else:
                self.emit(OP_POP)
            i -= 1

    # ---- Pratt driver ----
    def rule_of(self, t):
        return self.rules[t] if t in self.rules else [nil, nil, P_NONE]

    def parse_prec(self, prec):
        self.expr_starts.append(self.code_len())
        self.advance()
        rule = self.rule_of(self.prev_t()[0])
        if rule[0] == nil:
            self.err("expected an expression")
        pf = rule[0]
        pf()
        while self.rule_of(self.cur_t()[0])[2] >= prec and self.rule_of(self.cur_t()[0])[2] != P_NONE:
            self.advance()
            inf = self.rule_of(self.prev_t()[0])[1]
            inf()
        self.drop_expr_start()

    def drop_expr_start(self):
        cut = []
        for k in range(len(self.expr_starts) - 1):
            cut.append(self.expr_starts[k])
        self.expr_starts = cut

    def expression(self):
        self.parse_prec(P_TERNARY)

    # ---- expression atoms ----
    def number(self):
        t = self.prev_t()
        txt = t[1]
        if t[0] == T_INT:
            v = 0
            if len(txt) > 2 and txt[0] == "0" and (txt[1] == "x" or txt[1] == "X"):
                for k in range(2, len(txt)):
                    c = ord(txt[k])
                    d = c - 48 if c <= 57 else (c - 87 if c >= 97 else c - 55)
                    v = v * 16 + d
            else:
                for k in range(len(txt)):
                    v = v * 10 + (ord(txt[k]) - 48)
            self.emit_const(K_INT, v)
        else:
            self.emit_const(K_FLOAT, parse_float(txt))

    def decode_escapes(self, raw):
        out = []
        i = 0
        n = len(raw)
        while i < n:
            c = raw[i]
            if c == "\\" and i + 1 < n:
                e = raw[i + 1]
                i += 2
                if e == "n":
                    out.append("\n")
                elif e == "t":
                    out.append("\t")
                elif e == "r":
                    out.append("\r")
                elif e == "0":
                    out.append(chr(0))
                else:
                    out.append(e)       # \\ \" \' and unknown -> the char itself
            else:
                out.append(c)
                i += 1
        return "".join(out)

    def string_(self):
        self.emit_const(K_STR, self.decode_escapes(self.prev_t()[1]))

    def literal(self):
        t = self.prev_t()[0]
        if t == T_NIL:
            self.emit(OP_NIL)
        elif t == T_TRUE:
            self.emit(OP_TRUE)
        else:
            self.emit(OP_FALSE)

    def fstring(self):
        raw = self.prev_t()[1]
        n = len(raw)
        i = 0
        piece = 0
        while i < n:
            two = raw.sub(i, i + 2)
            if raw[i] != "{" or two == "{{":
                seg = []
                while i < n:
                    c = raw[i]
                    two = raw.sub(i, i + 2)
                    if c == "{":
                        if two == "{{":
                            seg.append("{")
                            i += 2
                            continue
                        break
                    if two == "}}":
                        seg.append("}")
                        i += 2
                        continue
                    if c == "\\" and i + 1 < n:
                        e = raw[i + 1]
                        i += 2
                        if e == "n":
                            seg.append("\n")
                        elif e == "t":
                            seg.append("\t")
                        elif e == "r":
                            seg.append("\r")
                        elif e == "0":
                            seg.append(chr(0))
                        else:
                            seg.append(e)
                        continue
                    seg.append(c)
                    i += 1
                self.emit_const(K_STR, "".join(seg))
                if piece > 0:
                    self.emit(OP_ADD)
                piece += 1
                continue
            # hole
            i += 1
            hs = i
            depth = 0
            colon = false
            while i < n:
                c = raw[i]
                if c == "'" or c == "\"":
                    q = c
                    i += 1
                    while i < n and raw[i] != q:
                        i = i + 2 if raw[i] == "\\" and i + 1 < n else i + 1
                    if i < n:
                        i += 1
                    continue
                if c == "{" or c == "(" or c == "[":
                    depth += 1
                elif c == ")" or c == "]":
                    depth -= 1
                elif c == "}":
                    if depth == 0:
                        break
                    depth -= 1
                elif c == ":" and depth == 0:
                    colon = true
                i += 1
            if i >= n:
                self.err("unterminated '{' in f-string")
            he = i
            i += 1
            if colon:
                self.err("format spec not supported in f-string")
            while hs < he and (raw[hs] == " " or raw[hs] == "\t"):
                hs += 1
            while he > hs and (raw[he - 1] == " " or raw[he - 1] == "\t"):
                he -= 1
            if hs == he:
                self.err("empty expression in f-string")
            hole = raw.sub(hs, he)
            self.emit(OP_GET_GLOBAL)
            self.emit16(self.ident_const("str"))
            saved_toks = self.toks
            saved_p = self.p
            self.toks = lex(hole)
            self.p = 0
            self.expression()
            if self.toks[self.p][0] != T_NEWLINE and self.toks[self.p][0] != T_EOF:
                self.toks = saved_toks
                self.p = saved_p
                self.err("unexpected text after the f-string expression")
            self.toks = saved_toks
            self.p = saved_p
            self.emit2(OP_CALL, 1)
            if piece > 0:
                self.emit(OP_ADD)
            piece += 1
        if piece == 0:
            self.emit_const(K_STR, "")

    def grouping(self):
        self.expression()
        self.consume(T_RPAREN, "expected ')' after expression")

    def comprehension_ahead(self):
        depth = 0
        k = self.p
        while self.toks[k][0] != T_EOF:
            tt = self.toks[k][0]
            if tt == T_LBRACKET or tt == T_LPAREN or tt == T_LBRACE:
                depth += 1
            elif tt == T_RBRACKET:
                if depth == 0:
                    return -1
                depth -= 1
            elif tt == T_RPAREN or tt == T_RBRACE:
                depth -= 1
            elif tt == T_FOR and depth == 0:
                return k
            k += 1
        return -1

    def list_literal(self):
        forp = self.comprehension_ahead()
        if forp >= 0:
            self.compile_comprehension(forp)
            return nil
        n = 0
        if not self.check(T_RBRACKET):
            self.expression()
            n += 1
            while self.match(T_COMMA):
                self.expression()
                n += 1
        self.consume(T_RBRACKET, "expected ']' after list elements")
        if n > 255:
            self.err("list literal too large")
        self.emit2(OP_MAKE_LIST, n)

    def compile_comprehension(self, forp):
        comp = Comp(self.cur, "<listcomp>", 0)
        comp.locals.append(["", 0, false, false])
        self.cur = comp
        self.emit2(OP_MAKE_LIST, 0)
        acc = self.add_local("")
        elem_p = self.p
        self.p = forp
        self.advance()
        self.consume(T_IDENT, "expected a loop variable")
        var = self.prev_t()
        self.consume(T_IN, "expected 'in' after the loop variable")
        self.parse_prec(P_OR)
        self.emit(OP_ITER)
        seq = self.add_local("")
        self.emit_const(K_INT, 0)
        idx = self.add_local("")
        self.emit(OP_NIL)
        vslot = self.add_local(var[1])
        guard_p = -1
        if self.match(T_IF):
            guard_p = self.p
            d = 0
            while not (self.toks[self.p][0] == T_RBRACKET and d == 0) and self.toks[self.p][0] != T_EOF:
                tt = self.toks[self.p][0]
                if tt == T_LBRACKET or tt == T_LPAREN or tt == T_LBRACE:
                    d += 1
                elif tt == T_RBRACKET or tt == T_RPAREN or tt == T_RBRACE:
                    d -= 1
                self.p += 1
        after_p = self.p
        loop_start = self.code_len()
        self.emit2(OP_GET_LOCAL, idx)
        self.emit2(OP_GET_LOCAL, seq)
        self.emit(OP_LEN)
        self.emit(OP_LT)
        exit_j = self.emit_jump(OP_JUMP_IF_FALSE)
        self.emit(OP_POP)
        self.emit2(OP_GET_LOCAL, seq)
        self.emit2(OP_GET_LOCAL, idx)
        self.emit(OP_INDEX_GET)
        self.emit2(OP_SET_LOCAL, vslot)
        self.emit(OP_POP)
        skip_j = -1
        if guard_p >= 0:
            sp = self.p
            self.p = guard_p
            self.parse_prec(P_OR)
            self.p = sp
            skip_j = self.emit_jump(OP_JUMP_IF_FALSE)
            self.emit(OP_POP)
        self.emit2(OP_GET_LOCAL, acc)
        sp = self.p
        self.p = elem_p
        self.expression()
        if self.p != forp:
            self.err("unexpected text after the comprehension element")
        self.p = sp
        self.emit(OP_INVOKE)
        self.emit16(self.ident_const("append"))
        self.emit(1)
        self.emit(OP_POP)
        if skip_j >= 0:
            cont = self.emit_jump(OP_JUMP)
            self.patch_jump(skip_j)
            self.emit(OP_POP)
            self.patch_jump(cont)
        self.emit2(OP_GET_LOCAL, idx)
        self.emit_const(K_INT, 1)
        self.emit(OP_ADD)
        self.emit2(OP_SET_LOCAL, idx)
        self.emit(OP_POP)
        self.emit_loop(loop_start)
        self.patch_jump(exit_j)
        self.emit(OP_POP)
        self.p = after_p
        self.consume(T_RBRACKET, "expected ']' after the comprehension")
        self.emit2(OP_GET_LOCAL, acc)
        self.emit(OP_RET)
        fn = comp.fn
        ups = comp.upvals
        self.cur = comp.enclosing
        self.emit(OP_CLOSURE)
        self.emit16(self.add_const(K_FN, fn))
        for u in ups:
            self.emit(1 if u[0] else 0)
            self.emit(u[1])
        self.emit2(OP_CALL, 0)

    def dict_literal(self):
        n = 0
        if not self.check(T_RBRACE):
            self.expression()
            self.consume(T_COLON, "expected ':' between dict key and value")
            self.expression()
            n += 1
            while self.match(T_COMMA):
                self.expression()
                self.consume(T_COLON, "expected ':' between dict key and value")
                self.expression()
                n += 1
        self.consume(T_RBRACE, "expected '}' after dict entries")
        if n > 255:
            self.err("dict literal too large")
        self.emit2(OP_MAKE_DICT, n)

    # M28: mirrors compiler.c's bracket_subscript -- read that comment for why
    # the colon inside '[' ... ']' needs no lexer state (T_COLON's other five
    # meanings never occur while parsing a '[' body). Written once so both
    # bracket sites share it; only index_ below actually emits OP_SLICE in
    # M28 (spec D3: slice ASSIGNMENT is a later milestone), the lvalue chain
    # calls it only to detect and reject one with a clear error.
    def bracket_subscript(self):
        self.expression()
        if self.match(T_COLON):
            self.expression()
            return true
        return false

    def index_(self):
        is_slice = self.bracket_subscript()
        self.consume(T_RBRACKET, "expected ']' after index")
        if is_slice:
            self.emit(OP_SLICE)
        else:
            self.emit(OP_INDEX_GET)

    def arg_list(self):
        argc = 0
        if not self.check(T_RPAREN):
            self.expression()
            argc += 1
            while self.match(T_COMMA):
                self.expression()
                if argc == 255:
                    self.err("too many arguments")
                argc += 1
        self.consume(T_RPAREN, "expected ')' after arguments")
        return argc

    def dot(self):
        self.consume(T_IDENT, "expected a name after '.'")
        attr = self.prev_t()
        name = self.ident_const(attr[1])
        if self.match(T_LPAREN):
            argc = self.arg_list()
            self.emit(OP_INVOKE)
            self.emit16(name)
            self.emit(argc)
        else:
            self.emit(OP_GET_PROPERTY)
            self.emit16(name)

    def call_(self):
        argc = self.arg_list()
        self.emit2(OP_CALL, argc)

    def unary(self):
        op = self.prev_t()[0]
        self.parse_prec(P_UNARY)
        if op == T_MINUS:
            self.emit(OP_NEG)
        elif op == T_NOT:
            self.emit(OP_NOT)
        elif op == T_TILDE:
            self.emit(OP_BNOT)

    # M27: a |> b, p -> path, p <- path. Left-associative; see compiler.c
    # pipeline_() for why these are operators and not calls.
    def pipeline_(self):
        op = self.prev_t()[0]
        self.parse_prec(P_PIPE + 1)
        if op == T_PIPEOP:
            self.emit(OP_PIPE)
        elif op == T_ARROW:
            self.emit(OP_REDIR_OUT)
        else:
            self.emit(OP_REDIR_IN)

    def binary(self):
        op = self.prev_t()[0]
        rp = self.rule_of(op)[2]
        self.parse_prec(rp if op == T_POW else rp + 1)
        if op == T_PLUS:
            self.emit(OP_ADD)
        elif op == T_MINUS:
            self.emit(OP_SUB)
        elif op == T_STAR:
            self.emit(OP_MUL)
        elif op == T_SLASH:
            self.emit(OP_DIV)
        elif op == T_PERCENT:
            self.emit(OP_MOD)
        elif op == T_EQ:
            self.emit(OP_EQ)
        elif op == T_NE:
            self.emit(OP_NE)
        elif op == T_LT:
            self.emit(OP_LT)
        elif op == T_LE:
            self.emit(OP_LE)
        elif op == T_GT:
            self.emit(OP_GT)
        elif op == T_GE:
            self.emit(OP_GE)
        elif op == T_AMP:
            self.emit(OP_BAND)
        elif op == T_PIPE:
            self.emit(OP_BOR)
        elif op == T_CARET:
            self.emit(OP_BXOR)
        elif op == T_SHL:
            self.emit(OP_SHL)
        elif op == T_SHR:
            self.emit(OP_SHR)
        elif op == T_POW:
            self.emit(OP_POW)

    def and_(self):
        end = self.emit_jump(OP_JUMP_IF_FALSE)
        self.emit(OP_POP)
        self.parse_prec(P_AND)
        self.patch_jump(end)

    def or_(self):
        else_j = self.emit_jump(OP_JUMP_IF_FALSE)
        end = self.emit_jump(OP_JUMP)
        self.patch_jump(else_j)
        self.emit(OP_POP)
        self.parse_prec(P_OR)
        self.patch_jump(end)

    def in_(self):
        self.parse_prec(P_CMP + 1)
        self.emit(OP_IN)

    def ternary_(self):
        cstart = self.expr_starts[len(self.expr_starts) - 1]
        code = self.cur.fn["code"]
        cons = []
        for k in range(cstart, len(code)):
            cons.append(code[k])
        cut = []
        for k in range(cstart):
            cut.append(code[k])
        self.cur.fn["code"] = cut
        self.parse_prec(P_OR)
        else_j = self.emit_jump(OP_JUMP_IF_FALSE)
        self.emit(OP_POP)
        for b in cons:
            self.emit(b)
        end_j = self.emit_jump(OP_JUMP)
        self.patch_jump(else_j)
        self.emit(OP_POP)
        self.consume(T_ELSE, "expected 'else' in conditional expression")
        self.parse_prec(P_TERNARY)
        self.patch_jump(end_j)

    def named_variable(self, name):
        slot = self.resolve_local(self.cur, name)
        if slot >= 0:
            self.emit2(OP_GET_LOCAL, slot)
            return nil
        up = self.resolve_upvalue(self.cur, name)
        if up >= 0:
            self.emit2(OP_GET_UPVALUE, up)
            return nil
        self.emit(OP_GET_GLOBAL)
        self.emit16(self.ident_const(name))

    def variable(self):
        self.named_variable(self.prev_t()[1])

    # ---- statements ----
    def store_name(self, name):
        slot = self.resolve_local(self.cur, name)
        if slot >= 0:
            self.emit2(OP_SET_LOCAL, slot)
            self.emit(OP_POP)
            return nil
        up = self.resolve_upvalue(self.cur, name)
        if up >= 0:
            self.emit2(OP_SET_UPVALUE, up)
            self.emit(OP_POP)
            return nil
        if self.cur.enclosing == nil:
            self.emit(OP_DEF_GLOBAL)
            self.emit16(self.ident_const(name))
        else:
            self.add_local(name)

    def consume_stmt_end(self):
        if self.match(T_SEMI):
            self.match(T_NEWLINE)
            return nil
        self.consume(T_NEWLINE, "expected a newline or ';' after the statement")

    def compound_op(self, t):
        if t == T_PLUSEQ:
            return OP_ADD
        if t == T_MINUSEQ:
            return OP_SUB
        if t == T_STAREQ:
            return OP_MUL
        if t == T_SLASHEQ:
            return OP_DIV
        if t == T_PERCENTEQ:
            return OP_MOD
        return -1

    def assign_ahead(self):
        if self.toks[self.p][0] != T_IDENT:
            return false
        depth = 0
        k = self.p
        while true:
            t = self.toks[k][0]
            if t == T_NEWLINE or t == T_SEMI or t == T_EOF:
                return false
            if t == T_LPAREN or t == T_LBRACKET:
                depth += 1
            elif t == T_RPAREN or t == T_RBRACKET:
                depth -= 1
            elif depth == 0 and (t == T_ASSIGN or self.compound_op(t) >= 0):
                return true
            k += 1

    def assignment(self):
        self.consume(T_IDENT, "expected a name")
        name = self.prev_t()[1]
        if self.check(T_COMMA):                  # multiple assignment / unpack
            targets = [name]
            while self.match(T_COMMA):
                self.consume(T_IDENT, "expected a name in multiple assignment")
                if len(targets) >= 64:
                    self.err("too many assignment targets")
                targets.append(self.prev_t()[1])
            self.consume(T_ASSIGN, "expected '=' in multiple assignment")
            if self.cur.enclosing != nil:
                for tn in targets:
                    if self.resolve_local(self.cur, tn) < 0 and self.resolve_upvalue(self.cur, tn) < 0:
                        self.emit(OP_NIL)
                        self.add_local(tn)
            self.expression()
            if self.check(T_COMMA):              # tuple form
                nv = 1
                while self.match(T_COMMA):
                    self.expression()
                    nv += 1
                if nv != len(targets):
                    self.err("assignment count mismatch")
                i = len(targets) - 1
                while i >= 0:
                    self.store_name(targets[i])
                    i -= 1
            else:                                 # list form
                self.begin_scope()
                tmp = self.add_local("")
                for i in range(len(targets)):
                    self.emit2(OP_GET_LOCAL, tmp)
                    self.emit_const(K_INT, i)
                    self.emit(OP_INDEX_GET)
                    self.store_name(targets[i])
                self.end_scope()
            return nil
        # name.field OP= rhs (compound on a single property -- self.i += 1)
        if self.check(T_DOT) and self.toks[self.p + 1][0] == T_IDENT and self.compound_op(self.toks[self.p + 2][0]) >= 0:
            self.advance()
            self.consume(T_IDENT, "expected a field name after '.'")
            fk = self.ident_const(self.prev_t()[1])
            cop = self.compound_op(self.cur_t()[0])
            self.advance()
            self.named_variable(name)
            self.named_variable(name)
            self.emit(OP_GET_PROPERTY)
            self.emit16(fk)
            self.expression()
            self.emit(cop)
            self.emit(OP_SET_PROPERTY)
            self.emit16(fk)
            self.emit(OP_POP)
            return nil
        # general lvalue chain, plain '=' : name (.field|[idx])* (.field|[idx]) = v
        if self.check(T_DOT) or self.check(T_LBRACKET):
            self.named_variable(name)
            while true:
                if self.match(T_DOT):
                    self.consume(T_IDENT, "expected a field name after '.'")
                    fk = self.ident_const(self.prev_t()[1])
                    if self.check(T_DOT) or self.check(T_LBRACKET):
                        self.emit(OP_GET_PROPERTY)
                        self.emit16(fk)
                    else:
                        self.consume(T_ASSIGN, "expected '=' in property assignment")
                        self.expression()
                        self.emit(OP_SET_PROPERTY)
                        self.emit16(fk)
                        self.emit(OP_POP)
                        return nil
                else:
                    self.match(T_LBRACKET)
                    is_slice = self.bracket_subscript()
                    self.consume(T_RBRACKET, "expected ']'")
                    # M28 spec D3: slice assignment is not in this milestone --
                    # reject it here rather than let a colon desync OP_INDEX_SET
                    # (which expects exactly one index value under `val`) or
                    # fall through to a misleading "expected ']'".
                    if is_slice:
                        self.err("slice assignment is not supported (r[a:b] is read-only)")
                    if self.check(T_DOT) or self.check(T_LBRACKET):
                        self.emit(OP_INDEX_GET)
                    else:
                        self.consume(T_ASSIGN, "expected '=' in indexed assignment")
                        self.expression()
                        self.emit(OP_INDEX_SET)
                        return nil
        cop = self.compound_op(self.cur_t()[0])
        if cop >= 0:
            self.advance()
            self.named_variable(name)
            self.expression()
            self.emit(cop)
            self.store_name(name)
        else:
            self.consume(T_ASSIGN, "expected '='")
            self.expression()
            self.store_name(name)

    def if_statement(self):
        self.expression()
        self.consume(T_COLON, "expected ':' after the condition")
        then_j = self.emit_jump(OP_JUMP_IF_FALSE)
        self.emit(OP_POP)
        self.block()
        else_j = self.emit_jump(OP_JUMP)
        self.patch_jump(then_j)
        self.emit(OP_POP)
        if self.match(T_ELIF):
            self.if_statement()
        elif self.match(T_ELSE):
            self.consume(T_COLON, "expected ':' after else")
            self.block()
        self.patch_jump(else_j)

    def loop_begin(self, bbase, cbase):
        L = {"bbase": bbase, "cbase": cbase, "breaks": [], "conts": []}
        self.loops.append(L)
        return L

    def loop_finish(self):
        cut = []
        for k in range(len(self.loops) - 1):
            cut.append(self.loops[k])
        self.loops = cut

    def while_statement(self):
        L = self.loop_begin(len(self.cur.locals), len(self.cur.locals))
        loop_start = self.code_len()
        self.expression()
        self.consume(T_COLON, "expected ':' after the condition")
        exit_j = self.emit_jump(OP_JUMP_IF_FALSE)
        self.emit(OP_POP)
        self.block()
        for off in L["conts"]:
            self.patch_jump(off)
        self.emit_loop(loop_start)
        self.patch_jump(exit_j)
        self.emit(OP_POP)
        for off in L["breaks"]:
            self.patch_jump(off)
        self.loop_finish()

    def for_statement(self):
        break_base = len(self.cur.locals)
        self.begin_scope()
        self.consume(T_IDENT, "expected a loop variable")
        var = self.prev_t()[1]
        self.consume(T_IN, "expected 'in' after the loop variable")
        self.expression()
        self.emit(OP_ITER)
        seq_slot = self.add_local("")
        self.emit_const(K_INT, 0)
        idx_slot = self.add_local("")
        var_slot = -1
        if self.cur.enclosing != nil:
            self.emit(OP_NIL)
            var_slot = self.add_local(var)
        self.consume(T_COLON, "expected ':' after the sequence")
        L = self.loop_begin(break_base, len(self.cur.locals))
        loop_start = self.code_len()
        self.emit2(OP_GET_LOCAL, idx_slot)
        self.emit2(OP_GET_LOCAL, seq_slot)
        self.emit(OP_LEN)
        self.emit(OP_LT)
        exit_j = self.emit_jump(OP_JUMP_IF_FALSE)
        self.emit(OP_POP)
        self.emit2(OP_GET_LOCAL, seq_slot)
        self.emit2(OP_GET_LOCAL, idx_slot)
        self.emit(OP_INDEX_GET)
        if var_slot >= 0:
            self.emit2(OP_SET_LOCAL, var_slot)
            self.emit(OP_POP)
        else:
            self.emit(OP_DEF_GLOBAL)
            self.emit16(self.ident_const(var))
        self.block()
        for off in L["conts"]:
            self.patch_jump(off)
        self.emit2(OP_GET_LOCAL, idx_slot)
        self.emit_const(K_INT, 1)
        self.emit(OP_ADD)
        self.emit2(OP_SET_LOCAL, idx_slot)
        self.emit(OP_POP)
        self.emit_loop(loop_start)
        self.patch_jump(exit_j)
        self.emit(OP_POP)
        self.end_scope()
        for off in L["breaks"]:
            self.patch_jump(off)
        self.loop_finish()

    def import_statement(self):
        self.consume(T_IDENT, "expected a module name after 'import'")
        name = self.prev_t()[1]
        self.emit(OP_IMPORT)
        self.emit16(self.ident_const(name))
        self.store_name(name)
        self.consume(T_NEWLINE, "expected a newline after import")

    def from_statement(self):
        self.consume(T_IDENT, "expected a module name after 'from'")
        modk = self.ident_const(self.prev_t()[1])
        self.consume(T_IMPORT, "expected 'import' after the module name")
        while true:
            self.consume(T_IDENT, "expected a name to import")
            nm = self.prev_t()[1]
            self.emit(OP_IMPORT)
            self.emit16(modk)
            self.emit(OP_GET_PROPERTY)
            self.emit16(self.ident_const(nm))
            self.store_name(nm)
            if not self.match(T_COMMA):
                break
        self.consume(T_NEWLINE, "expected a newline after import")

    def return_statement(self):
        if self.cur.enclosing == nil:
            self.err("'return' outside a function")
        if self.check(T_NEWLINE):
            self.emit(OP_NIL)
        else:
            self.expression()
        self.emit(OP_RET)
        self.consume(T_NEWLINE, "expected a newline after return")

    def raise_statement(self):
        self.expression()
        self.emit(OP_RAISE)
        self.consume(T_NEWLINE, "expected a newline after 'raise'")

    def try_statement(self):
        self.consume(T_COLON, "expected ':' after 'try'")
        setup = self.emit_jump(OP_SETUP_TRY)
        self.block()
        done = self.emit_jump(OP_POP_TRY)
        self.patch_jump(setup)
        self.consume(T_EXCEPT, "expected 'except' after the 'try' block")
        bound = false
        if self.check(T_IDENT):
            nm = self.cur_t()[1]
            self.advance()
            self.begin_scope()
            self.add_local(nm)
            bound = true
        else:
            self.emit(OP_POP)
        self.consume(T_COLON, "expected ':' after 'except'")
        self.block()
        if bound:
            self.end_scope()
        self.patch_jump(done)

    def break_statement(self):
        if len(self.loops) == 0:
            self.err("'break' outside a loop")
        L = self.loops[len(self.loops) - 1]
        self.pop_locals_to(L["bbase"])
        L["breaks"].append(self.emit_jump(OP_JUMP))
        self.consume_stmt_end()

    def continue_statement(self):
        if len(self.loops) == 0:
            self.err("'continue' outside a loop")
        L = self.loops[len(self.loops) - 1]
        self.pop_locals_to(L["cbase"])
        L["conts"].append(self.emit_jump(OP_JUMP))
        self.consume_stmt_end()

    def block(self):
        self.consume(T_NEWLINE, "expected a newline before the block")
        self.consume(T_INDENT, "expected an indented block")
        self.begin_scope()
        while not self.check(T_DEDENT) and not self.check(T_EOF):
            self.declaration()
        self.end_scope()
        self.consume(T_DEDENT, "expected the block to end (dedent)")

    # ---- functions / classes ----
    def bump_arity(self, comp):
        f = comp.fn
        f["arity"] = f["arity"] + 1

    def compile_function(self, name, is_lambda, is_method):
        saved_loops = self.loops
        self.loops = []
        comp = Comp(self.cur, name, 0)
        self.cur = comp
        if is_method:
            self.consume(T_LPAREN, "expected '(' after the method name")
            self.consume(T_IDENT, "expected 'self' as the first method parameter")
            comp.locals.append([self.prev_t()[1], 0, false, false])
            while self.match(T_COMMA):
                self.consume(T_IDENT, "expected a parameter name")
                self.bump_arity(comp)
                self.add_local(self.prev_t()[1])
            self.consume(T_RPAREN, "expected ')' after method parameters")
            self.consume(T_COLON, "expected ':' after the parameter list")
            self.block()
            self.emit(OP_NIL)
            self.emit(OP_RET)
        elif is_lambda:
            comp.locals.append(["", 0, false, false])
            if not self.check(T_COLON):
                self.consume(T_IDENT, "expected a parameter name")
                self.bump_arity(comp)
                self.add_local(self.prev_t()[1])
                while self.match(T_COMMA):
                    self.consume(T_IDENT, "expected a parameter name")
                    self.bump_arity(comp)
                    self.add_local(self.prev_t()[1])
            self.consume(T_COLON, "expected ':' after lambda parameters")
            self.expression()
            self.emit(OP_RET)
        else:
            comp.locals.append(["", 0, false, false])
            self.consume(T_LPAREN, "expected '(' after the function name")
            if not self.check(T_RPAREN):
                self.consume(T_IDENT, "expected a parameter name")
                self.bump_arity(comp)
                self.add_local(self.prev_t()[1])
                while self.match(T_COMMA):
                    self.consume(T_IDENT, "expected a parameter name")
                    self.bump_arity(comp)
                    self.add_local(self.prev_t()[1])
            self.consume(T_RPAREN, "expected ')' after parameters")
            self.consume(T_COLON, "expected ':' after the parameter list")
            self.block()
            self.emit(OP_NIL)
            self.emit(OP_RET)
        fn = comp.fn
        ups = comp.upvals
        self.cur = comp.enclosing
        self.loops = saved_loops
        self.emit(OP_CLOSURE)
        self.emit16(self.add_const(K_FN, fn))
        for u in ups:
            self.emit(1 if u[0] else 0)
            self.emit(u[1])

    def fun_declaration(self):
        self.consume(T_IDENT, "expected a function name")
        name = self.prev_t()[1]
        if self.cur.enclosing != nil:
            self.add_local(name)
        self.compile_function(name, false, false)
        if self.cur.enclosing == nil:
            self.emit(OP_DEF_GLOBAL)
            self.emit16(self.ident_const(name))

    def lambda_(self):
        self.compile_function("<lambda>", true, false)

    def super_(self):
        if not self.cls_active:
            self.err("'super' used outside a class")
        if not self.cls_has_super:
            self.err("'super' used in a class with no superclass")
        self.consume(T_DOT, "expected '.' after 'super'")
        self.consume(T_IDENT, "expected a superclass method name after 'super.'")
        name = self.ident_const(self.prev_t()[1])
        up = self.resolve_upvalue(self.cur, self.cls_name)
        if up >= 0:
            self.emit2(OP_GET_UPVALUE, up)
        else:
            self.emit(OP_GET_GLOBAL)
            self.emit16(self.ident_const(self.cls_name))
        self.emit(OP_GET_SUPER)
        self.emit16(name)

    def class_declaration(self):
        self.consume(T_IDENT, "expected a class name")
        name = self.prev_t()[1]
        namek = self.ident_const(name)
        self.emit(OP_CLASS)
        self.emit16(namek)
        if self.cur.enclosing == nil:
            self.emit(OP_DEF_GLOBAL)
            self.emit16(namek)
        else:
            self.add_local(name)
        saved_name = self.cls_name
        saved_super = self.cls_has_super
        saved_active = self.cls_active
        self.cls_name = name
        self.cls_has_super = false
        self.cls_active = true
        if self.match(T_LPAREN):
            self.consume(T_IDENT, "expected a superclass name")
            sup = self.prev_t()[1]
            if sup == name:
                self.err("a class cannot inherit from itself")
            self.named_variable(sup)
            self.named_variable(name)
            self.emit(OP_INHERIT)
            self.consume(T_RPAREN, "expected ')' after the superclass name")
            self.cls_has_super = true
        else:
            self.named_variable(name)
        self.consume(T_COLON, "expected ':' after the class header")
        self.consume(T_NEWLINE, "expected a newline before the class body")
        self.consume(T_INDENT, "expected an indented class body")
        while not self.check(T_DEDENT) and not self.check(T_EOF):
            self.consume(T_DEF, "class body may only contain method definitions")
            self.consume(T_IDENT, "expected a method name")
            mk = self.ident_const(self.prev_t()[1])
            self.compile_function(self.prev_t()[1], false, true)
            self.emit(OP_METHOD)
            self.emit16(mk)
        self.consume(T_DEDENT, "expected the class body to end (dedent)")
        self.emit(OP_POP)
        self.cls_name = saved_name
        self.cls_has_super = saved_super
        self.cls_active = saved_active

    # with NAME = <resource>: BODY -- deterministic release, mirroring
    # compiler.c with_statement().
    def with_statement(self):
        self.begin_scope()
        self.consume(T_IDENT, "expected a name after 'with'")
        var = self.prev_t()
        self.consume(T_ASSIGN, "expected '=' after the name in 'with'")
        self.expression()
        slot = self.add_local(var[1])
        self.cur.locals[slot][3] = true
        self.emit2(OP_WITH_BEGIN, slot)
        self.consume(T_COLON, "expected ':' after the 'with' resource")
        self.block()
        self.end_scope()

    def statement(self):
        if self.match(T_IF):
            self.if_statement()
        elif self.match(T_WHILE):
            self.while_statement()
        elif self.match(T_FOR):
            self.for_statement()
        elif self.match(T_BREAK):
            self.break_statement()
        elif self.match(T_CONTINUE):
            self.continue_statement()
        elif self.match(T_RETURN):
            self.return_statement()
        elif self.match(T_IMPORT):
            self.import_statement()
        elif self.match(T_FROM):
            self.from_statement()
        elif self.match(T_TRY):
            self.try_statement()
        elif self.match(T_WITH):
            self.with_statement()
        elif self.match(T_RAISE):
            self.raise_statement()
        elif self.assign_ahead():
            self.assignment()
            self.consume_stmt_end()
        else:
            # A command statement: if the outermost operation was |>, -> or <-,
            # the pipeline gets the scope `with` would have given it (started
            # here, waited for at the end of the statement) instead of being
            # silently dropped. Mirrors compiler.c statement().
            before = self.code_len()
            self.expression()
            last = self.cur.fn["code"][self.code_len() - 1] if self.code_len() > before else OP_NIL
            if last == OP_PIPE or last == OP_REDIR_OUT or last == OP_REDIR_IN:
                self.begin_scope()
                slot = self.add_local("")
                self.cur.locals[slot][3] = true
                self.emit2(OP_WITH_BEGIN, slot)
                self.end_scope()
            else:
                self.emit(OP_POP)
            self.consume_stmt_end()

    def declaration(self):
        if self.match(T_DEF):
            self.fun_declaration()
        elif self.match(T_CLASS):
            self.class_declaration()
        else:
            self.statement()

    def compile_module(self):
        top = Comp(nil, "", 0)
        top.locals.append(["", 0, false, false])
        self.cur = top
        while not self.check(T_EOF):
            if self.match(T_NEWLINE):
                continue
            self.declaration()
        self.emit(OP_NIL)
        self.emit(OP_RET)
        return top.fn

# ---- entry points ----
def compile_src(src):
    return Parser(lex(src)).compile_module()

def compile_file(inp, outp):
    src = file_read(inp)
    if src == nil:
        raise f"cannot open {inp}"
    blob = dump_module(compile_src(src))
    if file_write(outp, blob) < 0:
        raise f"cannot write {outp}"
    return len(blob)
