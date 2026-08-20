# elfspin.as -- a process that STAYS ALIVE and does nothing, so that two
# instances of the SAME binary can be observed at the same instant.
#
# WHY THIS EXISTS AT ALL. The claim under measurement is "two processes running
# the same binary SHARE its text". Every word of that is about a moment when
# two of them are RUNNING; a binary that starts, works and exits proves nothing
# about sharing, because the second run's frames are the first run's frames
# returned to the allocator. So the subject has to be made to sit still, and
# nothing on this disk sits still: /bin/sleep POLLS the wall clock and yields
# (c/apps/coreutils/sleep.c), and a background process's stdin is closed to an
# instant EOF by the shell (c/apps/coreutils/sh.c start_pipeline), so blocking
# on a read is not available either.
#
# SYS_NANOSLEEP (84) is, and it BLOCKS -- which matters more than it looks.
# A busy-waiting subject competes with the process taking the measurement for
# the same TCG cores, so the numbers would be taken from a machine under load
# manufactured by the instrument. Three of these sleep at essentially zero CPU.
#
# THE SUBJECT IS /bin/as ITSELF, which is the point of running a SCRIPT rather
# than shipping a new binary: /bin/as is 327,240 bytes with a 249,140-byte
# R E segment and a 29,624-byte R segment (readelf -lW build/as.elf), i.e. 67
# whole pages that elf_file_runs() can share -- large enough that three copies
# of it are visible in a free-frame count, where /bin/sleep's two pages are not.
#
#   as /usr/as/examples/elfspin.as <tag>
#
# Prints ELFSPIN-UP <tag> once it is resident and sleeping. The harness WAITS
# for that line rather than sleeping a guessed number of seconds: "the subject
# had not finished loading yet" and "the sharing did not happen" produce the
# same free-frame reading, and only one of them is a finding.

SYS_NANOSLEEP = 84

Ts = layout("logit_timespec", 16, [
    ["sec", 0, 8, "i"],
    ["nsec", 8, 8, "i"]
])

a = args()
tag = a[1] if len(a) > 1 else "1"

t = Ts()
t.sec = 2
t.nsec = 0

# Printed AFTER the timespec is built, so the line means "this process is fully
# up and about to block", not "this process has been created".
print("ELFSPIN-UP", tag)

# 600 x 2s. Bounded rather than infinite so that a harness which loses its
# QEMU still terminates; nothing is expected to reach the end of it.
i = 0
while i < 600:
    syscall(SYS_NANOSLEEP, addr(t), 0)
    i = i + 1

print("ELFSPIN-DONE", tag)
