# aslex -- the AetherScript lexer, in AetherScript (M21-P3 self-hosting, S1).
# A faithful translation of c/apps/as/lexer.c: batch tokenizer with Python
# INDENT/DEDENT, ()[]{} line continuation, hex/float numbers, raw-interior
# strings (escapes preserved for the compiler to decode), and M23 f-strings
# (raw interior with hole tracking). Errors raise a string exception.
#
# Token = [type, text, line].  Type numbers mirror lexer.h's enum exactly
# (the fixpoint harness compares them against the C lexer's output).

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
# M27 ports: |>  ->  <-  and the `with` keyword.
T_PIPEOP = 66
T_ARROW = 67
T_LARROW = 68
T_WITH = 69
T_EOF = 70
T_ERROR = 71

KEYWORDS = {"if": T_IF, "or": T_OR, "in": T_IN, "def": T_DEF, "and": T_AND,
            "not": T_NOT, "for": T_FOR, "nil": T_NIL, "try": T_TRY,
            "elif": T_ELIF, "else": T_ELSE, "from": T_FROM, "true": T_TRUE,
            "while": T_WHILE, "false": T_FALSE, "class": T_CLASS,
            "super": T_SUPER, "break": T_BREAK, "raise": T_RAISE,
            "return": T_RETURN, "import": T_IMPORT, "lambda": T_LAMBDA,
            "except": T_EXCEPT, "with": T_WITH, "continue": T_CONTINUE}

def is_digit(c):
    return c >= 48 and c <= 57

def is_alpha(c):
    return (c >= 97 and c <= 122) or (c >= 65 and c <= 90) or c == 95

def is_alnum(c):
    return is_alpha(c) or is_digit(c)

def is_hex(c):
    return is_digit(c) or (c >= 97 and c <= 102) or (c >= 65 and c <= 70)

class Lexer:
    def init(self, src):
        self.src = src
        self.n = len(src)
        self.i = 0
        self.line = 1
        self.toks = []
        self.indent = [0]
        self.bracket = 0

    def at(self, i):
        return ord(self.src[i]) if i < self.n else 0

    def emit(self, t, text):
        self.toks.append([t, text, self.line])

    def err(self, msg):
        raise f"{msg} (line {self.line})"

    # Same message, but about a line the scanner has already walked past. The
    # unterminated-literal errors are the only ones that need it and they are
    # the reason it exists: a string with no closing quote swallows every
    # newline after it, so self.line by the time we notice is the END of the
    # file, and the user is pointed at a line that is not the one with the
    # stray quote on it. This is the lexer's half of the same idea as
    # Parser.err_at in asc.as -- and of error_at(Token, ...) in compiler.c,
    # which the C lexer has but does not use for this either: both compilers
    # reported the wrong line here until this gate measured it.
    def err_at(self, line, msg):
        raise f"{msg} (line {line})"

    # ---- scan one f-string body: self.i is just past the opening quote.
    # Returns the raw interior; leaves self.i past the closing quote.
    def fstring_body(self, q):
        start = self.i
        open_line = self.line          # self.line still points at the f" itself
        depth = 0
        while self.i < self.n and (self.at(self.i) != q or depth > 0):
            c = self.at(self.i)
            if c == 10:
                self.line += 1
                self.i += 1
                continue
            if depth == 0:
                if c == 92 and self.i + 1 < self.n:        # backslash escape
                    self.i += 2
                    continue
                if c == 123 and self.at(self.i + 1) == 123:  # {{
                    self.i += 2
                    continue
                if c == 125 and self.at(self.i + 1) == 125:  # }}
                    self.i += 2
                    continue
                if c == 123:                                  # enter a hole
                    depth = 1
                self.i += 1
                continue
            if c == 39 or c == 34:                            # string inside hole
                hq = c
                self.i += 1
                while self.i < self.n and self.at(self.i) != hq:
                    if self.at(self.i) == 92 and self.i + 1 < self.n:
                        self.i += 2
                    else:
                        if self.at(self.i) == 10:
                            self.line += 1
                        self.i += 1
                if self.i < self.n:
                    self.i += 1
                continue
            if c == 123 or c == 40 or c == 91:
                depth += 1
            elif c == 125 or c == 41 or c == 93:
                depth -= 1
            self.i += 1
        if self.at(self.i) != q:
            self.err_at(open_line, "unterminated f-string")
        body = self.src.sub(start, self.i)
        self.i += 1
        return body

    # ---- scan tokens to the end of the current logical line
    def scan_line(self):
        while true:
            while self.at(self.i) == 32 or self.at(self.i) == 9:
                self.i += 1
            if self.at(self.i) == 35:                  # comment
                while self.i < self.n and self.at(self.i) != 10:
                    self.i += 1
            if self.i >= self.n:
                return nil
            c = self.at(self.i)
            if c == 10:
                if self.bracket > 0:
                    self.i += 1
                    self.line += 1
                    continue
                return nil
            s = self.i
            if is_digit(c) or (c == 46 and is_digit(self.at(self.i + 1))):
                if c == 48 and (self.at(self.i + 1) == 120 or self.at(self.i + 1) == 88):
                    self.i += 2
                    hx = self.i
                    while is_hex(self.at(self.i)):
                        self.i += 1
                    if self.i == hx:
                        self.err("'0x' needs hex digits")
                    self.emit(T_INT, self.src.sub(s, self.i))
                else:
                    isf = false
                    while is_digit(self.at(self.i)):
                        self.i += 1
                    if self.at(self.i) == 46:
                        isf = true
                        self.i += 1
                        while is_digit(self.at(self.i)):
                            self.i += 1
                    if self.at(self.i) == 101 or self.at(self.i) == 69:
                        isf = true
                        self.i += 1
                        if self.at(self.i) == 43 or self.at(self.i) == 45:
                            self.i += 1
                        while is_digit(self.at(self.i)):
                            self.i += 1
                    self.emit(T_FLOAT if isf else T_INT, self.src.sub(s, self.i))
            elif is_alpha(c):
                while is_alnum(self.at(self.i)):
                    self.i += 1
                word = self.src.sub(s, self.i)
                nq = self.at(self.i)
                if len(word) == 1 and (word == "f" or word == "F") and (nq == 34 or nq == 39):
                    self.i += 1
                    self.emit(T_FSTR, self.fstring_body(nq))
                else:
                    self.emit(KEYWORDS[word] if word in KEYWORDS else T_IDENT, word)
            elif c == 34 or c == 39:
                q = c
                open_line = self.line      # before the scan walks past newlines
                self.i += 1
                cs = self.i
                while self.i < self.n and self.at(self.i) != q:
                    if self.at(self.i) == 92 and self.i + 1 < self.n:
                        if self.at(self.i + 1) == 10:
                            self.line += 1
                        self.i += 2
                    else:
                        if self.at(self.i) == 10:
                            self.line += 1
                        self.i += 1
                if self.at(self.i) != q:
                    self.err_at(open_line, "unterminated string")
                self.emit(T_STR, self.src.sub(cs, self.i))
                self.i += 1
            else:
                self.i += 1
                n2 = self.at(self.i)                   # char after c
                if c == 40:
                    self.emit(T_LPAREN, "(")
                    self.bracket += 1
                elif c == 41:
                    self.emit(T_RPAREN, ")")
                    if self.bracket > 0:
                        self.bracket -= 1
                elif c == 91:
                    self.emit(T_LBRACKET, "[")
                    self.bracket += 1
                elif c == 93:
                    self.emit(T_RBRACKET, "]")
                    if self.bracket > 0:
                        self.bracket -= 1
                elif c == 123:
                    self.emit(T_LBRACE, "{")
                    self.bracket += 1
                elif c == 125:
                    self.emit(T_RBRACE, "}")
                    if self.bracket > 0:
                        self.bracket -= 1
                elif c == 44:
                    self.emit(T_COMMA, ",")
                elif c == 58:
                    self.emit(T_COLON, ":")
                elif c == 59:
                    self.emit(T_SEMI, ";")
                elif c == 46:
                    self.emit(T_DOT, ".")
                elif c == 43:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_PLUSEQ, "+=")
                    else:
                        self.emit(T_PLUS, "+")
                elif c == 45:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_MINUSEQ, "-=")
                    elif n2 == 62:
                        self.i += 1
                        self.emit(T_ARROW, "->")
                    else:
                        self.emit(T_MINUS, "-")
                elif c == 42:
                    if n2 == 42:
                        self.i += 1
                        self.emit(T_POW, "**")
                    elif n2 == 61:
                        self.i += 1
                        self.emit(T_STAREQ, "*=")
                    else:
                        self.emit(T_STAR, "*")
                elif c == 47:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_SLASHEQ, "/=")
                    else:
                        self.emit(T_SLASH, "/")
                elif c == 37:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_PERCENTEQ, "%=")
                    else:
                        self.emit(T_PERCENT, "%")
                elif c == 38:
                    self.emit(T_AMP, "&")
                elif c == 124:
                    if n2 == 62:
                        self.i += 1
                        self.emit(T_PIPEOP, "|>")
                    else:
                        self.emit(T_PIPE, "|")
                elif c == 94:
                    self.emit(T_CARET, "^")
                elif c == 126:
                    self.emit(T_TILDE, "~")
                elif c == 61:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_EQ, "==")
                    else:
                        self.emit(T_ASSIGN, "=")
                elif c == 33:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_NE, "!=")
                    else:
                        self.err("unexpected '!'")
                elif c == 60:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_LE, "<=")
                    elif n2 == 60:
                        self.i += 1
                        self.emit(T_SHL, "<<")
                    elif n2 == 45:
                        self.i += 1
                        self.emit(T_LARROW, "<-")
                    else:
                        self.emit(T_LT, "<")
                elif c == 62:
                    if n2 == 61:
                        self.i += 1
                        self.emit(T_GE, ">=")
                    elif n2 == 62:
                        self.i += 1
                        self.emit(T_SHR, ">>")
                    else:
                        self.emit(T_GT, ">")
                else:
                    self.err(f"unexpected character '{chr(c)}'")

    def run(self):
        while self.i < self.n:
            if self.bracket == 0:
                col = 0
                while self.at(self.i) == 32 or self.at(self.i) == 9:
                    col = col + (8 - col % 8 if self.at(self.i) == 9 else 1)
                    self.i += 1
                if self.at(self.i) == 10:               # blank line
                    self.i += 1
                    self.line += 1
                    continue
                if self.at(self.i) == 35:               # comment-only line
                    while self.i < self.n and self.at(self.i) != 10:
                        self.i += 1
                    continue
                if self.i >= self.n:
                    break
                top = self.indent[len(self.indent) - 1]
                if col > top:
                    if len(self.indent) >= 64:
                        self.err("too many indentation levels")
                    self.indent.append(col)
                    self.emit(T_INDENT, "")
                else:
                    while col < self.indent[len(self.indent) - 1]:
                        self.pop_indent()
                    if col != self.indent[len(self.indent) - 1]:
                        self.err("inconsistent indentation")
            self.scan_line()
            if self.bracket == 0 and self.at(self.i) == 10:
                self.emit(T_NEWLINE, "")
                self.i += 1
                self.line += 1
            elif self.i >= self.n:
                break
        nt = len(self.toks)
        if nt > 0 and self.toks[nt - 1][0] != T_NEWLINE:
            self.emit(T_NEWLINE, "")
        while len(self.indent) > 1:
            self.pop_indent()
        self.emit(T_EOF, "")
        return self.toks

    def pop_indent(self):
        # drop the top indent level + emit DEDENT (no list.pop(): rebuild len-1)
        cut = []
        for k in range(len(self.indent) - 1):
            cut.append(self.indent[k])
        self.indent = cut
        self.emit(T_DEDENT, "")

def lex(src):
    return Lexer(src).run()
