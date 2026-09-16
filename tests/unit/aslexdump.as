# aether: 3.0
# The original S1 tool now runs as native A3. Keep its four-column output so
# callers can compare it with the independent C frontend's `as -lex` stream.
# The lexer's public token sequence uses explicit Any fields; casts validate
# that interface instead of relying on the retired VM's implicit conversions.
from std.aslex import lex

def main() -> i64:
    arguments = args()
    if len(arguments) != 2:
        print("usage: aslexdump SOURCE")
        return 2
    # Keep only the source bytes after leaving this scope. Tokenization and
    # later GC must not keep a file descriptor alive for the whole tool run.
    with file = open(arguments[1]):
        source = file.readall().decode()
    for token in lex(source):
        checksum = 0
        text = cast[str](token[1])
        for index in range(len(text)):
            checksum = (checksum + ord(text[index])) % 9973
        kind = cast[i64](token[0])
        line = cast[i64](token[2])
        print(f"{kind} {line} {len(text)} {checksum}")
    return 0
