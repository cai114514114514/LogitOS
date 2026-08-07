# evqstat.as -- print the machine's SYS_SYSINFO block.
#
# Exists so the window manager's event-ring accounting is readable from a
# script, and therefore from a headless test: tests/boot/run-evq-test.sh floods
# the pointer through QEMU's input layer and then runs this to read back
#
#   Events <queued> queued, <merged> merged, <dropped> dropped
#
# `dropped` is the number that has to stay 0 under that flood. Motion coalesces
# onto motion at enqueue (c/kernel/gui/evq.c), so an unbounded stream of samples
# occupies one slot and cannot evict a queued click.
#
# The rest of the block (uptime, memory, barriers, process list) comes along
# because sysinfo is one text answer, and printing all of it means this script
# does not have to be edited every time the kernel learns to report something
# new.

from abi import sysinfo

_b = buffer(4096)
_n = sysinfo(_b, 4096)
print(mem2str(_b, _n))
