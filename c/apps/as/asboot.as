# asboot -- the bridge that makes /bin/as's compiler AetherScript, not C.
#
# THIS IS DELIBERATELY THE SMALLEST THING THAT CAN BE A BRIDGE. /bin/as
# (c/apps/as/as.c) no longer links compiler.c or lexer.c, so the binary has no
# way of its own to turn source into bytecode. What it has instead is this
# module, compiled at BUILD time by the host asc and embedded in the binary as
# build/asboot_la.inc. To compile a program, as.c:
#
#   1. makes a module, sets its global __src to the program text,
#   2. as_load()s this module's bytecode and as_run()s it,
#   3. reads __blob (the .la bytes) or __err (the message) back out of it.
#
# There is no C -> AetherScript call bridge in this tree (as.h exports no
# "call this closure with these arguments"), and adding one would have meant
# editing vm.c -- which is the oracle half of the crosscheck. Module globals
# ARE readable and writable from C (as_module_slot), so the argument and the
# result travel through globals instead. That is the whole reason this file is
# a module-level script and not a function.
#
# `from asc import ...` is the line that matters: it is what pulls
# /usr/as/lib/asc.la -- the compiler that ships -- in through the VM's own
# module loader, with no special path handling here.
#
# THE try/except IS NOT DECORATION. asc raises a STRING on a syntax error. An
# uncaught raise at module level would reach the user as "uncaught exception"
# plus a traceback through the compiler's internals, where the C compiler used
# to print one line naming the line number. Catching it here keeps
# `as: <message>` reading the way it always did.
from asc import compile_src, dump_module

__blob = nil
__err = nil
try:
    __blob = dump_module(compile_src(__src))
except e:
    __err = str(e)
