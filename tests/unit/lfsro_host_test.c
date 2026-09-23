/* The read-only second-disk reader must report signed sizes honestly and
 * read a directory block once per scan, not once per entry. This links its
 * real implementation; only device, lock, console and geometry are supplied. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lfsro.h"
#include "logitfs_fmt.h"
#include "blkdev.h"

int fsstub_verbose;

#define BLOCKS 64u
static uint8_t disk[BLOCKS * LFS_BS];
static struct blkdev dev = { BLOCKS * LFS_SPB };
static int checks, failures;
static int reads;

static void check(int yes, const char *what)
{
    checks++;
    if (!yes) { failures++; printf("FAIL: %s\n", what); }
}

struct blkdev *blk_find(const char *name)
{
    return strcmp(name, "testdisk") == 0 ? &dev : NULL;
}

int blk_dev_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    if (d != &dev || lba > dev.nsectors || count > dev.nsectors - lba) return -1;
    reads++;
    memcpy(buf, disk + (size_t)lba * LFS_SECTOR, (size_t)count * LFS_SECTOR);
    return 0;
}

/* Geometry is not under test; the fixture spells valid region boundaries. */
int fsck_super_valid(const struct lfs_super *sb)
{
    return sb->magic == LFS_MAGIC && sb->total_blocks == BLOCKS &&
           sb->inode_start == 2 && sb->data_start == 6 ? 0 : -1;
}

static struct lfs_dinode *inode(uint32_t i)
{
    return (struct lfs_dinode *)(void *)(disk + 2 * LFS_BS + i * LFS_INODE_SIZE);
}

static void fixture(void)
{
    memset(disk, 0, sizeof disk);
    struct lfs_super sb = {
        .magic = LFS_MAGIC, .version = LFS_VERSION, .block_size = LFS_BS,
        .total_blocks = BLOCKS, .inode_count = 64,
        .bitmap_start = 1, .bitmap_blocks = 1,
        .inode_start = 2, .inode_blocks = 2,
        .log_start = 4, .log_blocks = 2, .data_start = 6, .root_ino = 0,
    };
    memcpy(disk, &sb, sizeof sb);
    inode(0)->type = LFS_T_DIR;
    inode(0)->size = LFS_DIRENT_SZ;
    inode(0)->direct[0] = 6;
    inode(1)->type = LFS_T_FILE;
    struct lfs_dirent *entry = (struct lfs_dirent *)(void *)(disk + 6 * LFS_BS);
    entry->ino = 1;
    memcpy(entry->name, "huge", 5);
}

static void scan_fixture(void)
{
    fixture();
    inode(0)->size = 32 * LFS_DIRENT_SZ;
    struct lfs_dirent *entries = (struct lfs_dirent *)(void *)(disk + 6 * LFS_BS);
    for (int i = 0; i < 32; i++) {
        entries[i].ino = (uint32_t)i + 1;
        snprintf(entries[i].name, sizeof entries[i].name, "f%02d", i);
        inode((uint32_t)i + 1)->type = LFS_T_FILE;
    }
}

int main(void)
{
    fixture();
    struct filesystem *fs = lfsro_create("testdisk");
    check(fs != NULL, "read-only instance created");
    if (!fs) return 1;
    check(fs->iops->mount(fs) == 0, "valid fixture mounted");
    check(fs->iops->count(fs, "/") == 1, "file appears in root directory");

    inode(1)->size = (uint32_t)INT32_MAX;
    check(fs->iops->size(fs, "/huge") == INT32_MAX,
          "largest representable file size is reported exactly");
    check(fs->iops->ent_size(fs, "/", 0) == INT32_MAX,
          "directory entry reports largest representable size");

    inode(1)->size = (uint32_t)INT32_MAX + 1u;
    check(fs->iops->size(fs, "/huge") == -1,
          "out-of-range file size is refused instead of wrapping negative");
    check(fs->iops->ent_size(fs, "/", 0) == 0,
          "out-of-range directory entry size follows LogitFS zero convention");

    inode(1)->size = UINT32_MAX;
    check(fs->iops->size(fs, "/huge") == -1,
          "maximum disk size is refused instead of looking like another error");
    check(fs->iops->ent_size(fs, "/", 0) == 0,
          "maximum disk size never becomes a negative entry size");

    scan_fixture();
    reads = 0;
    check(fs->iops->count(fs, "/") == 32, "all 32 directory entries are counted");
    int count_reads = reads;
    check(count_reads == 2, "count reads the inode and shared directory block once each");
    reads = 0;
    check(strcmp(fs->iops->ent_name(fs, "/", 31), "f31") == 0,
          "the last entry is found correctly");
    int nth_reads = reads;
    check(nth_reads == 2, "nth entry reads the inode and directory block once each");
    printf("directory reads: count %d, last entry %d\n", count_reads, nth_reads);

    lfsro_destroy(fs);
    printf("lfsro host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
