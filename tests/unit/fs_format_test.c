/* The on-disk format has two definitions -- c/fs/logitfs_fmt.h and the Python
 * mirror in tools/mkfs.py -- and nothing stops them drifting apart except this.
 *
 * So: take a REAL image, the one `make build/disk.img` produces and the one the
 * kernel actually boots, and read it with the C definitions. If a field moved on
 * one side and not the other, every assertion below is aimed at noticing.
 * A drift here is not a subtle bug: it is a kernel that mounts an image whose
 * inodes it is reading at the wrong offsets.
 *
 * usage: fs_format_test <image>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logitfs_fmt.h"
#include "fsck.h"
#include "fs_check.h"

int fsstub_verbose = 0;

static FILE *g_img;
static long  g_blocks;

static int f_read(void *cx, uint32_t blk, void *buf)
{
    (void)cx;
    if ((long)blk >= g_blocks) return -1;
    if (fseek(g_img, (long)blk * LFS_BS, SEEK_SET)) return -1;
    return fread(buf, 1, LFS_BS, g_img) == LFS_BS ? 0 : -1;
}

static void say(void *cx, const char *msg, uint32_t a, uint32_t b)
{
    (void)cx;
    printf("    fsck: ");
    printf(msg, a, b);
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <image>\n", argv[0]); return 2; }
    g_img = fopen(argv[1], "rb");
    if (!g_img) { fprintf(stderr, "%s: cannot open %s\n", argv[0], argv[1]); return 2; }
    fseek(g_img, 0, SEEK_END);
    g_blocks = ftell(g_img) / LFS_BS;

    /* --- the struct sizes the format depends on ---------------------------- */
    fs_ok(sizeof(struct lfs_dinode) == LFS_INODE_SIZE,
          "struct lfs_dinode is %d bytes, the format says %d",
          (int)sizeof(struct lfs_dinode), LFS_INODE_SIZE);
    fs_ok(sizeof(struct lfs_dirent) == LFS_DIRENT_SZ,
          "struct lfs_dirent is %d bytes, the format says %d",
          (int)sizeof(struct lfs_dirent), LFS_DIRENT_SZ);
    fs_ok(sizeof(struct lfs_super) == 13 * 4, "the superblock is 13 u32");
    /* The timestamp offsets tools/mkfs.py packs to (OFF_ATIME = 64). */
    fs_ok(__builtin_offsetof(struct lfs_dinode, direct) == 8, "direct[] at byte 8");
    fs_ok(__builtin_offsetof(struct lfs_dinode, indirect) == 56, "indirect at byte 56");
    fs_ok(__builtin_offsetof(struct lfs_dinode, double_indirect) == 60,
          "double_indirect at byte 60");
    fs_ok(__builtin_offsetof(struct lfs_dinode, atime) == 64, "atime at byte 64");
    fs_ok(__builtin_offsetof(struct lfs_dinode, mtime) == 72, "mtime at byte 72");
    fs_ok(__builtin_offsetof(struct lfs_dinode, ctime) == 80, "ctime at byte 80");

    /* --- the image itself --------------------------------------------------- */
    uint8_t sblk[LFS_BS];
    struct lfs_super sb;
    if (f_read(NULL, 0, sblk)) { fprintf(stderr, "cannot read block 0\n"); return 2; }
    memcpy(&sb, sblk, sizeof sb);

    fs_ok(sb.magic == LFS_MAGIC, "magic is LOGI (got %08x)", sb.magic);
    fs_ok(sb.version == LFS_VERSION, "version is %d (got %u)", LFS_VERSION, sb.version);
    fs_ok(sb.block_size == LFS_BS, "block size is %d (got %u)", LFS_BS, sb.block_size);
    fs_ok(fsck_super_valid(&sb) == 0, "the geometry mkfs.py wrote passes the driver's own bounds");
    fs_ok((long)sb.total_blocks <= g_blocks,
          "the superblock claims %u blocks, the file holds %ld", sb.total_blocks, (long)g_blocks);

    /* An empty log: a freshly built image has no transaction outstanding. */
    uint8_t logh[LFS_BS];
    fs_ok(f_read(NULL, sb.log_start, logh) == 0, "the log header block is readable");
    fs_ok(((uint32_t *)(void *)logh)[LFS_LOGH_MAGIC] != LFS_LOG_MAGIC,
          "a freshly built image carries no commit record");

    /* --- inodes read at the right offsets ----------------------------------- */
    uint8_t iblk[LFS_BS];
    fs_ok(f_read(NULL, sb.inode_start, iblk) == 0, "the first inode block is readable");
    struct lfs_dinode *root = (struct lfs_dinode *)(void *)iblk + sb.root_ino;
    fs_ok(root->type == LFS_T_DIR, "the root inode reads back as a directory (type %u)", root->type);
    fs_ok(root->size % LFS_DIRENT_SZ == 0,
          "the root's size (%u) is a whole number of dirents", root->size);
    fs_ok(root->size > 0, "the root directory is not empty");
    fs_ok(root->direct[0] >= sb.data_start,
          "the root's first data block (%u) is in the data area", root->direct[0]);

    /* The timestamps mkfs.py stamps: not zero, and not absurd. If the Python
     * pack offset and the C struct disagree, this is where it shows. */
    fs_ok(root->mtime > 1000000000LL,
          "mkfs stamped the root's mtime (got %lld) -- a 0 here means the Python "
          "offsets and struct lfs_dinode have drifted apart", (long long)root->mtime);
    fs_ok(root->atime == root->mtime && root->ctime == root->mtime,
          "mkfs stamps all three times identically");

    int stamped = 0, live = 0;
    for (uint32_t i = 0; i < sb.inode_count && i < LFS_IPB; i++) {
        struct lfs_dinode *in = (struct lfs_dinode *)(void *)iblk + i;
        if (in->type == LFS_T_FREE) continue;
        live++;
        if (in->mtime > 1000000000LL) stamped++;
    }
    fs_ok(live > 0 && stamped == live, "every live inode in the first block is stamped (%d/%d)",
          stamped, live);

    /* --- and the whole thing is consistent ---------------------------------- */
    struct fsck_dev d = { f_read, NULL, NULL, NULL, (uint32_t)g_blocks };
    struct fsck_report r;
    int rc = fsck_run(&d, 0, &r, say, NULL);
    fs_ok(rc == 0 && r.problems == 0,
          "the image tools/mkfs.py just built is fsck-clean (%d problem(s))", r.problems);

    fclose(g_img);
    return fs_verdict("fs_format_test");
}
