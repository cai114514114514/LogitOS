#include <stdint.h>
#include <stddef.h>
#include "pcache.h"
#include "pcache_vfs.h"
#include "vfs.h"
#include "vfs_meta.h"
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
 * than 4096 (pcache.h's contract on struct pcache_ops). vfs_pread is exactly
 * that shape, so this function is now one call.
 *
 * IT USED TO BE NINETY LINES OF WORKAROUND, and the workaround is DELETED
 * rather than kept beside the real thing, because two paths that answer the
 * same question are two paths that can disagree. What it worked around is
 * recorded here, because the reason is worth keeping and the code is not:
 *
 *   c/fs/vfs.h had no offset read. vfs_read(path, buf, max) always started at
 *   byte 0, and on logitfs it was ALL OR NOTHING -- asking for the first 12 KiB
 *   of a 36 KiB file returned -1, not 12 KiB. So this file kept a whole-file
 *   WINDOW: one kmalloc of the entire file, held across consecutive reads of
 *   the same path, dropped as soon as a different path was read, and kept from
 *   going stale by a third callback (->forget) that existed only for it.
 *
 *   Two costs went with it, and the first is the one that matters now. Filling
 *   ONE page allocated the WHOLE file, so a file bigger than the kernel heap
 *   could not be faulted at all -- the exact ceiling the file-backed VMA work
 *   exists to remove, reintroduced one layer underneath it. And the window was
 *   a second copy of file data with its own invalidation rules, sitting under a
 *   cache whose whole job is to be the one copy.
 *
 * vfs_pread returns 0 at or past end of file, which is precisely what
 * pcache.c's contract wants for a page beyond the file. A backend that cannot
 * read part of a file returns VFS_ENOSYS (negative) and that is reported as -1
 * -- a refusal, not a page of zeroes. */
static long pcv_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    if (!path || !dst || len == 0) return -1;
    if (len > 0x7FFFFFFFull || off > 0x7FFFFFFFFFFFFFFFull) return -1;
    int n = vfs_pread(path, dst, (int)len, (long long)off);
    return n < 0 ? -1 : (long)n;
}

/* No ->forget any more. The window it invalidated is gone, and the cache below
 * is now the only copy of a file's bytes on this path -- pcache_invalidate_path
 * already throws that copy away. A hook that has nothing to drop is a line
 * every future reader has to check is still doing nothing. pcache.c calls it
 * only under `if (pc_ops->forget)`, so NULL is the supported spelling of "this
 * backend caches nothing of its own". */

static const struct pcache_ops pcv_ops = {
    .stat = pcv_stat,
    .read = pcv_read,
};

void pcache_vfs_install(void)
{
    pcache_set_ops(&pcv_ops);
    kprintf("[pcache] backend installed over c/fs/vfs.h (statx + vfs_pread, VFS-identity gated)\n");
}
