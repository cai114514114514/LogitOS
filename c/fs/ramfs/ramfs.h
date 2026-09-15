#ifndef LOGIT_RAMFS_H
#define LOGIT_RAMFS_H

#include "vfs.h"

/* An in-memory filesystem, one instance per mount.
 *
 * It exists for two reasons that are not "because every kernel has one":
 *   - it is the second filesystem the mount table needs in order to be more
 *     than a table with one row, and it needs no device, so the mount-table
 *     unit tests run on the host;
 *   - it is writable by construction, which makes it the honest place to put
 *     /tmp on a system whose on-disk filesystem is being rewritten underneath.
 *
 * Instance-aware (struct fs_iops), so `mount ramfs a /tmp` and `mount ramfs b
 * /var` are two different filesystems and not two names for one. */
struct filesystem *ramfs_create(const char *label);
void               ramfs_destroy(struct filesystem *fs);

#endif /* LOGIT_RAMFS_H */
