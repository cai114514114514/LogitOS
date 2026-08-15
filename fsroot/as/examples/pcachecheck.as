# pcachecheck.as -- the page cache's headline claim, proved on the real
# machine: read() and mmap() of the SAME file return THE SAME MEMORY, not two
# copies (c/kernel/mm/pcache.h's "WHY THIS IS NOT bcache.c"). Driven by
# tests/boot/run-mm-test.sh, section 5.
#
# WHY THIS PROGRAM AND NOT A NEW C ONE. /bin/as already has raw memory access
# (peek8) and raw syscalls (syscall()) for exactly this kind of on-device
# proof -- see the fault-containment check earlier in the same harness, which
# writes and runs a one-line .as program rather than shipping a new binary.
# No new binary needs to go on the disk image to drive a syscall nothing else
# in userland calls yet.
#
# SYS_MMAP_FILE IS 162 and MMAP_PROT_{READ,WRITE} ARE 1/2
# (include/abi/logit_abi.h). Spelled out here as literals rather than reached
# through fsroot/as/lib/abi.as: that file is GENERATED (tools/gen_abi.py) from
# include/abi/logit_calls.abi, and SYS_MMAP itself has never been added to
# either -- every mmap-shaped syscall has always been a RAW syscall from
# AetherScript's point of view, not a wrapped one. This stays consistent with
# that rather than special-casing the new call.
#
# THE PROOF, IN THREE PARTS:
#   1. BYTES.  What mmap() hands back matches what was written, byte for byte
#      -- checked against every byte of CONTENT, not one sample.
#   2. ONCE.   Two INDEPENDENT mappings (two separate open()s, two separate
#      VMAs) of the SAME file fault in from ONE device read, not two -- the
#      exact thing tests/unit/mm_pcache_test.c's -DPCACHE_PER_OPEN negative
#      control breaks: keyed per OPEN instead of per INODE, the second
#      mapping would be a second miss into a second, independent frame. Read
#      off the one number nothing else can fake: the [pcache] report line's
#      hit/miss/shared counts, printed via SYS_MEMINFO's diagnostic door.
#   3. REFUSED. A writable file mapping gets LOGIT_MMAP_FILE_E_WRITE (-1), not
#      0 (which would read as "try again") and not a silently-downgraded
#      private copy whose writes go nowhere.

SYS_MMAP_FILE = 162
MMAP_PROT_READ = 1
MMAP_PROT_WRITE = 2
SYS_OPEN = 54
SYS_MEMINFO = 94
MMCTL_REPORT = 1
O_RDONLY = 0

Req = layout("logit_mmap_file_req", 32, [
    ["hint", 0, 8, "u"],
    ["len", 8, 8, "u"],
    ["off", 16, 8, "u"],
    ["fd", 24, 4, "i"],
    ["prot", 28, 4, "i"]
])

PATH = "/pcachecheck.dat"
CONTENT = "PCACHE-IDENTITY-0123456789-the-same-frame"

print("pcachecheck: start")

wrote = file_write(PATH, CONTENT)
if wrote != len(CONTENT):
    print("PC_FAIL write", wrote)
else:
    # Two SEPARATE opens of the SAME path -- two open file DESCRIPTIONS, which
    # is the distinction pcache.h's design section calls out by name: the
    # cache is keyed on the FILE (dev, ino), not on either of these.
    fd1 = syscall(SYS_OPEN, addr(PATH), O_RDONLY)
    fd2 = syscall(SYS_OPEN, addr(PATH), O_RDONLY)
    print("PC_FD", fd1, fd2)
    if fd1 < 0 or fd2 < 0:
        print("PC_FAIL open")
    else:
        r1 = Req()
        r1.hint = 0
        r1.len = 4096
        r1.off = 0
        r1.fd = fd1
        r1.prot = MMAP_PROT_READ
        base1 = syscall(SYS_MMAP_FILE, addr(r1), 0, 0)
        print("PC_BASE1", base1)

        # --- part 1: the bytes -------------------------------------------
        ok = base1 > 0
        if ok:
            i = 0
            while i < len(CONTENT):
                if peek8(base1 + i) != ord(CONTENT[i]):
                    ok = false
                    print("PC_MISMATCH", i, peek8(base1 + i), ord(CONTENT[i]))
                i = i + 1
        print("PC_BYTES", "ok" if ok else "FAIL")

        # --- part 2: the second, independent mapping ----------------------
        r2 = Req()
        r2.hint = 0
        r2.len = 4096
        r2.off = 0
        r2.fd = fd2
        r2.prot = MMAP_PROT_READ
        base2 = syscall(SYS_MMAP_FILE, addr(r2), 0, 0)
        print("PC_BASE2", base2)
        # Different addresses (two VMAs) reading the same first byte -- this
        # alone is consistent with two copies. The [pcache] line below is
        # what is NOT consistent with two copies.
        same_byte = base2 > 0 and peek8(base2) == peek8(base1)
        print("PC_SECOND", "ok" if same_byte else "FAIL")

        # THE FALSIFIABLE LINE. mm_report()'s pcache_report() prints
        # "[pcache] on demand: N pages resident ..., H hits, M misses, K
        # shared". Both mappings above already faulted page 0 in by this
        # point, so the harness requires misses == 1 (one real device read
        # served BOTH mappings) and shared >= 1 (the frame both faults
        # landed on is referenced more than once) -- exactly what the
        # PCACHE_PER_OPEN control cannot produce.
        syscall(SYS_MEMINFO, 0, MMCTL_REPORT, 0)

        # --- part 3: a writable file mapping, refused ----------------------
        r3 = Req()
        r3.hint = 0
        r3.len = 4096
        r3.off = 0
        r3.fd = fd1
        r3.prot = MMAP_PROT_READ | MMAP_PROT_WRITE
        rw = syscall(SYS_MMAP_FILE, addr(r3), 0, 0)
        print("PC_WRITE_REFUSED", rw)

        # --- extra: an fd that names no open file at all -------------------
        # Bounds-checked well before pcache is ever asked about it
        # (proc_fd_get returns NULL past NFD) -- confirms a bad fd is refused
        # rather than mapping whatever garbage happens to sit in that slot.
        r4 = Req()
        r4.hint = 0
        r4.len = 4096
        r4.off = 0
        r4.fd = 9999
        r4.prot = MMAP_PROT_READ
        bad = syscall(SYS_MMAP_FILE, addr(r4), 0, 0)
        print("PC_BADFD", bad)

print("pcachecheck: done")
