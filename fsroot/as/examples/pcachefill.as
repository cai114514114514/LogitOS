# pcachefill.as -- put the page cache under the only pressure this machine can
# actually produce, and read the ceiling off the kernel's own counters.
#
# WHY THIS EXISTS. Until elf_load started building file-backed VMAs (465aecc13)
# nothing on this machine ever put a page in the page cache, so its POOL had
# never been approached by anything and its full-pool behaviour had never run.
# That behaviour was: hand the page back with no entry behind it, silently, and
# leak the frame -- see c/kernel/mm/pcache.h. Measuring it needs a workload that
# maps a lot of file data at once and KEEPS it mapped, which is exactly what an
# exec does not do (a program's text is demand-paged, so it only ever pulls in
# the pages it runs).
#
# WHY A SCRIPT AND NOT A NEW BINARY -- the same argument pcachecheck.as makes:
# /bin/as already has raw syscalls, so the one call userland cannot otherwise
# reach (SYS_MMAP_FILE; c/apps/libc/src/mman.c still refuses every non-anonymous
# request) needs nothing new on the disk to drive it.
#
# THE THREE CEILINGS IT RUNS INTO, all real and all reported:
#   pcache pool     c/kernel/mm/pcache.c, sized from RAM
#   pcache files    PCACHE_MAXFILE = 32 file slots, system-wide
#   VMA_MAXAREA     32 areas per ADDRESS SPACE (c/kernel/mm/vma.h) -- and this
#                   is the one that binds here, because /bin/as itself already
#                   holds five (text, rodata, bss, stack, the malloc arena).
#                   So this program maps as many files as it can and STOPS when
#                   the kernel starts refusing, rather than assuming a count.
#
# IT DOES NOT ABORT ON A REFUSAL. A path that is not on this disk, a file the
# cache will not take, an area the space has no room for: each is skipped and
# counted. The harness (tests/boot/run-pcachefill.sh) decides whether what was
# achieved is enough to prove anything, and says so out loud when it is not --
# "the workload did not happen" and "the cache misbehaved" are different
# findings and only one of them is the kernel's.

SYS_OPEN = 54
SYS_CLOSE = 55
SYS_LSEEK = 57
SYS_MEMINFO = 94
SYS_MMAP_FILE = 162

O_RDONLY = 0
SEEK_END = 2
MMAP_PROT_READ = 1

MMCTL_REPORT = 1
MMCTL_AUDIT = 2
MMCTL_RECLAIM = 3
MMCTL_STATS = 4

Req = layout("logit_mmap_file_req", 32, [
    ["hint", 0, 8, "u"],
    ["len", 8, 8, "u"],
    ["off", 16, 8, "u"],
    ["fd", 24, 4, "i"],
    ["prot", 28, 4, "i"]
])

# Ordered LARGEST FIRST, because the binding limit is the number of AREAS and
# not the number of bytes: with at most ~26 areas to spend, spending them on
# the biggest files is the only way to get a page count worth measuring. The
# list is what this disk image actually carries (Makefile's $(DISK) recipe);
# anything missing is skipped, so the same script survives the image changing.
PATHS = [
    "/browser.aex",
    "/model.lm",
    "/fonts/ui.ttf",
    "/preview.aex",
    "/bin/imgcheck",
    "/bin/jsbench",
    "/fonts/text.ttf",
    "/terminal.aex",
    "/bin/vidcheck",
    "/bin/audiocheck",
    "/bin/as",
    "/bin/ch.aex",
    "/bin/h2check",
    "/bin/lm",
    "/bin/libmcheck",
    "/bin/libctest",
    "/bin/thrtest",
    "/bin/libctest2",
    "/studio.aex",
    "/monitor.aex",
    "/files.aex",
    "/settings.aex",
    "/gallery.aex",
    "/clock.aex",
    "/widgets.aex",
    "/textedit.aex"
]

# How many of the files above are mapped a SECOND time, through a SECOND open.
# This is not padding: it is the only way this program can manufacture the case
# reclaim's eligibility test is really about -- a page with TWO PTEs pointing at
# it AND a page-cache entry, where rmap_count(2) + pcache_holds(1) has to equal
# pmm_refcount(3). One mapping proves nothing about that sum; a wrong term
# would still add up at 1 + 1 == 2.
DOUBLE = 3

print("PCFILL-BEGIN")

files = 0
pages = 0
refused = 0
shared_pages = 0
sink = 0
idx = 0

for p in PATHS:
    fd = syscall(SYS_OPEN, addr(p), O_RDONLY)
    if fd < 0:
        refused = refused + 1
    else:
        sz = syscall(SYS_LSEEK, fd, 0, SEEK_END)
        if sz <= 0:
            refused = refused + 1
        else:
            r = Req()
            r.hint = 0
            r.len = sz
            r.off = 0
            r.fd = fd
            r.prot = MMAP_PROT_READ
            base = syscall(SYS_MMAP_FILE, addr(r), 0, 0)
            if base <= 0:
                # The address space is out of areas, or the cache is out of file
                # slots. Either is a real ceiling and is reported as one.
                refused = refused + 1
            else:
                # TOUCH EVERY PAGE. Reserving is free -- the frames arrive on
                # the first read of each page and not before, which is the whole
                # point of a file-backed VMA, so a program that maps and does
                # not touch puts NOTHING in the cache and would measure nothing.
                off = 0
                n = 0
                while off < sz:
                    sink = sink + peek8(base + off)
                    n = n + 1
                    off = off + 4096
                files = files + 1
                pages = pages + n
                print("PCFILL-MAP", p, sz, n)

                # The second, INDEPENDENT mapping: a second open (so a second
                # open file description), a second VMA, the same inode. Every
                # page it touches is a page cache HIT that installs a second
                # PTE onto a frame the cache already holds.
                if idx < DOUBLE:
                    fd2 = syscall(SYS_OPEN, addr(p), O_RDONLY)
                    if fd2 >= 0:
                        r2 = Req()
                        r2.hint = 0
                        r2.len = sz
                        r2.off = 0
                        r2.fd = fd2
                        r2.prot = MMAP_PROT_READ
                        b2 = syscall(SYS_MMAP_FILE, addr(r2), 0, 0)
                        if b2 > 0:
                            off = 0
                            while off < sz:
                                sink = sink + peek8(b2 + off)
                                off = off + 4096
                            shared_pages = shared_pages + n
                        syscall(SYS_CLOSE, fd2)
                idx = idx + 1
        # The fd is closed as soon as the mapping exists, and the mapping keeps
        # working: vma_reserve_file() took its OWN reference on the cache entry
        # (c/kernel/mm/mmsys.c says so where it puts ours back). Closing matters
        # because an open fd on this machine costs the WHOLE FILE in kernel heap
        # (c/kernel/exec/file.c:343) -- holding 26 of them open would be ~19 MiB
        # of kheap for nothing, and would charge this measurement to the wrong
        # subsystem.
        syscall(SYS_CLOSE, fd)

# The checksum exists so the touches cannot be optimised away and so a page
# that faulted in as garbage would change a number somebody can see.
print("PCFILL-MAPPED", files, pages, shared_pages, refused, sink)

# Every mapping above is STILL LIVE at this point -- that is the whole design of
# this program. So the reclaim pass below runs against a machine whose page
# cache is full of pages that are mapped once and pages that are mapped twice,
# which is the only state in which reclaim's three-number agreement can be
# observed doing anything.
got = syscall(SYS_MEMINFO, 0, MMCTL_RECLAIM, 1024)
print("PCFILL-RECLAIM asked 1024 got", got)

syscall(SYS_MEMINFO, 0, MMCTL_STATS, 0)
syscall(SYS_MEMINFO, 0, MMCTL_REPORT, 0)
bugs = syscall(SYS_MEMINFO, 0, MMCTL_AUDIT, 0)
print("PCFILL-AUDIT", bugs)
print("PCFILL-END")
