# storchild.as -- the second half of storprobe's test 8: re-read /sp/a.bin
# from a NEW process. storprobe wrote 8 bytes and closed; if the fd layer's
# flush had stayed in the first process's kernel buffer this reads -1/0 and
# the parent's "STORPROBE cross-process" line shows it. Prints exactly one
# line the harness can grep.

SYS_OPEN = 54
SYS_CLOSE = 55
SYS_READ = 56

fd = syscall(SYS_OPEN, addr("/sp/a.bin"), 0, 0)
# Top-level alloc on purpose: an assignment inside a function creates a LOCAL
# in this language, so allocating there would leave this global nil and
# addr(buf) below would fail. Same note as storprobe.as's RD_BUF.
buf = alloc(64)
n = -1
if fd >= 0:
    n = syscall(SYS_READ, fd, addr(buf), 64)
    syscall(SYS_CLOSE, fd, 0, 0)
ok = 0
if n == 8:
    if peek8(addr(buf)) == ord("d"):
        if peek8(addr(buf) + 7) == ord("d"):
            ok = 1
print("STORCHILD readback", n, "bytes-ok", ok)
