/* /bin/closefull -- THE GATE for "make close() able to report a failed
 * write" (audit item 3 / CLAUDE.md structural gap #3's tail).
 *
 * file_close() (c/kernel/exec/file.c) used to be void and SYS_CLOSE
 * (c/kernel/exec/syscall.c) returned 0 unconditionally, so the ONLY symptom
 * of a write that never made it to disk was that the file quietly was not
 * there -- exactly the shape this tree refuses for flock() returning 0 when
 * it holds no lock. This program exercises the fix through the REAL mini-libc
 * path (open/write/close from c/apps/libc/src/io.c, the one this change
 * touched), not a reimplementation of it.
 *
 * Run against an image tests/boot/mkdiskfull.py has deliberately left with
 * only a handful of free blocks (see run-closefull-test.sh), it:
 *
 *   1. Writes a SMALL file first and demands close() return 0. This is the
 *      control INSIDE the gate: without it, "close() reported a failure"
 *      would be indistinguishable from "close() now always reports a
 *      failure", which is not the claim -- see CLAUDE.md rule 5, a control
 *      that cannot be watched failing is worse than none. This one CAN be
 *      watched failing: run it against a filesystem with 0 free blocks and
 *      CLOSETEST-SMALL goes red too, for a reason that has nothing to do
 *      with the big write below.
 *   2. Writes a BIG file, bigger than what mkdiskfull.py left free, and
 *      demands close() return NONZERO -- the actual gate.
 *
 * Both results are printed as `rc=` lines the boot harness greps; nothing
 * here decides pass/fail itself, matching test-fdstream's own division of
 * labour between the on-device instrument and the host-side assertion. */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void report(const char *tag, int rc)
{
    printf("%s rc=%d errno=%d\n", tag, rc, rc < 0 ? errno : 0);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/diskfull_test.bin";
    long size = argc > 2 ? atol(argv[2]) : 65536;

    int sfd = open("/diskfull_small.bin", O_WRONLY | O_CREAT | O_TRUNC);
    if (sfd < 0) {
        printf("CLOSETEST-SMALL open-failed errno=%d\n", errno);
    } else {
        char sbuf[16] = "small-write-ok";
        write(sfd, sbuf, sizeof sbuf);
        report("CLOSETEST-SMALL", close(sfd));
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("CLOSETEST-BIG open-failed errno=%d\n", errno);
        printf("CLOSETEST-DONE\n");
        return 1;
    }

    /* write() itself only buffers into the kernel's per-fd `backing` heap
     * block (file_write() in file.c) -- the real disk allocation and the
     * failure it can hit do not happen until the whole-file flush at close().
     * So a short write here is not expected and is not what this gate is
     * about; only close()'s return matters. */
    char buf[4096];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (char)('A' + (i % 26));
    long left = size;
    while (left > 0) {
        long n = left < (long)sizeof buf ? left : (long)sizeof buf;
        long w = write(fd, buf, n);
        if (w <= 0) break;
        left -= w;
    }
    report("CLOSETEST-BIG", close(fd));
    printf("CLOSETEST-DONE\n");
    return 0;
}
