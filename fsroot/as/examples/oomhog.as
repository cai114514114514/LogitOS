# oomhog -- take most of the machine's memory and then just sit there.
#
#   as /usr/as/examples/oomhog.as <mib>
#
# WHY THIS IS NOT mempress.as. mempress maps more than exists and then keeps
# touching it, so IT is the process faulting when memory runs out -- and the
# out-of-memory killer's answer in that case is the one that already worked
# before the killer existed: the program asking for memory is the program that
# dies. That case proves nothing about choosing a victim.
#
# The bug being gated is the OTHER one: memory is gone, and the process that
# dies is whoever touches a page NEXT. To produce it, something has to hold the
# memory while somebody ELSE faults. So this program takes what it wants, says
# so, and then stops allocating entirely.
#
# THE HEARTBEAT IS NOT DECORATION. A kill on this machine is a MARK: the victim
# runs proc_exit() on itself at its next kernel entry (c/kernel/exec/proc.c), so
# a process spinning in pure bytecode with no syscall in it would be marked and
# never die -- and the harness would record "the killer does not work" when what
# it actually found is a documented cost of the mark-and-check design. The loop
# below makes a syscall per iteration, which is what a real program does (a GUI
# app polls an event every frame, a shell reads a line) and is the condition the
# killer's own comment says it relies on.
#
# The data is written with a per-page pattern for the same reason mempress does
# it: a page holding data cannot be taken by reclaim's free drop tier, so the
# pressure is real rather than something the kernel can absorb.

SYS_MMAP = 92
SYS_YIELD = 12

WORDS = 512              # 8-byte words in a 4096-byte page

a = args()
mib = 128
if len(a) > 1:
    if a[1] == "64":
        mib = 64
    if a[1] == "96":
        mib = 96
    if a[1] == "160":
        mib = 160
    if a[1] == "192":
        mib = 192
    if a[1] == "200":
        mib = 200
    if a[1] == "224":
        mib = 224

npages = mib * 256
bytes = mib * 1024 * 1024

print("OOMHOG-START", mib, "MiB,", npages, "pages")

base = syscall(SYS_MMAP, bytes, 3, 0)
if base < 4096:
    print("OOMHOG-FAIL mmap refused:", base)
else:
    p = i64ptr(base)
    i = 0
    while i < npages:
        # One word per page: enough to make the page resident AND to make it
        # hold data (so the drop tier cannot have it back for free).
        p[i * WORDS] = i + 7
        i = i + 1
    print("OOMHOG-RESIDENT", npages, "pages")

    # From here on: no allocation, no growth, one syscall per iteration so the
    # kill mark can be claimed. Unbounded on purpose -- the harness kills the
    # machine, and a hog that exits on its own would end the pressure early and
    # let the innocent process succeed for the wrong reason.
    n = 0
    while 1 == 1:
        syscall(SYS_YIELD, 0, 0, 0)
        n = n + 1
        if n % 2000000 == 0:
            print("OOMHOG-ALIVE", n)

print("OOMHOG-DONE")
