# elfstat.as -- read the kernel's memory instruments from ring 3, at a named
# moment, and optionally force a reclaim pass first.
#
#   as /usr/as/examples/elfstat.as <tag>            print the counters
#   as /usr/as/examples/elfstat.as <tag> <n>        ask reclaim for n frames first
#
# WHY A SCRIPT AND NOT A NEW BINARY: the same argument pcachecheck.as makes --
# /bin/as already has raw syscalls, so a counter that is reachable from ring 3
# needs nothing new on the disk to read it.
#
# WHAT IT PRINTS AND WHY THAT SET. Three lines matter and they are not
# interchangeable:
#   [mmstat]   free=/dropped=  -- machine-readable, greppable, and the ONLY
#              number that answers "how many frames does this machine have
#              left", which is the whole of G1.
#   [reclaim]  ... dropped free [N zero + M cache] ...  -- M is the counter
#              that has never been nonzero, because until file-backed text
#              existed nothing in this tree produced a page for tier 1's
#              second producer to drop.
#   [pcache]   ... peak P of 4096 slots ...  -- R5's ceiling, against which
#              a full pool silently hands back UNCACHED pages.
#
# THE MEASURING PROCESS IS ITSELF /bin/as, and that is deliberate rather than
# convenient: it means every reading in a series is taken with exactly one
# extra copy of the subject binary resident, so that copy is a CONSTANT across
# the series and cancels out of every difference. A measurement taken by a
# different binary each time would not have that property.

SYS_MEMINFO = 94

MMCTL_REPORT  = 1        # c/kernel/mm/mmsys.c
MMCTL_AUDIT   = 2
MMCTL_RECLAIM = 3
MMCTL_STATS   = 4

a = args()
tag = a[1] if len(a) > 1 else "?"

print("ELFSTAT-BEGIN", tag)

# The forced pass comes FIRST, so the counters printed below already include
# whatever it did. Asking for frames the machine is not short of is the only
# way to make reclaim run at all here: the full desktop peaks at 229 MiB of
# 511 and never reaches the watermark on its own, so "the counter stayed zero"
# under natural load would be evidence about the workload, not the mechanism.
if len(a) > 2:
    # There is no int() builtin in this language (mempress.as says so in as
    # many words), so the frame count is chosen by NAME rather than parsed.
    n = 64
    if a[2] == "256":
        n = 256
    if a[2] == "1024":
        n = 1024
    if a[2] == "4096":
        n = 4096
    got = syscall(SYS_MEMINFO, 0, MMCTL_RECLAIM, n)
    print("ELFSTAT-RECLAIM", tag, "asked", n, "got", got)

syscall(SYS_MEMINFO, 0, MMCTL_STATS, 0)
syscall(SYS_MEMINFO, 0, MMCTL_REPORT, 0)
bugs = syscall(SYS_MEMINFO, 0, MMCTL_AUDIT, 0)
print("ELFSTAT-AUDIT", tag, bugs)

print("ELFSTAT-END", tag)
