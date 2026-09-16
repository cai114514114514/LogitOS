# aether: 3.0
# barriers -- how many write barriers has the kernel issued, and does writing a
# file issue any?
#
#   as /usr/as/examples/barriers.as
#
# A journal orders nothing on its own. A completed block write means the DEVICE
# accepted the block, not that the platter holds it, and a disk reorders freely
# inside its own write cache -- so "write the data, then write the commit record"
# is not an ordering unless a barrier separates them. QEMU's virtio-blk really
# does advertise a writeback cache (the kernel prints it at boot), so this is not
# theoretical here.
#
# The count comes from the kernel's own sysinfo text, so this measures what the
# filesystem actually asked the hardware for, not what the code appears to say.

from std.abi import sysinfo
from std.strings import lines, starts_with

# -1 means the query itself failed, -2 means it worked but had no Barriers line.
# Two different problems deserve two different answers: collapsing them cost a
# debugging cycle chasing a missing line that was really a refused syscall.
def barrier_count() -> i64:
    b = buffer(4096)
    n = sysinfo(b, len(b))
    if n <= 0:
        return -1
    if n > len(b):
        return -3
    unsafe:
        text = mem2str(b, n)

    # The old substring scan accepted a label inside unrelated text and
    # silently picked the last duplicate. Require one complete counter row.
    # -3 distinguishes malformed data from a refused or absent query.
    count = -2
    for line in lines(text):
        if starts_with(line, "Barriers "):
            if count != -2:
                return -3
            count = parse_int(line.slice(9, len(line)))
            if count < 0:
                return -3
    return count


def _failure(count: i64) -> None:
    if count == -1:
        print("BARRIERS-FAIL sysinfo() was refused (returned <= 0)")
    elif count == -2:
        print("BARRIERS-FAIL sysinfo has no Barriers line")
    else:
        print("BARRIERS-FAIL malformed sysinfo counter")


def main() -> i64:
    before = barrier_count()
    if before < 0:
        _failure(before)
        return 1
    file_write("/dur/barrier.probe", Bytes("a barrier probe, written to force a transaction"))
    after = barrier_count()
    if after < 0:
        _failure(after)
        return 1
    print("BARRIERS", before, "->", after, "delta", after - before)
    # One committed transaction issues three (staged blocks, commit record,
    # checkpoint). Creating a file is more than one transaction, so the floor is
    # deliberately loose -- the assertion that matters is "not zero".
    if after - before >= 3:
        print("BARRIERS-OK")
        return 0
    else:
        print("BARRIERS-FAIL a file write issued", after - before, "barriers, expected >= 3")
        return 1
