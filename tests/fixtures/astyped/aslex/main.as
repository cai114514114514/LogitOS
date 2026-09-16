# aether: 3.0
from aslex import Lexer, lex, is_digit, is_alpha, is_alnum, is_hex
from aslex import T_IDENT, T_ASSIGN, T_STR, T_NEWLINE, T_EOF, T_INDENT, T_DEDENT


def tokens() -> None:
    source = "name = '中文'\n"
    scanned = lex(source)
    assert len(scanned) == 5
    expected = [T_IDENT, T_ASSIGN, T_STR, T_NEWLINE, T_EOF]
    for index in range(len(expected)):
        assert cast[i64](scanned[index][0]) == expected[index]
    assert cast[str](scanned[2][1]) == "中文"
    assert cast[i64](scanned[2][3]) == 7
    # End offsets retain the lexer API: a string excludes its closing quote.
    assert cast[i64](scanned[2][4]) == 14
    assert cast[i64](scanned[3][3]) == 15

    indented = lex("if true:\n\tx = [\n  1, 2]\n\ty = 3\nz = 4")
    enters = 0
    leaves = 0
    for token in indented:
        if cast[i64](token[0]) == T_INDENT:
            enters += 1
        if cast[i64](token[0]) == T_DEDENT:
            leaves += 1
    assert enters == 1 and leaves == 1
    assert is_digit(48) and not is_digit(47)
    assert is_alpha(95) and not is_alpha(48)
    assert is_alnum(57) and not is_alnum(32)
    assert is_hex(70) and is_hex(102) and not is_hex(103)


def failures() -> None:
    scanner = Lexer("\n\n'broken\ntext")
    try:
        scanner.run()
        assert false
    except ValueError as error:
        assert error.message == "unterminated string (line 3)"
        assert scanner.error_span[0] == 2
    try:
        lex("\nf\"{value\n")
        assert false
    except ValueError as error:
        assert error.message == "unterminated f-string (line 2)"
    try:
        lex("0x")
        assert false
    except ValueError as error:
        assert error.message == "'0x' needs hex digits (line 1)"
    try:
        lex("if true:\n    x = 1\n  y = 2")
        assert false
    except ValueError as error:
        assert error.message == "inconsistent indentation (line 3)"
    try:
        lex("!")
        assert false
    except ValueError as error:
        assert error.message == "unexpected '!' (line 1)"
    try:
        lex("@")
        assert false
    except ValueError as error:
        assert error.message == "unexpected character '@' (line 1)"


def retained() -> None:
    scanner = Lexer("value = " + "'retained text'")
    run = scanner.run
    gc_collect()
    result = run()
    gc_collect()
    assert cast[str](result[2][1]) == "retained text"
    # Public incremental helpers remain callable with concrete argument types.
    other = Lexer("")
    other.emit(T_IDENT, "manual")
    assert cast[str](other.toks[0][1]) == "manual"
    other.indent.append(4)
    other.pop_indent()
    assert len(other.indent) == 1
    assert cast[i64](other.toks[1][0]) == T_DEDENT


def main() -> None:
    tokens()
    failures()
    retained()
    # Keyword globals stay rooted; transient lexers/tokens must be reclaimed.
    gc_collect()
    baseline = gc_live_bytes()
    for iteration in range(20):
        tokens()
        gc_collect()
    assert gc_live_bytes() == baseline
    print("native lexer library ok")
