# S1 harness driver: dump tokens from the self-hosted lexer in the same
# "type line len checksum" format as `asc -lex`.
from aslex import lex
src = file_read(args()[1])
for t in lex(src):
    s = 0
    txt = t[1]
    for k in range(len(txt)):
        s = (s + ord(txt[k])) % 9973
    print(f"{t[0]} {t[2]} {len(txt)} {s}")
