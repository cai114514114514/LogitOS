# aether: 3.0
# storprobe -- the Step-0 capability matrix for the storage-kernel wave
# (2026-08-30). Driven by tests/boot/run-storage-test.sh.
#
# WHAT THIS MEASURES AND WHY A SCRIPT. c/apps/browser/js_idb.c and js_cache.c
# both refuse durability with the same sentence: "there is no VFS positional
# write to build real durability on (CLAUDE.md structural gap #3)". That
# sentence was written before SYS_CLOSE could report a failed flush, before
# SYS_FSYNC, and before the fd layer grew a seek cursor -- so before anything
# is built on top of it, the ACTUAL capabilities of ring 3 on the DISK
# filesystem have to be measured, not quoted. /bin/sh cannot reach the paths
# that matter (it has no lseek, no O_APPEND open, no rename-over-existing
# probe), and no coreutil calls ftruncate; an .as script driving raw
# syscall() is the established shape for exactly this (argvlimit.as,
# pcachecheck.as, durcheck.as) and adds no new binary to the disk.
#
# Every test prints one "STORPROBE <name> rc=<...>" line so the harness can
# build the op x result matrix from the serial log alone. NOTHING here asserts
# what the answer should be yet: this run is the MEASUREMENT; the gates that
# assert land with the fix and cite these numbers.
#
# Numbers spelled out, not imported from abi.as, for argvlimit.as's reason: a
# generated import would make this script silently agree with whatever the
# table says today, and the point is to catch the kernel disagreeing with the
# documented ABI.

SYS_WRITE = 1
SYS_DELETE_FILE = 17
SYS_OPEN = 54
SYS_CLOSE = 55
SYS_READ = 56
SYS_LSEEK = 57
SYS_RENAME = 65
SYS_FSYNC = 74
SYS_FTRUNCATE = 189     # the number the storage wave claims; -1 == absent
                        # (checked against the pre-fix kernel, where EVERY
                        # number dispatches to the default and answers -1)

O_RDONLY = 0
O_WRONLY = 1
O_RDWR = 2
O_CREAT = 256
O_TRUNC = 512
O_APPEND = 1024
SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2

P = "/sp/a.bin"

# Read a whole file through the fd path (not the file_read builtin, which is
# SYS_READ_FILE -- a different door). Returns the length; leaves the bytes at
# RD_BUF for peek8().
#
# RD_BUF is allocated at TOP LEVEL, not in here, and that is a language rule
# rather than style: an assignment inside a function creates a LOCAL, so
# `RD_BUF = alloc(...)` in here would leave the top-level RD_BUF nil and every
# later peek8(addr(RD_BUF)) failing exactly the way the first draft of this
# script did on the machine. alloc() zeroes, which test 4 depends on: a hole
# read back as non-zero bytes is FILE content, not leftovers of a previous
# read sitting in this buffer.
def rd_len(path: str) -> i64:
    unsafe:
        fd = syscall(SYS_OPEN, addr(path), O_RDONLY, 0)
        if fd < 0:
            return -1
        n = syscall(SYS_READ, fd, addr(RD_BUF), 8192)
        syscall(SYS_CLOSE, fd, 0, 0)
        if n < 0:
            return -1
        return n

def rep(ch: str, n: i64) -> str:
    out = ""
    for i in range(n):
        out = out + ch
    return out

def nonzero_hole(from_i: i64, to_i: i64) -> i64:
    unsafe:
        # count non-zero bytes in [from_i, to_i) of RD_BUF
        bad = 0
        for i in range(from_i, to_i):
            if peek8(addr(RD_BUF) + u64(i)) != 0:
                bad = bad + 1
        return bad

RD_BUF = buffer(8192)

def main() -> i64:
    unsafe:



        # ---- 1. open + O_TRUNC + full rewrite (the whole-store pattern) ------------
        s64 = rep("A", 64)
        fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_CREAT + O_TRUNC, 0)
        w = -99
        c = -99
        if fd >= 0:
            w = syscall(SYS_WRITE, fd, addr(s64), 64)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        print("STORPROBE trunc-rewrite open", fd, "write", w, "close", c, "readback", n)

        # ---- 2. append --------------------------------------------------------------
        s16 = rep("b", 16)
        fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_APPEND, 0)
        w = -99
        c = -99
        if fd >= 0:
            w = syscall(SYS_WRITE, fd, addr(s16), 16)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        tail_ok = 0
        head_ok = 0
        if n == 80:
            if peek8(addr(RD_BUF) + 63) == ord("A"):
                head_ok = 1
            if peek8(addr(RD_BUF) + 64) == ord("b"):
                tail_ok = 1
        print("STORPROBE append write", w, "close", c, "readback", n,
              "byte63=A", head_ok, "byte64=b", tail_ok)

        # ---- 3. lseek mid-file + write in place (O_RDWR, no trunc) ------------------
        fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
        ls = -99
        w = -99
        c = -99
        if fd >= 0:
            ls = syscall(SYS_LSEEK, fd, 4, SEEK_SET)
            w = syscall(SYS_WRITE, fd, addr("XYZ"), 3)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        m0 = peek8(addr(RD_BUF) + 3)
        m1 = peek8(addr(RD_BUF) + 4)
        m2 = peek8(addr(RD_BUF) + 6)
        m3 = peek8(addr(RD_BUF) + 7)
        print("STORPROBE midfile lseek", ls, "write", w, "close", c, "readback", n,
              "b3", m0, "b4", m1, "b6", m2, "b7", m3)

        # ---- 4. grow via seek-past-end + write: is the hole ZERO? -------------------
        # POSIX: a write past EOF makes bytes [old_end, off) read back as 0. The fd
        # layer grows the kmalloc buffer without zeroing the gap, so the fear is that
        # uninitialized kernel heap gets persisted. MEASURED, not assumed.
        fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
        end = -99
        ls = -99
        w = -99
        c = -99
        if fd >= 0:
            end = syscall(SYS_LSEEK, fd, 0, SEEK_END)
            ls = syscall(SYS_LSEEK, fd, end + 16, SEEK_SET)
            w = syscall(SYS_WRITE, fd, addr("Z"), 1)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        hole = -1
        lastb = -1
        if n == end + 17:
            hole = nonzero_hole(end, end + 16)
            lastb = peek8(addr(RD_BUF) + u64(n - 1))
        print("STORPROBE grow-seek end", end, "lseek", ls, "write", w, "close", c,
              "readback", n, "hole-nonzero-bytes", hole, "last-byte", lastb)

        # ---- 5. shrink: what can ring 3 DO today? -----------------------------------
        # (a) in-place write at a lower offset cannot shorten a file
        fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
        w = -99
        c = -99
        if fd >= 0:
            syscall(SYS_LSEEK, fd, 0, SEEK_SET)
            w = syscall(SYS_WRITE, fd, addr("Q"), 1)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        print("STORPROBE shrink-inplace write", w, "close", c, "readback", n)

        # (b) SYS_FTRUNCATE -- absent until the storage wave; the pre-fix kernel
        # answers -1 for it like every number with no dispatch case.
        fd = syscall(SYS_OPEN, addr(P), O_RDWR, 0)
        t = -99
        c = -99
        if fd >= 0:
            t = syscall(SYS_FTRUNCATE, fd, 40, 0)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        print("STORPROBE ftruncate rc", t, "close", c, "readback", n)

        # (c) the only working shrink: O_TRUNC whole rewrite at the smaller length
        s40 = rep("c", 40)
        fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_TRUNC, 0)
        w = -99
        c = -99
        if fd >= 0:
            w = syscall(SYS_WRITE, fd, addr(s40), 40)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        b0 = -1
        if n == 40:
            b0 = peek8(addr(RD_BUF))
        print("STORPROBE trunc-shrink write", w, "close", c, "readback", n, "byte0", b0)

        # ---- 6. rename over an existing file (atomic replace) -----------------------
        file_write("/sp/rep-target", Bytes("OLD"))
        file_write("/sp/rep-src", Bytes("NEW"))
        rc1 = syscall(SYS_RENAME, addr("/sp/rep-src"), addr("/sp/rep-target"), 0)
        rc2 = syscall(SYS_RENAME, addr("/sp/rep-target"), addr("/sp/renamed"), 0)
        back = file_read("/sp/renamed")
        print("STORPROBE rename-replace rc", rc1, "rename-fresh rc", rc2,
              "content-after", back)

        # ---- 7. fsync + explicit error surface --------------------------------------
        fd = syscall(SYS_OPEN, addr(P), O_WRONLY + O_TRUNC, 0)
        w = -99
        fs = -99
        c = -99
        if fd >= 0:
            w = syscall(SYS_WRITE, fd, addr("dddddddd"), 8)
            fs = syscall(SYS_FSYNC, fd, 0, 0)
            c = syscall(SYS_CLOSE, fd, 0, 0)
        n = rd_len(P)
        print("STORPROBE fsync write", w, "fsync", fs, "close", c, "readback", n)

        # ---- 8. the file survives a reopen by a NEW process -------------------------
        # run() (the builtin, not sys.as's wrapper, so this script also parses on the
        # host) forks+execs /bin/as; the child re-reads the file with its OWN fd
        # table, so a flush that never left this process's buffer would read 0 there.
        # The list is [path, argv1...] -- argv0 is supplied by the loader, so the
        # script path is element 1 (the first draft passed "as" as argv1 and the
        # child died with "cannot open as" before reading anything).
        # Migration correction: storchild is now a native A3 executable. The old
        # /bin/as source route above described the VM child; native guest execution
        # must use the packaged artifact until a host build connection is available.
        # A3 makes a running process a SCOPED resource: it is started into a
        # `with` and released on every way out of the block, so a child cannot
        # outlive the scope that made it. The A2 form started and waited on the
        # same unscoped value and leaked the process on an early return.
        p = run(["/usr/as/bin/storchild.aex"])
        with process = p.start():
            code = process.wait()
        print("STORPROBE cross-process child-exit", code)

        syscall(SYS_DELETE_FILE, addr(P), 0, 0)
        syscall(SYS_DELETE_FILE, addr("/sp/renamed"), 0, 0)
        print("STORPROBE-DONE")
        return 0
