/* A full disk must still be able to free space.
 *
 * Every directory change used to be copy-on-write: the new copy had to be
 * ALLOCATED before the old blocks could be released at commit, so with zero
 * free blocks a delete -- the only way to make room -- failed too, and the
 * volume was wedged until it was reformatted. Zero free blocks is reachable
 * with ordinary operations: with exactly two blocks left, creating the first
 * file in an EMPTY directory takes both (its data block and the directory's
 * first block) and has no old directory block to hand back.
 *
 * The apparatus lands on exactly two free blocks by arithmetic, not by luck,
 * and asserts it did. -DLFS_DIR_COW_NEGCTL restores copy-on-write for the
 * delete path, and every "can free space" assertion must then FAIL. */
#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }

static uint8_t buf[2 << 20];

static int free_blocks(void)
{
    struct lfs_super sb;
    sim_read_super(&sb);
    uint8_t bm[LFS_BS];
    int f = 0;
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        bcache_read(sb.bitmap_start + i, bm);
        for (uint32_t b = 0; b < LFS_BS * 8; b++) {
            uint32_t blk = i * LFS_BS * 8 + b;
            if (blk >= sb.data_start && blk < sb.total_blocks && !(bm[b >> 3] & (1 << (b & 7)))) f++;
        }
    }
    return f;
}

int main(void)
{
    sim_open_n(300);
    fs_ok(logitfs.mount() == 0, "mount");
    memset(buf, 7, sizeof buf);
    fs_ok(logitfs.mkdir("/e") == 0, "make the empty directory");

    /* Consume free blocks until exactly two remain. A file of d data blocks
     * costs d, plus one pointer block past the 12 direct ones; the root
     * directory's own rewrite is returned at commit. */
    char p[64];
    int n = 0;
    for (int guard = 0; guard < 60; guard++) {
        int F = free_blocks();
        if (F <= 2) break;
        int c = F - 2;
        int d = c <= 12 ? c : c - 1;
        if (c == 13) d = 12;                      /* 13 is unreachable (12 or 14) */
        snprintf(p, sizeof p, "/fill%d", n);
        if (logitfs.write(p, buf, d * LFS_BS) < 0) break;
        n++;
    }
    fs_ok(free_blocks() == 2, "apparatus reaches exactly two free blocks (have %d)", free_blocks());
    fs_ok(logitfs.write("/e/x", buf, 1) == 1, "create the first file in the empty directory");
    fs_ok(free_blocks() == 0, "that takes the last two blocks: zero free (have %d)", free_blocks());

    int del_small = logitfs.del("/e/x");
    fs_ok(del_small >= 0, "delete a file at zero free blocks");
    int del_big = logitfs.del("/fill0");
    fs_ok(del_big >= 0, "delete a large file at zero free blocks");
    fs_ok(free_blocks() > 100, "space is actually returned (have %d free)", free_blocks());
    fs_ok(logitfs.write("/after", buf, 50000) == 50000, "the volume accepts writes again");

    logitfs_unmount();
    fs_ok(logitfs.mount() == 0, "remount");
    fs_ok(logitfs.size("/after") == 50000, "the new file survived a remount");
    fs_ok(logitfs_fsck(0) == 0, "filesystem is clean");
    logitfs_unmount();
    sim_close();
    return fs_verdict("fs_full_delete_test");
}
