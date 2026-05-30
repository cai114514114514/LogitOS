#include <stdint.h>
#include <stddef.h>
#include "aquafs.h"
#include "ata.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);   /* lib/string.c */

#define SECTOR       512
#define AQUAFS_MAGIC 0x41515541u      /* "AQUA" */
#define DIR_LBA      1
#define DIR_SECTORS  2
#define ENTRY_SIZE   64
#define MAX_FILES    16
#define DATA_LBA     3

struct ondisk_entry {
    char     name[48];
    uint32_t start_lba;
    uint32_t size;
    uint8_t  pad[8];
} __attribute__((packed));

static struct {
    char     name[48];
    uint32_t start_lba;
    uint32_t size;
} entries[MAX_FILES];
static uint32_t entry_count;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static struct ondisk_entry *dir_buf(uint8_t *buf)
{
    return (struct ondisk_entry *)buf;
}

static int aquafs_mount(void)
{
    uint8_t sb[SECTOR];
    if (ata_read(0, 1, sb))
        return -1;

    uint32_t magic = *(uint32_t *)(sb + 0);
    if (magic != AQUAFS_MAGIC)
        return -1;

    entry_count = *(uint32_t *)(sb + 8);
    if (entry_count > MAX_FILES)
        entry_count = MAX_FILES;

    uint8_t dir[SECTOR * DIR_SECTORS];
    if (ata_read(DIR_LBA, DIR_SECTORS, dir))
        return -1;

    struct ondisk_entry *e = dir_buf(dir);
    for (uint32_t i = 0; i < entry_count; i++) {
        memcpy(entries[i].name, e[i].name, 48);
        entries[i].name[47] = '\0';
        entries[i].start_lba = e[i].start_lba;
        entries[i].size      = e[i].size;
    }
    return 0;
}

static void aquafs_list(void)
{
    kprintf("[fs] %u file(s):\n", entry_count);
    for (uint32_t i = 0; i < entry_count; i++)
        kprintf("[fs]   %-16s %u bytes @ lba %u\n",
                entries[i].name, entries[i].size, entries[i].start_lba);
}

static int find(const char *name)
{
    for (uint32_t i = 0; i < entry_count; i++)
        if (streq(entries[i].name, name))
            return (int)i;
    return -1;
}

static int aquafs_size(const char *name)
{
    int i = find(name);
    return i < 0 ? -1 : (int)entries[i].size;
}

/* Reads whole sectors into buf (which must hold ceil(size/512)*512 bytes);
 * returns the file's logical byte size. */
static int aquafs_read(const char *name, void *buf, int max)
{
    int i = find(name);
    if (i < 0)
        return -1;

    uint32_t size = entries[i].size;
    uint32_t sectors = (size + SECTOR - 1) / SECTOR;
    if (sectors == 0)
        sectors = 1;
    if ((int)(sectors * SECTOR) > max)
        return -1;

    if (ata_read(entries[i].start_lba, (uint8_t)sectors, buf))
        return -1;
    return (int)size;
}

struct filesystem aquafs = {
    .name  = "aquafs",
    .mount = aquafs_mount,
    .list  = aquafs_list,
    .size  = aquafs_size,
    .read  = aquafs_read,
};
