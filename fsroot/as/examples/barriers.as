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

from abi import sysinfo

# -1 means the query itself failed, -2 means it worked but had no Barriers line.
# Two different problems deserve two different answers: collapsing them cost a
# debugging cycle chasing a missing line that was really a refused syscall.
def barrier_count():
    b = buffer(1024)
    n = sysinfo(b, 1024)
    if n <= 0:
        return -1
    text = mem2str(b, n)
    # "Barriers <n>\n" -- pull the digits that follow the label
    key = "Barriers "
    at = -1
    for i in range(len(text) - len(key) + 1):
        hit = true
        for k in range(len(key)):
            if text[i + k] != key[k]:
                hit = false
        if hit:
            at = i + len(key)
    if at < 0:
        return -2
    v = 0
    i = at
    while i < len(text) and ord(text[i]) >= 48 and ord(text[i]) <= 57:
        v = v * 10 + (ord(text[i]) - 48)
        i = i + 1
    return v

before = barrier_count()
if before == -1:
    print("BARRIERS-FAIL sysinfo() was refused (returned <= 0)")
elif before == -2:
    print("BARRIERS-FAIL sysinfo has no Barriers line")
else:
    file_write("/dur/barrier.probe", "a barrier probe, written to force a transaction")
    after = barrier_count()
    print("BARRIERS", before, "->", after, "delta", after - before)
    # One committed transaction issues three (staged blocks, commit record,
    # checkpoint). Creating a file is more than one transaction, so the floor is
    # deliberately loose -- the assertion that matters is "not zero".
    if after - before >= 3:
        print("BARRIERS-OK")
    else:
        print("BARRIERS-FAIL a file write issued", after - before, "barriers, expected >= 3")
