/* SPDX-License-Identifier: MIT
 * Validate a private copy with the same journal recovery and fsck as mount.
 * The input is never writable. The output is a mode-0600 temporary image used
 * by the packer; diagnostics contain counts, never file data or names. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fsck.h"

struct image {
    unsigned char *data;
    size_t size;
};

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

static int read_block(void *context, uint32_t block, void *output)
{
    struct image *image = context;
    size_t offset = (size_t)block * LFS_BS;
    if (offset + LFS_BS > image->size)
        return -1;
    memcpy(output, image->data + offset, LFS_BS);
    return 0;
}

static int write_block(void *context, uint32_t block, const void *input)
{
    struct image *image = context;
    size_t offset = (size_t)block * LFS_BS;
    if (offset + LFS_BS > image->size)
        return -1;
    memcpy(image->data + offset, input, LFS_BS);
    return 0;
}

static int sync_copy(void *context)
{
    (void)context;
    return 0;
}

static int write_range(int fd, const struct image *image, size_t begin, size_t end)
{
    while (begin < end) {
        size_t length = end - begin;
        if (length > 1024 * 1024)
            length = 1024 * 1024;
        ssize_t written = pwrite(fd, image->data + begin, length, (off_t)begin);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        begin += (size_t)written;
    }
    return 0;
}

static int write_snapshot(int fd, const struct image *image)
{
    if (ftruncate(fd, (off_t)image->size) < 0)
        return -1;

#ifdef LFS_SNAPSHOT_DENSE
    /* Test-only control: old behavior writes every zero block as real data. */
    return write_range(fd, image, 0, image->size);
#else
    static const unsigned char zero[LFS_BS];
    size_t block = 0;

    /* Read the recovered MAP_PRIVATE view, not the source file's sparse extents.
     * Journal replay can change a page that was zero in the source. Holes in
     * this new file read as zeros, so the raw bytes remain identical while a
     * mostly empty 512 MiB disk no longer costs 512 MiB of snapshot writes. */
    while (block < image->size) {
        if (memcmp(image->data + block, zero, LFS_BS) == 0) {
            block += LFS_BS;
            continue;
        }
        size_t begin = block;
        do {
            block += LFS_BS;
        } while (block < image->size &&
                 memcmp(image->data + block, zero, LFS_BS) != 0);
        if (write_range(fd, image, begin, block) < 0)
            return -1;
    }
    return 0;
#endif
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fputs("usage: lfs_snapshot <input> <new-output>\n", stderr);
        return 2;
    }

    int source = open(argv[1], O_RDONLY);
    int output = -1;
    int result = 1;
    struct stat stat;
    struct image image = {0};

    if (source < 0 || fstat(source, &stat) < 0 || !S_ISREG(stat.st_mode) ||
        stat.st_size < LFS_BS || stat.st_size % LFS_BS != 0)
        goto done;

    image.size = (size_t)stat.st_size;
    image.data = mmap(NULL, image.size, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                      source, 0);
    if (image.data == MAP_FAILED) {
        image.data = NULL;
        goto done;
    }

    struct lfs_super super;
    memcpy(&super, image.data, sizeof super);
    if (fsck_super_valid(&super) < 0 ||
        (uint64_t)super.total_blocks * LFS_BS != image.size)
        goto done;

    struct fsck_dev device = {
        read_block, write_block, sync_copy, &image, super.total_blocks
    };
    int discarded = 0;
    int replayed = fsck_log_recover(&device, &super, &discarded);
    if (replayed < 0)
        goto done;

    device.write = NULL;
    struct fsck_report report;
    if (fsck_run(&device, 0, &report, NULL, NULL) < 0) {
        fprintf(stderr, "profile snapshot: filesystem refused (%d problems)\n",
                report.problems);
        goto done;
    }

    output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output < 0 || write_snapshot(output, &image) < 0 || fsync(output) < 0)
        goto done;

    printf("profile snapshot: checked; journal replayed=%d discarded=%d\n",
           replayed, discarded);
    result = 0;
done:
    if (output >= 0) {
        if (close(output) < 0)
            result = 1;
        if (result)
            unlink(argv[2]);
    }
    if (image.data)
        munmap(image.data, image.size);
    if (source >= 0)
        close(source);
    if (result)
        fputs("profile snapshot: refused; original image untouched\n", stderr);
    return result;
}
