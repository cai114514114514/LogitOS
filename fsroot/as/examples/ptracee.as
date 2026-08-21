# ptracee -- a program that stays alive in RING 3 long enough to be looked at.
#
# The tracee half of `make test-ptrace-os`. Its whole job is to be a process
# that is genuinely EXECUTING while somebody else inspects it.
#
# THE INNER LOOP MAKES NO SYSCALL, AND THE FIRST VERSION OF THIS FILE DID.
# It was `while syscall(SYS_MONOTONIC_MS,...) - t0 < 15000: i = i + 1`, which
# looks like a spin and is not: a stopped process parks at the return-to-ring-3
# boundary, and for a loop whose every iteration enters the kernel that
# boundary is ALWAYS THE SAME INSTRUCTION -- the one after the `int $0x80` in
# the syscall wrapper. Measured: two attach/detach rounds 400 ms apart both
# reported rip 0x50004ba2, exactly equal, and the tracer's "these registers are
# moving" check failed against a completely correct kernel.
#
# So the clock is consulted only once per OUTER iteration and the inner loop is
# pure arithmetic in the VM. The only thing that will ever enter the kernel
# from inside it is the 100 Hz timer, which lands wherever the interpreter
# happens to be -- which is the point, and is also the harder case for ATTACH:
# nothing about this thread cooperates with being stopped.
#
# BOUNDED BY WALL CLOCK, not by an iteration count. A count is a different
# amount of time on every machine and under every load -- and this runs under
# TCG, where it would be different again. 20 seconds is long enough for four
# attach/detach rounds and short enough that a harness which loses its QEMU
# does not leave a process spinning forever.

SYS_MONOTONIC_MS = 75

t0 = syscall(SYS_MONOTONIC_MS, 0, 0, 0)

# Printed BEFORE the loop, so the line means "this process is in its loop", not
# "this process was created": a human reading the log has to be able to tell a
# tracee that never started from one that was never attached to.
print("PTRACEE-UP")

i = 0
spin = 1
while spin == 1:
    j = 0
    while j < 4000:
        i = i + j
        j = j + 1
    if syscall(SYS_MONOTONIC_MS, 0, 0, 0) - t0 > 20000:
        spin = 0

print("PTRACEE-DONE", i)
