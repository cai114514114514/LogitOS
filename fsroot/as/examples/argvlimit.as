# argvlimit.as -- SYS_EXECVE's argument-vector bound, proved on the machine.
# Driven by tests/boot/run-argv-limits-test.sh.
#
# WHY A SCRIPT AND NOT THE SHELL. c/kernel/exec/exec.c used to accept a vector
# of 49 entries and silently exec the first 48 (copy_uvec stopped at its bound
# and never looked at the slot after it). /bin/sh cannot reach that line:
# the shell's own bound is the same constant (include/abi/logit_exec.h), so
# every argv it builds fits. The caller that can overshoot is one that builds
# its argv by hand -- exactly what /bin/as does with buffer()/poke64()/
# syscall(), and exactly what a ported program with its own exec wrapper
# would do. Same reasoning as pcachecheck.as: no new binary on the disk to
# drive a syscall nothing else in userland calls this way yet.
#
# THE NUMBERS ARE SPELLED OUT, not imported: fsroot/as/lib/abi.as is generated
# from include/abi/logit_calls.abi and does not carry LOGIT_ARG_MAX. They are
# the values in include/abi/logit_exec.h; if that header moves, this script
# reports the new kernel answer against the old expectation, which is the
# failure wanted.

from sys import run

SYS_EXECVE = 51
E2BIG = -7             # LOGIT_EXEC_E2BIG
ARG_MAX = 256          # LOGIT_ARG_MAX: entries in argv, NULL not counted
ARG_BYTES = 16384      # LOGIT_ARG_BYTES: argv + envp strings together

# A NULL-terminated char*[] in script memory (sys.as's _argv, repeated here
# because it is private there and the point is to build the array by hand).
def vec(strings):
    n = len(strings)
    pv = buffer(8 * (n + 1))
    for i in range(n):
        poke64(addr(pv) + 8 * i, addr(strings[i]))
    poke64(addr(pv) + 8 * n, 0)
    return pv

path = "/bin/true"

# 1. ARG_MAX + 1 entries. The old kernel ran /bin/true with ARG_MAX of them and
#    this script would have been replaced by it, printing nothing further.
args = ["true"]
for i in range(ARG_MAX):
    args.append("x")
pv = vec(args)
rc = syscall(SYS_EXECVE, addr(path), addr(pv), 0)
print("argvlimit: 257 entries -> rc", rc, "E2BIG:", rc == E2BIG)

# 2. Two entries, one of them ARG_BYTES long: the byte budget, not the count.
big = "y"
while len(big) < ARG_BYTES:
    big = big + big
pv = vec(["true", big])
rc = syscall(SYS_EXECVE, addr(path), addr(pv), 0)
print("argvlimit: 16 KiB string -> rc", rc, "E2BIG:", rc == E2BIG)

# 3. Exactly ARG_MAX entries RUN, and the last one arrives: a real fork+execve
#    of /bin/echo whose final argument is the marker the harness looks for.
#    A kernel that still dropped the tail would print the line without it.
args = ["echo"]
for i in range(ARG_MAX - 2):
    args.append("k")
args.append("ARGVLIMIT-LAST-256")
code = run("/bin/echo", args)
print("argvlimit: 256 entries exit", code)

print("argvlimit ok")
