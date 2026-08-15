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
 * because that is all F_VFS's whole-file slurp (c/kernel/exec/file.c) has
 * ever needed, and every backend under struct filesystem/struct fs_iops takes
 * the same (path, buf, max) with no offset either. Adding a real pread means
 * changing c/fs/vfs.c and every backend beneath it -- out of reach here (this
 * unit owns kmain.c and this file, not c/fs/, which the storage line owns) --
 * so this builds the bounded read the only way available: read the file's
 * first (off + len) bytes into a scratch buffer and keep the tail. It is the
 * same "read the whole thing into a kmalloc buffer" idiom file.c's F_VFS
 * backend already uses, extended by one slice operation.
 *
 * THE COST, NAMED RATHER THAN LEFT FOR SOMEONE TO DISCOVER: this is O(off),
 * not O(len). A file mapped and faulted in start-to-end therefore costs
 * O(n^2) total bytes copied across the whole walk, not O(n), and each miss
 * kmallocs a scratch buffer the size of the offset it is reading up to --
 * kheap.h: kfree returns it to kheap's free lists but the arena that backs
 * them never shrinks, so a single high-offset miss on a large file raises the
 * kernel heap's high-water mark for the rest of the boot. The real fix is an
 * offset primitive in the VFS; this is the correct answer available without
 * touching a file this unit does not own. The common case is unaffected: off
 * == 0 -- the first page of every file, and the entirety of a one-page file
 * -- skips the scratch buffer and reads straight into the caller's frame. */
static long pcv_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    if (!path || !dst || len == 0) return -1;

    if (off == 0) {
        if (len > 0x7FFFFFFFull) return -1;      /* vfs_read's `max` is a plain int */
        int n = vfs_read(path, dst, (int)len);
        return n < 0 ? -1 : (long)n;
    }

    uint64_t need = off + len;
    if (need < off || need > 0x7FFFFFFFull) return -1;   /* overflow, or past what vfs_read can take */

    void *scratch = kmalloc((size_t)need);
    if (!scratch) return -1;

    int got = vfs_read(path, scratch, (int)need);
    if (got < 0 || (uint64_t)got <= off) {
        kfree(scratch);
        return got < 0 ? -1 : 0;                  /* short file: nothing at `off` (already past EOF) */
    }

    uint64_t avail = (uint64_t)got - off;
    uint64_t n = avail < len ? avail : len;
    memcpy(dst, (const uint8_t *)scratch + off, (size_t)n);
    kfree(scratch);
    return (long)n;
}

static const struct pcache_ops pcv_ops = {
    .stat = pcv_stat,
    .read = pcv_read,
};

void pcache_vfs_install(void)
{
    pcache_set_ops(&pcv_ops);
    kprintf("[pcache] backend installed over c/fs/vfs.h (stat+read, VFS-identity gated)\n");
}
