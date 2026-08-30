# storgate -- the ON-DEVICE gate for SYS_FTRUNCATE and the zero-filled hole
# (storage wave, 2026-08-30). Driven by tests/boot/run-storage-test.sh, which
# boots TWICE against one persistent disk copy:
#
#   boot 1  this script, phase "run": write -> rewrite at a different length
#           -> grow (ftruncate, must be zeros in the gap) -> shrink
#           (ftruncate) -> mid-file rewrite via lseek+write -> re-read every
#           time, byte-verified -> leave a known final image on disk.
#   boot 2  this script, phase "verify": read the final image back and check
#           it byte-for-byte. A write that never left the first boot's
#           buffers cannot pass this; only the disk can.
#
# The red side of this gate is the PRE-FIX measurement, not a stubbed kernel:
# fsroot/as/examples/storprobe.as against the unmodified kernel answered
# "ftruncate rc -1" and "hole-nonzero-bytes 10 of 16" (2026-08-30, recorded in
# tests/storage.mk's header), which is the same asserts failing before the
# syscall existed. The host half and its -DSTORAGE_NEGCTL control are
# tests/unit/storage_test.c.
#
# Numbers spelled out, for argvlimit.as's reason: a generated import would
# agree with whatever the table says, and the point is to catch disagreement.

SYS_WRITE = 1
SYS_DELETE_FILE = 17
SYS_OPEN = 54
SYS_CLOSE = 55
SYS_READ = 56
SYS_LSEEK = 57
SYS_FSYNC = 74
SYS_FTRUNCATE = 189

O_RDONLY = 0
O_WRONLY = 1
O_RDWR = 2
O_CREAT = 256
O_TRUNC = 512
SEEK_SET = 0
SEEK_END = 2

P = "/sg/data.bin"

# Top-level alloc: an assignment inside a function creates a LOCAL in this
# language, so the buffer has to be made here (see storprobe.as's note).
BUF = alloc(8192)

def check(ok, what):
    # a function may READ this global and CALL a builtin to mutate --
    # append() on a list reaches the list object itself, no assignment.
    if ok:
        print("STORGATE-ok", what)
    else:
        print("STORGATE-FAIL", what)

def rd_len(path):
    fd = syscall(SYS_OPEN, addr(path), O_RDONLY, 0)
    if fd < 0:
        return -1
    n = syscall(SYS_READ, fd, addr(BUF), 8192)
    syscall(SYS_CLOSE, fd, 0, 0)
    if n < 0:
        return -1
    return n

def rep(ch, n):
    out = ""
    for i in range(n):
        out = out + ch
    return out

def bytes_are(ch, from_i, to_i):
    ok = 1
    want = ord(ch)
    for i in range(from_i, to_i):
        if peek8(addr(BUF) + i) != want:
            ok = 0
    return ok

def zeros(from_i, to_i):
    ok = 1
    for i in range(from_i, to_i):
        if peek8(addr(BUF) + i) != 0:
            ok = 0
    return ok

def phase_run():
    # 1. fresh write, 300 'a's
    fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_CREAT + O_TRUNC, 0)
    a300 = rep("a", 300)
    w = syscall(SYS_WRITE, fd, addr(a300), 300)
    c = syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    print("STORGATE-detail write-300 w=", w, " close=", c, " n =", n)
    check(w == 300 and c == 0 and n == 300 and bytes_are("a", 0, 300),
          "write-300 verified")

    # 2. whole rewrite at a DIFFERENT (smaller) length: 120 'b's
    fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_TRUNC, 0)
    b120 = rep("b", 120)
    w = syscall(SYS_WRITE, fd, addr(b120), 120)
    syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    print("STORGATE-detail rewrite-120 w=", w, " n=", n)
    check(w == 120 and n == 120 and bytes_are("b", 0, 120),
          "rewrite-120 verified")

    # 3. grow via SYS_FTRUNCATE: the gap must read as ZERO bytes
    fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
    t = syscall(SYS_FTRUNCATE, fd, 200, 0)
    fs = syscall(SYS_FSYNC, fd, 0, 0)
    syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    print("STORGATE-detail ftruncate-grow-200 t =", t, " fsync =", fs, " n =", n)
    check(t == 0 and fs == 0 and n == 200 and bytes_are("b", 0, 120)
          and zeros(120, 200),
          "ftruncate-grow-200 gap is zeros")

    # 4. shrink via SYS_FTRUNCATE: to 60
    fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
    t = syscall(SYS_FTRUNCATE, fd, 60, 0)
    syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    print("STORGATE-detail ftruncate-shrink-60 t =", t, " n =", n)
    check(t == 0 and n == 60 and bytes_are("b", 0, 60),
          "ftruncate-shrink-60 verified")

    # 5. positional rewrite via lseek + write, then the reopen proof
    fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
    ls = syscall(SYS_LSEEK, fd, 10, SEEK_SET)
    w = syscall(SYS_WRITE, fd, addr("MIDFILE"), 7)
    syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    mid = mem2str(addr(BUF) + 10, 7)
    print("STORGATE-detail midfile-rewrite lseek =", ls, " w =", w, " n =", n, " mid =", mid)
    check(ls == 10 and w == 7 and n == 60 and mid == "MIDFILE",
          "midfile-rewrite verified")

    # 6. write past EOF the raw way; the gap must be zeros here too
    fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
    syscall(SYS_LSEEK, fd, 70, SEEK_SET)
    w = syscall(SYS_WRITE, fd, addr("Z"), 1)
    syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    print("STORGATE-detail seek-past-end-gap-zero n =", n)
    check(w == 1 and n == 71 and zeros(60, 70) and peek8(addr(BUF) + 70) == ord("Z"),
          "seek-past-end gap reads as zeros")

    # 7. the whole-store pattern, leaving the image boot 2 verifies
    fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_TRUNC, 0)
    final = "REBOOT-PROOF-V1 " + rep("r", 111)
    w = syscall(SYS_WRITE, fd, addr(final), len(final))
    fs = syscall(SYS_FSYNC, fd, 0, 0)
    c = syscall(SYS_CLOSE, fd, 0, 0)
    n = rd_len(P)
    print("STORGATE-detail final-image w=", w, " fsync =", fs, " n =", n)
    check(w == len(final) and fs == 0 and c == 0 and n == 127,
          "final-image written")

    # 8. refusals must stay refusals
    fd = syscall(SYS_OPEN, addr(P), O_RDONLY, 0)
    t = syscall(SYS_FTRUNCATE, fd, 5, 0)
    syscall(SYS_CLOSE, fd, 0, 0)
    print("STORGATE-detail readonly-ftruncate t =", t)
    check(t == -1, "readonly ftruncate refused")

def phase_verify():
    n = rd_len(P)
    want = "REBOOT-PROOF-V1 " + rep("r", 111)
    got = mem2str(addr(BUF), n)
    print("STORGATE-detail reboot-image n =", n)
    check(n == 127 and got == want, "image across reboot byte-exact")

a = args()
mode = a[1] if len(a) > 1 else "run"
if mode == "run":
    phase_run()
    print("STORGATE-RUN-DONE")
else:
    phase_verify()
    print("STORGATE-VERIFY-DONE")
