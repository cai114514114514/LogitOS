# selfhost -- run the AetherScript compiler (written in AetherScript) ON Logit.
# Proves the whole pipeline (lex -> Pratt parse -> bytecode -> .la serialize)
# executes on the real OS, not just the host.
import asc
fn = asc.compile_src("def sq(n):\n    return n * n\nprint(sq(9))\n")
blob = asc.dump_module(fn)
print("selfhost bytes:", len(blob) > 0)
print("selfhost magic:", blob.sub(0, 4))
print("selfhost ok")
