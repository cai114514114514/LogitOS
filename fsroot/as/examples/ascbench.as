# ascbench -- what /bin/as costs ON THE MACHINE, not on the host.
#
# SW-shipped-compiler replaced /bin/as's C compiler with the AetherScript one
# (/usr/as/lib/asc.la, interpreted by the C VM). The host figures for that swap
# were single-digit milliseconds; QEMU/TCG is not the host, and the shell the
# GUI Terminal spawns can be `as ash.as`, so the number a user feels is the one
# measured here. Run it before and after and diff the two.
#
# WHAT IS TIMED is a whole `run()`: fork + execve /bin/as + read the script off
# LogitFS + compile + execute + exit. That is deliberately not just the
# compiler -- process start is part of what a user waits for, and isolating the
# compiler would report a smaller number than anybody experiences.
#
# The first run of each case is a discarded warm-up: the very first spawn of
# /bin/as pays for paging its ELF in, and charging that to the compiler would
# overstate the change.
from abi import monotonic_ms
from sys import run

def bench(label, argv):
    run("/bin/as", argv)                 # warm-up, not counted
    i = 0
    total = 0
    best = 0
    while i < 3:
        t0 = monotonic_ms()
        rc = run("/bin/as", argv)
        dt = monotonic_ms() - t0
        if rc != 0:
            print("ascbench", label, "FAILED rc", rc)
            return
        total = total + dt
        if best == 0 or dt < best:
            best = dt
        i = i + 1
    print("ascbench", label, "best_ms", best, "avg_ms", total / 3)

# args is a FULL argv (sys.spawn's contract: args[0] is argv[0]). Getting that
# wrong does not error -- /bin/as with no script argument reads stdin, so the
# child blocks forever on the shell's pipe and the benchmark hangs. It did.
#
# A 6-line script: the floor -- almost all of this is spawn + loading the
# compiler, and almost none of it is compiling.
bench("hello", ["as", "/usr/as/examples/hello.as"])
# ash.as (204 lines) compiled but not run: exactly the work added to the start
# of every AetherScript shell.
bench("ash-c", ["as", "-c", "/usr/as/examples/ash.as", "-o", "/ascbench.la"])
print("ascbench done")
