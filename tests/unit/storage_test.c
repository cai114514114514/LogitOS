/* storage_test -- SYS_FTRUNCATE and the zero-filled hole, host gate.
 *
 * WHAT IT GATES, and the order it gates it in:
 *   1. file_truncate's contract: grow zero-fills, shrink cuts, no-op does not
 *      schedule a write-back, and every refusal set says -1 rather than
 *      pretending (read-only fd, streamed fd, negative length).
 *   2. file_write's hole rule: a write past EOF leaves the gap ZERO, not
 *      whatever the allocator handed back. This was a measured defect on the
 *      machine (10 of 16 gap bytes non-zero, storprobe.as 2026-08-30), and the
 *      host model's kmalloc poisons its memory with 0xA5 so the control
 *      reproduces it deterministically instead of hoping the host heap was
 *      dirty that day.
 *   3. the flush path the truncate rides: fsync and last-close write the
 *      EXACT new length through the whole-file VFS op, and a refused flush
 *      (modelled full disk) is reported by file_close() as -1 -- the 0/-1
 *      convention SYS_CLOSE translates to -2.
 *
 * THE NEGATIVE CONTROL is -DSTORAGE_NEGCTL (test-storage-negctl): it restores
 * BOTH pre-fix behaviours in c/kernel/exec/fd/file.c -- the hole is left as
 * allocated, and file_truncate answers -1 -- and this suite then FAILS, for
 * exactly the two reasons above, which is the only honest proof the asserts
 * are attached to something. See poll.mk's negctl for the shape.
 *
 * Real code under test: c/kernel/exec/fd/file.c, unmodified, per
 * tests/unit/storhost/hostmodel.c's header.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "file.h"
#include "logit_abi.h"      /* O_*, SEEK_* */

/* the host model's controls (definitions in tests/unit/storhost/hostmodel.c) */
void stor_vfs_reset(void);
int  stor_vfs_writecount(void);
void stor_vfs_set_refuse(int on);
int  stor_vfs_size(void);

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) { failures++; printf("FAIL: %s\n", what); }
    else       printf("ok:   %s\n", what);
}

#define PATH "/store"

int main(void)
{
    file_init();
    stor_vfs_reset();

    /* ---- grow zero-fills (the hole bug's fix, driven through truncate) ---- */
    {
        struct file *f = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        check(f != NULL, "open(O_CREAT|O_TRUNC) works");
        char buf[32];
        memset(buf, 'A', sizeof buf);
        check(file_write(f, buf, 32) == 32, "write 32 bytes");
        check(file_truncate(f, 64) == 0, "ftruncate grow 32 -> 64 returns 0");
        /* f->size is internal; read back through a FRESH read-only fd, which
         * on the model goes through vfs_pread and therefore the flushed bytes
         * only after fsync -- so fsync first, exactly as a durable caller
         * would. */
        check(file_fsync(f) == 0, "fsync after truncate-grow");
        struct file *r = file_open_vfs(PATH, O_RDONLY);
        check(r != NULL, "reopen read-only after grow");
        unsigned char back[128];
        memset(back, 0xEE, sizeof back);
        long n = file_read(r, back, sizeof back);
        check(n == 64, "grown file reads back 64 bytes");
        int hole_bad = 0;
        for (int i = 32; i < 64; i++) hole_bad |= (back[i] != 0);
        check(!hole_bad, "bytes 32..63 of the grown file are ZERO (not 0xA5 heap)");
        file_close(r);
        file_close(f);
    }

    /* ---- shrink cuts, and the flush writes the SHORTER length ------------- */
    {
        stor_vfs_reset();
        struct file *f = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        char buf[100];
        memset(buf, 'B', sizeof buf);
        file_write(f, buf, 100);
        check(file_truncate(f, 40) == 0, "ftruncate shrink 100 -> 40 returns 0");
        check(file_fsync(f) == 0, "fsync after truncate-shrink");
        check(stor_vfs_size() == 40, "whole-file flush landed the NEW length (40)");
        file_close(f);
        struct file *r = file_open_vfs(PATH, O_RDONLY);
        unsigned char back[128];
        long n = file_read(r, back, sizeof back);
        check(n == 40, "reopened file is 40 bytes long");
        int same = 1;
        for (int i = 0; i < 40; i++) same &= (back[i] == 'B');
        check(same, "the surviving 40 bytes are the FIRST 40, intact");
        file_close(r);
    }

    /* ---- a write past EOF zero-fills its own gap (the raw write path) ----- */
    {
        stor_vfs_reset();
        struct file *f = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        char buf[16];
        memset(buf, 'C', sizeof buf);
        file_write(f, buf, 16);
        check(file_lseek(f, 32, SEEK_SET) == 32, "seek 16 -> 32 (past EOF)");
        check(file_write(f, "D", 1) == 1, "write 1 byte at 32");
        check(file_fsync(f) == 0, "fsync after gap write");
        file_close(f);
        struct file *r = file_open_vfs(PATH, O_RDONLY);
        unsigned char back[128];
        long n = file_read(r, back, sizeof back);
        check(n == 33, "file grew to 33 bytes");
        int gap_bad = 0;
        for (int i = 16; i < 32; i++) gap_bad |= (back[i] != 0);
        check(!gap_bad, "the seek-past-end gap [16,32) reads as ZERO, not heap");
        check(back[32] == 'D', "the byte written past EOF is where it was put");
        file_close(r);
    }

    /* ---- no-op truncate schedules no write-back --------------------------- */
    {
        stor_vfs_reset();
        struct file *f = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        char buf[20];
        memset(buf, 'E', sizeof buf);
        file_write(f, buf, 20);
        file_fsync(f);                       /* clean now */
        int before = stor_vfs_writecount();
        check(file_truncate(f, 20) == 0, "ftruncate to the CURRENT length succeeds");
        file_close(f);                       /* would flush if dirty */
        check(stor_vfs_writecount() == before,
              "a no-op ftruncate scheduled no write-back at close (no phantom -2)");
    }

    /* ---- refusals are -1, never a fake success ---------------------------- */
    {
        stor_vfs_reset();
        struct file *w = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        file_write(w, "x", 1);
        file_close(w);

        struct file *r = file_open_vfs(PATH, O_RDONLY);
        check(r != NULL, "reopen read-only for the refusal cases");
        check(file_truncate(r, 0) == -1, "ftruncate on a READ-ONLY fd refuses (-1)");
        file_close(r);

        struct file *w2 = file_open_vfs(PATH, O_RDWR);
        check(file_truncate(w2, -1) == -1, "negative length refuses (-1)");
        file_close(w2);

        struct file *p[2];
        check(file_pipe(p, &p[1]) == 0, "make a pipe for the type refusal");
        check(file_truncate(p[0], 8) == -1, "ftruncate on a PIPE refuses (-1)");
        file_close(p[0]); file_close(p[1]);

        check(file_truncate(NULL, 8) == -1, "NULL file refuses (-1)");
    }

    /* ---- the flush failure is reported, not swallowed --------------------- */
    {
        stor_vfs_reset();
        struct file *f = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        file_write(f, "ffffff", 6);
        file_truncate(f, 3);
        stor_vfs_set_refuse(1);              /* the disk just filled up */
        check(file_fsync(f) == -1, "fsync reports the refused flush (-1)");
        check(file_close(f) == -1, "last close reports the refused flush (-1)");
        stor_vfs_set_refuse(0);
    }

    /* ---- the full whole-store pattern end to end ---------------------------
     * open+O_TRUNC, rewrite SHORTER, fsync, close, reopen: this is exactly
     * what a localStorage/IDB/cookie store save does, measured working on the
     * machine by storprobe.as; asserted here so a regression in the fd layer
     * is caught host-side too. */
    {
        stor_vfs_reset();
        struct file *f = file_open_vfs(PATH, O_WRONLY | O_CREAT | O_TRUNC);
        char big[80];
        memset(big, 'G', sizeof big);
        file_write(f, big, 80);
        file_close(f);

        f = file_open_vfs(PATH, O_WRONLY | O_TRUNC);
        file_write(f, "small-store", 11);
        file_close(f);

        check(stor_vfs_size() == 11, "O_TRUNC rewrite left exactly 11 bytes");
        struct file *r = file_open_vfs(PATH, O_RDONLY);
        unsigned char back[128];
        long n = file_read(r, back, sizeof back);
        check(n == 11 && memcmp(back, "small-store", 11) == 0,
              "the rewritten store reads back byte-exact at the smaller length");
        file_close(r);
    }

    if (failures) {
        printf("storage_test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("storage_test: all checks passed\n");
    return 0;
}
