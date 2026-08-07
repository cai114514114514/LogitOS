#ifndef LOGIT_LFSRO_H
#define LOGIT_LFSRO_H

#include "vfs.h"

/* A READ-ONLY LogitFS reader, one instance per block device.
 *
 * WHY A SECOND READER OF THE SAME FORMAT
 * --------------------------------------|
 * Because c/fs/logitfs.c is a singleton and cannot become two. Its superblock,
 * block bitmap and inode table are file-static, and every one of its VFS ops
 * takes a path and no `self` -- so there is no argument that could say which
 * mount is being addressed. Making it instance-aware means rewriting it, and
 * it is being rewritten right now by the crash-consistency line for an
 * unrelated reason. Two lines restructuring one file is how a filesystem gets
 * lost.
 *
 * So the mount table gets a second driver for the same on-disk format instead,
 * built on the shared c/fs/logitfs_fmt.h -- which exists for exactly this
 * situation: c/fs/fsck.c already reads an image that logitfs has NOT mounted,
 * for the same reason, and the format constants were lifted into a header so
 * that a second reader is a reader and not a copy.
 *
 * Read-only on purpose. Writing means the block bitmap, the inode allocator
 * and the write-ahead log, and re-implementing a journal beside the one being
 * built next door would be a genuinely bad idea. Mounting a second disk to
 * READ it is the claim the mount table needs to support, and it is the whole
 * claim this makes.
 *
 * `dev` is a block-device name as the block layer publishes it: "virtio1",
 * "nvme0p2", "ahci0". Returns NULL if there is no such device or it does not
 * hold a LogitFS superblock. */
struct filesystem *lfsro_create(const char *dev);
void               lfsro_destroy(struct filesystem *fs);

#endif /* LOGIT_LFSRO_H */
