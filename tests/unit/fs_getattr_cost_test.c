/* LogitFS getattr is part of every vfs_pread permission check.  A large file's
 * st_blocks answer must therefore not walk that file's block mapping.  Drive
 * the real filesystem and buffer cache over the simulated disk, and count the
 * cache lookups rather than host time: the count is deterministic under load. */

#include "fs_sim.h"
#include "fs_check.h"
#include "logitfs.h"
#include "bcache.h"

#define NBLOCKS 4096
#define FILE_BLOCKS 900
#define FILE_SIZE (FILE_BLOCKS * LFS_BS)

static uint8_t image[FILE_SIZE];

int main(void)
{
    sim_open_n(NBLOCKS);
    fs_ok(logitfs.mount() == 0, "mount");
    for (int i = 0; i < FILE_SIZE; i++) image[i] = (uint8_t)(i * 17 + (i >> 12));
    fs_ok(logitfs.write("/big", image, FILE_SIZE) == FILE_SIZE,
          "write a %d-block dense file", FILE_BLOCKS);
    fs_ok(bcache_sync() == 0, "sync");

    bcache_drop();
    struct bcache_stats before, after;
    bcache_getstats(&before);
    struct vattr a;
    fs_ok(logitfs.getattr && logitfs.getattr("/big", &a) == 0, "getattr succeeds");
    bcache_getstats(&after);

    unsigned long lookups = (after.hits - before.hits) + (after.misses - before.misses);
    fs_ok(a.size == FILE_SIZE, "size is %d bytes", FILE_SIZE);
    fs_ok(a.blocks == (uint64_t)FILE_BLOCKS * (LFS_BS / 512),
          "st_blocks reports all %d dense data blocks in 512-byte units", FILE_BLOCKS);
    fs_ok(lookups <= 4,
          "GETATTR O(1): %lu cache lookup(s) for a %d-block file, must be <= 4",
          lookups, FILE_BLOCKS);
    printf("  getattr %d-block file: %lu cache lookup(s)\n", FILE_BLOCKS, lookups);

    logitfs_unmount();
    sim_close();
    return fs_verdict("fs_getattr_cost_test");
}
