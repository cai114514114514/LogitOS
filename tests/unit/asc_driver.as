# S2 harness driver: compile a source file to .la with the self-hosted compiler.
from asc import compile_file
a = args()
compile_file(a[1], a[2])
