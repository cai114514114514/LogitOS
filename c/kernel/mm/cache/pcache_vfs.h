#ifndef LOGIT_PCACHE_VFS_H
#define LOGIT_PCACHE_VFS_H

/* The seam pcache.h asks for (struct pcache_ops), built over c/fs/vfs.h.
 *
 * c/kernel/mm/ may not depend on c/fs/ -- pcache.c is compiled host-side
 * (tests/unit/mm_run.sh) with mmhost.h as its only seam, and there is no VFS
 * there. This one file is the boundary: it is kernel-only (real vfs_* calls),
 * it is the only c/kernel/mm/ file that includes "vfs.h", and it is the only
 * one that may not be added to MMSRC in tests/unit/mm_run.sh.
 *
 * Call once, from kmain.c, after vfs_mount() -- see the call site for why
 * after and not before. Installs the ops and says so on the console; a seam
 * nobody can see connect is one nobody can tell is connected. */
void pcache_vfs_install(void);

#endif /* LOGIT_PCACHE_VFS_H */
