# mempress -- manufacture memory pressure, from ring 3, and prove the pages
# survived it.
#
#   as /usr/as/examples/mempress.as small|mid|big|huge
#
# WHY THIS IS A PROGRAM AND NOT A KERNEL SELF-TEST
#
# Reclaim is only interesting when it runs underneath something that does not
# know it exists. A kernel selftest that allocates and frees its own pages
# proves the algorithm; it does not prove that a real ring-3 process, faulting
# through the real int-0x80 path with the real scheduler underneath it, gets its
# memory back unchanged. So the pressure is applied from here: map far more than
# the machine has, write to every page, then read every page back.
#
# TWO REGIONS, BECAUSE RECLAIM HAS TWO TIERS AND THEY FAIL DIFFERENTLY.
#
#   the ZERO region   every page is faulted in and left all-zero. The kernel can
#                     throw these away and rebuild them on the next fault with no
#                     disk involved at all -- the cheapest reclaim there is. This
#                     is not a contrived case: browser.aex maps a 105.5 MiB .bss
#                     at launch against a 12.7 MiB heap peak, and the difference
#                     is exactly this, ~93 MiB of frames holding zeroes.
#                     What it proves: the page comes back, still zero.
#
#   the DATA region   every page gets eight words whose values depend on BOTH the
#                     page number and the word's position in it. Nothing can
#                     reconstruct these, so they must go to the swap device and
#                     come back byte for byte.
#                     What it proves: the three ways swap goes wrong --
#                       all pages come back zero    -> dropped instead of written
#                       page N comes back as page M -> the slot arithmetic is off
#                       one word in a page is wrong -> the transfer is off by an
#                                                      offset
#                     A buffer of zeroes, or one constant per page, would pass at
#                     least one of those.
#
# Eight probes rather than the whole page because this is an interpreted
# language: 4096 bytes a page over 65000 pages is minutes of VM time, and eight
# words spread across the page catch every failure above at a fraction of it.
#
# Output is grepped by tests/boot/run-swap-test.sh. MEMPRESS-OK means everything
# came back; MEMPRESS-CORRUPT means the machine lost data, which is the one
# result that must never appear.

SYS_MMAP = 92
SYS_MUNMAP = 93
SYS_MEMINFO = 94

MMCTL_STATS = 4          # see c/kernel/mm/mmsys.c

WORDS = 512              # 8-byte words in a 4096-byte page
PROBES = 8               # words actually written per data page
STRIDE = 64              # 64 words = 512 bytes apart, so probes span the page

# Sizes are named, not parsed: there is no int() builtin. MiB, per region --
# the program maps this much ZERO and this much DATA, so it needs twice this
# much address space and puts twice this much pressure on the machine.
def size_mib(name):
    if name == "small":
        return 16
    if name == "big":
        return 192
    if name == "huge":
        return 256
    return 96

# The value that belongs at word `k` of page `i`, and nowhere else.
def mark(i, k):
    return i * 1000003 + k * 7 + 12345

# Fault in every page of the region and leave it all-zero. Writing zero still
# takes the fault, so the frame is really resident -- it is just resident
# holding nothing, which is the state the drop tier exists for.
def touch_zero(p, npages):
    i = 0
    while i < npages:
        p[i * WORDS] = 0
        i = i + 1

def touch_data(p, npages):
    i = 0
    while i < npages:
        base = i * WORDS
        k = 0
        while k < PROBES:
            p[base + k * STRIDE] = mark(i, k)
            k = k + 1
        i = i + 1

def verify_zero(p, npages):
    bad = 0
    i = 0
    while i < npages:
        got = p[i * WORDS]
        if got != 0:
            if bad < 6:
                print("MEMPRESS-BAD zero page", i, "got", got)
            bad = bad + 1
        i = i + 1
    return bad

def verify_data(p, npages):
    bad = 0
    i = 0
    while i < npages:
        # Progress, every 16 MiB. This phase is where every evicted page has to
        # come back off the disk, so it is by far the slowest part of the run --
        # and without a heartbeat, a harness that gives up early is
        # indistinguishable from a kernel that hung.
        if i % 4096 == 0:
            print("MEMPRESS-VERIFY-AT", i)
        base = i * WORDS
        k = 0
        while k < PROBES:
            got = p[base + k * STRIDE]
            if got != mark(i, k):
                if bad < 6:
                    print("MEMPRESS-BAD page", i, "word", k, "got", got, "want", mark(i, k))
                bad = bad + 1
            k = k + 1
        i = i + 1
    return bad

a = args()
name = a[1] if len(a) > 1 else "mid"
mib = size_mib(name)
npages = mib * 256                      # 1 MiB = 256 pages
bytes = mib * 1024 * 1024

print("MEMPRESS-START", name, mib, "MiB per region,", npages, "pages each")

zbase = syscall(SYS_MMAP, bytes, 3, 0)  # PROT_READ | PROT_WRITE
dbase = syscall(SYS_MMAP, bytes, 3, 0)

# 0 is failure, and nothing below the first page can be a real mapping.
if zbase < 4096:
    print("MEMPRESS-FAIL mmap refused the zero region:", zbase)
elif dbase < 4096:
    print("MEMPRESS-FAIL mmap refused the data region:", dbase)
else:
    print("MEMPRESS-MAPPED", zbase, dbase)
    zp = i64ptr(zbase)
    dp = i64ptr(dbase)

    # Phase 1: make both regions resident. Physical memory runs out somewhere in
    # the middle of this, in an ordinary page fault, with the shell and the
    # window manager running underneath.
    touch_zero(zp, npages)
    print("MEMPRESS-ZEROED", npages)
    touch_data(dp, npages)
    print("MEMPRESS-TOUCHED", npages)
    syscall(SYS_MEMINFO, 0, MMCTL_STATS, 0)

    # Phase 2: read everything back. Whatever was evicted has to be either
    # rebuilt (the zero region) or read off the swap device (the data region).
    zbad = verify_zero(zp, npages)
    print("MEMPRESS-ZERO-VERIFIED", zbad)
    dbad = verify_data(dp, npages)
    syscall(SYS_MEMINFO, 0, MMCTL_STATS, 0)

    if zbad == 0:
        if dbad == 0:
            print("MEMPRESS-OK", npages * 2, "pages verified")
        else:
            print("MEMPRESS-CORRUPT", dbad, "wrong words in the data region")
    else:
        print("MEMPRESS-CORRUPT", zbad, "zero pages came back non-zero")

    syscall(SYS_MUNMAP, zbase, bytes)
    syscall(SYS_MUNMAP, dbase, bytes)
    print("MEMPRESS-UNMAPPED")

print("MEMPRESS-DONE")
