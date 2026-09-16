# aether: 3.0
# oomsmall -- the INNOCENT process. It asks for very little and must survive.
#
#   as /usr/as/examples/oomsmall.as <mib>
#
# This is the program the bug kills. c/kernel/mm/virt/fault.c used to return 0 when
# memory was gone, which ends the FAULTING process -- and on a machine one hog
# has emptied, the next process to fault is whoever the user just started. It is
# never the hog: the hog already has its memory and is not asking for more.
#
# So the whole assertion of tests/boot/run-oom-test.sh is: this program prints
# OOMSMALL-OK. Not "something died" -- something dies either way. This one
# living is the difference between the two behaviours.
#
# ---------------------------------------------------------------------------
# WHY IT WAITS, AND WHY IT WATCHES THE MACHINE RATHER THAN THE CLOCK
#
# It has to be RUNNING before memory runs out, not started afterwards, and that
# is not a stylistic choice -- it is forced. Measured 2026-08-20 on a 320 MiB
# machine with a 200 MiB hog resident and 64 MiB still free:
#
#     [execve] /bin/as: bad aex header
#
# /bin/as (about 1 MiB) cannot be exec'd a second time once the machine is in
# that state. The first exec of the same binary, minutes earlier, worked; both
# execs work on a 512 MiB machine; /bin/echo and /bin/uname keep working
# throughout; kmalloc never refused (no [oom] line, no failed kheap grow) and
# 16,572 frames were free. So the file read comes back wrong under pressure --
# a real defect somewhere in the loader or the filesystem, reported rather than
# worked around, and NOT this program's business. Starting early sidesteps it.
#
# The wait is on the machine's OWN free-frame count (SYS_MEMINFO), not on a
# sleep: a timed wait would be a number calibrated against one host's speed
# under TCG, and when it drifted the failure would read as "the killer does not
# work" rather than "the hog had not finished yet". Polling the fact means the
# program allocates at exactly the moment there is nothing left to allocate.

SYS_MMAP = 92
SYS_MUNMAP = 93
SYS_MEMINFO = 94
SYS_YIELD = 12

WORDS = 512

# struct logit_meminfo (include/abi/logit_abi.h, and modelled in
# fsroot/as/lib/abi.as which agrees): frames_free is at byte 16, so word 2.
MI_FRAMES_FREE = 2

# Start allocating once the machine has less than this many frames free.
# MEASURED on the gate's own machine (320 MiB), 2026-08-20: 67,821 frames free
# with the desktop and this program up, and 16,621 left once the 200 MiB hog is
# resident. 20,000 sits between those two and nowhere near either, so it fires
# after the hog has finished and never before it has started.
LOWMARK = 20000

# Bounded, so a run where the hog never fills memory ENDS and says so rather
# than hanging until the harness gives up -- those are different findings.
#
# MEASURED the same day: this loop runs about 333,000 polls per second under
# TCG (4,000,000 went by in the 12 s between the two programs starting), and the
# hog needs two to four minutes to write 200 MiB. The first version of this file
# used 4,000,000 and therefore STOPPED WAITING before the hog had allocated
# anything: it found 67,821 frames free, allocated happily, and printed
# OOMSMALL-OK. That is a false pass -- the one assertion this harness rests on,
# satisfied without the machine ever running out of memory. The budget covers
# ten minutes of hog now, and the free-frame condition ends the wait long before
# it is reached.
MAXPOLL = 200000000

def mark(i: i64) -> i64:
    return i * 1000003 + 41


def map_anon(size: i64) -> i64:
    unsafe:
        return syscall(SYS_MMAP, size, 3, 0)


def unmap(base: i64, size: i64) -> i64:
    unsafe:
        return syscall(SYS_MUNMAP, base, size)


def yield_() -> i64:
    unsafe:
        return syscall(SYS_YIELD, 0, 0, 0)


def read_meminfo(mbuf: i64) -> i64:
    unsafe:
        return syscall(SYS_MEMINFO, mbuf, 0, 0)


# The three pointer helpers below exist because A3 requires pointer construction
# and dereference to be inside unsafe, and the frames-free word is read from
# inside a WHILE CONDITION. Putting the block around the loop would widen unsafe
# over the whole poll; naming the read keeps it one word wide and leaves the
# loop reading as the free-frame poll it is.
def free_frames(mbuf: i64) -> i64:
    unsafe:
        mp = i64ptr(u64(mbuf))
        return mp[MI_FRAMES_FREE]


def clear_free_frames(mbuf: i64) -> None:
    unsafe:
        mp = i64ptr(u64(mbuf))
        mp[MI_FRAMES_FREE] = 0


# Written AND read back in one place. A run that survived by being handed a
# frame somebody else still maps fails here instead of passing -- the failure
# mode a killer creates, which a "did it exit cleanly" check cannot see.
def touch_and_verify(base: i64, npages: i64) -> i64:
    unsafe:
        p = i64ptr(u64(base))
        i = 0
        while i < npages:
            p[i * WORDS] = mark(i)
            i = i + 1
        print("OOMSMALL-TOUCHED", npages)

        bad = 0
        i = 0
        while i < npages:
            got = p[i * WORDS]
            if got != mark(i):
                if bad < 4:
                    print("OOMSMALL-BAD page", i, "got", got, "want", mark(i))
                bad = bad + 1
            i = i + 1
        return bad


def main() -> i64:
    a = args()
    mib = 8
    if len(a) > 1:
        if a[1] == "4":
            mib = 4
        if a[1] == "16":
            mib = 16
        if a[1] == "32":
            mib = 32
        if a[1] == "64":
            mib = 64
        if a[1] == "96":
            mib = 96

    npages = mib * 256
    span = mib * 1024 * 1024

    print("OOMSMALL-START", mib, "MiB,", npages, "pages")

    # One page to receive the meminfo struct. Taken FIRST, while memory is still
    # plentiful: an innocent process that could not even allocate its own scratch
    # would be testing the wrong thing.
    mbuf = map_anon(4096)
    if mbuf < 4096:
        print("OOMSMALL-FAIL could not map the meminfo page:", mbuf)
    else:
        clear_free_frames(mbuf)
        read_meminfo(mbuf)
        print("OOMSMALL-READY free frames now", free_frames(mbuf))

        polls = 0
        free = free_frames(mbuf)
        while free >= LOWMARK and polls < MAXPOLL:
            yield_()
            polls = polls + 1
            if polls % 2000 == 0:
                read_meminfo(mbuf)
                free = free_frames(mbuf)

        print("OOMSMALL-GO free frames", free, "after", polls, "polls")

        base = map_anon(span)
        if base < 4096:
            # A refused mmap is a RESERVATION failure, not an out-of-memory: no
            # frame is taken until the page is touched. Reported separately so it
            # cannot be read as the thing this test is about.
            print("OOMSMALL-FAIL mmap refused:", base)
        else:
            bad = touch_and_verify(base, npages)

            if bad == 0:
                print("OOMSMALL-OK", npages, "pages written and read back")
            else:
                print("OOMSMALL-CORRUPT", bad, "pages wrong")

            unmap(base, span)

    print("OOMSMALL-DONE")
    return 0
