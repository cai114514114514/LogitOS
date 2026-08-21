# crashme -- fault on purpose, at an address chosen in advance.
#
#   as /usr/as/examples/crashme.as
#
# The fixture for `make test-coredump-os`. It stores to 0xdeadbee0, which is
# outside every area a program on this machine has, so the CPU raises vector 14
# and c/kernel/cpu/interrupts.c takes the ring-3 fault path -- which is where
# c/kernel/exec/coredump.c writes the dump.
#
# WHY AN .as SCRIPT AND NOT A C PROGRAM. The C one exists too --
# c/apps/coreutils/crash.c, which additionally loads sentinel values into
# r12..r15, rbx and rdi so the dump's whole register file has an oracle written
# down before the fault. It is NOT on the disk: the Makefile's CLI list is
# hand-written (Makefile:460) and this work does not own that file.
# `fsroot/as/examples/*.as` is a WILDCARD (Makefile:25), so this script reaches
# /usr/as/examples/ with no build-system change at all, which is why the
# on-device gate is written against it and why it is worth having both.
#
# WHAT IT CAN STILL PROVE WITHOUT SENTINELS, and it is the property the gate is
# named for: THE ADDRESS IS WRITTEN DOWN HERE, in this file, before anything
# runs. When the kernel's [fault] line reports cr2=0xdeadbee0, and the dump's
# LOGIT note reports 0xdeadbee0, and the dump's NT_SIGINFO si_addr reports
# 0xdeadbee0, that is three readings agreeing with a number that came from a
# fourth place. rip and rsp have no such advance oracle, so the harness compares
# them between the two channels the kernel itself prints -- the [fault] line,
# which quotes the trap frame, and the [core] line, which quotes the FILE.
#
# poke64 is CAP_RAW-gated (c/apps/as/as_native.c) and /bin/as grants CAP_RAW by
# default (as.c:111), so this runs as an ordinary program with no special
# launch. If that default ever narrows this script stops faulting and starts
# refusing, and the harness would otherwise read that as "the dump was not
# written" -- so the line below is printed first and the log says which of the
# two happened.

ADDR = 0xdeadbee0

print("CRASHME about to store to", ADDR)
poke64(ADDR, 0x1234)

# Reached only if that address was writable, which is a finding of its own
# rather than a pass: the harness greps for this line and fails on it.
print("CRASHME DID NOT FAULT -- that address is mapped")
