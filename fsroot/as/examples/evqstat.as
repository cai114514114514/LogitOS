# aether: 3.0
# evqstat.as -- print the machine's SYS_SYSINFO block.
#
# Exists so the window manager's event-ring accounting is readable from a
# script, and therefore from a headless test: tests/boot/run-input-test.sh drives
# the pointer through QEMU's input layer and then runs this to read back
#
#   Events <queued> queued, <merged> merged, <dropped> dropped
#
# `dropped` is the number that has to stay 0 under that flood. Motion coalesces
# onto motion at enqueue (c/kernel/gui/input/evq.c), so an unbounded stream of samples
# occupies one slot and cannot evict a queued click.
#
# The rest of the block (uptime, memory, barriers, process list) comes along
# because sysinfo is one text answer, and printing all of it means this script
# does not have to be edited every time the kernel learns to report something
# new.

from std.abi import sysinfo

def main() -> None:
    data = buffer(4096)
    count = sysinfo(data, len(data))
    # A failed syscall is not a zero-length report. Keep the error visible to
    # callers, and never pass a negative/oversized count to the raw text view.
    if count < 0 or count > len(data):
        raise IOError("SYS_SYSINFO returned an invalid report length")
    unsafe:
        report = mem2str(data, count)
    print(report)
