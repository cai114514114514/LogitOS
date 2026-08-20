#include <stdint.h>
#include <stddef.h>
#include "pcache.h"
#include "pcache_vfs.h"
#include "vfs.h"
#include "vfs_meta.h"
#include "kheap.h"
#include "kprintf.h"

/* See pcache_vfs.h for why this file exists at all and pcache.h's "the seam"
 * for the contract it is implementing. This is the c/fs/ side of it: the two
 * callbacks a filesystem has to answer so pcache.c never has to know one
 * exists. */

void *memcpy(void *, const void *, size_t);   /* see pcache.c's identical line: no libc here */

/* --------------------------------------------------------------------- stat --
 *
 * (dev, ino) is the cache's whole notion of identity (pcache.h): two names
 * that resolve to the same file MUST land on the same entry, or two processes
 * mapping "the same" file stop sharing it, silently, which is half of what
 * this cache is for.
 *
 * vfs_statx's dev/ino are only trustworthy when VA_INO is set. Quoting the
 * comment above vfs_statx itself (c/fs/vfs.c): "two paths naming the same
 * file agree on (dev, ino) exactly when the backend reports real inode
 * numbers." LogitFS's getattr does (logitfs_getattr sets a->ino to the real
 * on-disk inode number and a->flags |= VA_INO). A backend that does NOT --
 * ramfs and lfsro both pass `getattr = NULL` today -- makes every one of its
 * files report ino 0, and caching two such files under the same fabricated
 * identity would mean the second one opened hands out pages that belong to
 * the first. So: no VA_INO, no caching, full stop. This is a correctness
 * decision, not a capacity one, which is why it lives here rather than as
 * one more way for pcache.c's file table to say "no slot". */
static int pcv_stat(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size)
{
    struct vattr a;
    if (vfs_statx(path, &a, 1) != 0) return -1;
    if (a.type != VT_REG) return -1;          /* a directory or symlink is not a page-cacheable file */
    if (!(a.flags & VA_INO)) return -1;       /* see block comment above */
    *dev = a.dev;
    *ino = a.ino;
    *size = a.size;
    return 0;
}

/* --------------------------------------------------------------------- read --
 *
 * pcache.c asks for `len` bytes at `off`, always page-aligned and never more
 * than 4096 (pcache.h's contract on struct pcache_ops). c/fs/vfs.h has no
 * offset read: vfs_read(path, buf, max) always starts at byte 0 of the file,
 * because that is all F_VFS's whole-file slurp (c/kernel/exec/file.c) has ever
 * needed, and every backend under struct filesystem/struct fs_iops takes the
 * same (path, buf, max) with no offset either. Adding a real pread means
 * changing c/fs/vfs.c and every backend beneath it -- out of reach here (this
 * unit owns this file, not c/fs/, which the storage line owns).
 *
 * TWO THINGS THIS FUNCTION USED TO GET WRONG, and the first one is why the
 * page cache had never served a single page of a real file:
 *
 * 1. vfs_read IS ALL OR NOTHING. logitfs.c:569 -- `if (max < 0 || size >
 *    (uint32_t)max || !size_ok(size)) return -1` -- so asking for the first
 *    12 KiB of a 36 KiB file does not return 12 KiB, it returns **-1**. The
 *    old code read (off + len) bytes into a scratch buffer and kept the tail,
 *    which fails for every file larger than off + 4096, i.e. for every page of
 *    every multi-page file; and its `off == 0` fast path asked for 4096 bytes
 *    of the whole file, which fails for every file bigger than one page. So
 *    pcache_get() returned 0 for essentially everything. Nothing caught it
 *    because nothing called it: SYS_MMAP_FILE's only caller in the tree is a
 *    script written to exercise it, one page long. Measured, not reasoned:
 *    /bin/login, 36112 bytes, page index 2 -> the loader's file VMA faulted,
 *    do_file declined, and the process died at its own entry point.
 *
 * 2. IT WAS O(off) PER PAGE. Even had the slice worked, a file faulted in
 *    start to end would re-read the whole prefix once per page -- O(n^2) bytes
 *    for an n-page file, with a kmalloc the size of the prefix each time. That
 *    is not a tuning detail once a program's TEXT is faulted in this way: the
 *    browser's 870 file-backed pages would have cost about 1.5 GiB of copying
 *    to start.
 *
 * Both are fixed by the same thing, which is the only shape available while
 * the only primitive is "read the whole file": ONE whole-file window, kept
 * across consecutive reads of the same path. A sequential walk then costs one
 * device read and one kmalloc for the file, not one per page. The window is
 * dropped as soon as a different file is read, so at most one is live -- the
 * same allocation exec.c already makes for the image it is loading, and for
 * the same duration.
 *
 * WHAT IS NOT DONE, and why: no size cap. A cap would mean pcv_read returning
 * -1 for a big file, which at FAULT time is not a fallback -- pcache_get()
 * returns 0, do_file() declines and the process dies. A load can fall back to
 * copying; a fault cannot. So the failure modes here are exactly kmalloc's,
 * which is where they already were. */
/* PCACHE_PATHMAX and not a number of its own. The cache will hold an entry for
 * any path that fits ITS buffer, and a window that could not name such a path
 * would return -1 for it -- which at fault time is not a fallback, it is a dead
 * process. The two limits have to be the same limit, so it is the same
 * #define. (96 here and 192 there was the first draft, and the gap was
 * reachable: every path 96..191 characters long.) */
#define PCV_PATHMAX PCACHE_PATHMAX

static char     pcv_wpath[PCV_PATHMAX];
static uint8_t *pcv_wbuf;
static uint64_t pcv_wsize;

static int pcv_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void pcv_window_drop(void)
{
    if (pcv_wbuf) kfree(pcv_wbuf);
    pcv_wbuf = 0;
    pcv_wsize = 0;
    pcv_wpath[0] = 0;
}

/* Load `path` whole. Returns 0, or -1 leaving no window behind. */
static int pcv_window_fill(const char *path)
{
    pcv_window_drop();

    uint64_t n = 0;
    {
        size_t i = 0;
        while (path[i] && i < PCV_PATHMAX - 1) i++;
        if (path[i]) return -1;                  /* longer than the window can name;
                                                  * refuse rather than serve a prefix
                                                  * of some other file with the same
                                                  * first 95 characters */
    }
    int sz = vfs_size(path);
    if (sz <= 0 || sz > 0x7FFFFFFF) return -1;
    n = (uint64_t)sz;

    uint8_t *buf = kmalloc((size_t)n);
    if (!buf) return -1;
    int got = vfs_read(path, buf, (int)n);
    if (got < 0 || (uint64_t)got != n) { kfree(buf); return -1; }

    size_t i = 0;
    for (; path[i]; i++) pcv_wpath[i] = path[i];
    pcv_wpath[i] = 0;
    pcv_wbuf = buf;
    pcv_wsize = n;
    return 0;
}

static long pcv_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    if (!path || !dst || len == 0) return -1;

    if (!pcv_wbuf || !pcv_streq(pcv_wpath, path))
        if (pcv_window_fill(path) < 0) return -1;

    if (off >= pcv_wsize) return 0;              /* past EOF: not a page of this file */
    uint64_t n = pcv_wsize - off;
    if (n > len) n = len;
    memcpy(dst, pcv_wbuf + off, (size_t)n);
    return (long)n;
}

/* The file's bytes changed. pcache.c calls this from every invalidation path,
 * because the window is a second copy of the file and a second copy that
 * nobody invalidates is exactly the bug pcache_invalidate_path exists to
 * prevent, one layer down. By PATH and not by identity for the same reason
 * that function gives: the caller may have just made the name mean a different
 * inode. Conservative on purpose -- the window is dropped whenever any file is
 * invalidated whose name we cannot prove is not ours. */
static void pcv_forget(const char *path)
{
    if (!pcv_wbuf) return;
    if (!path || pcv_streq(pcv_wpath, path)) pcv_window_drop();
}

static const struct pcache_ops pcv_ops = {
    .stat = pcv_stat,
    .read = pcv_read,
    .forget = pcv_forget,
};

void pcache_vfs_install(void)
{
    pcache_set_ops(&pcv_ops);
    kprintf("[pcache] backend installed over c/fs/vfs.h (stat+read, VFS-identity gated)\n");
}
