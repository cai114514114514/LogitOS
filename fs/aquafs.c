#include <stdint.h>
#include <stddef.h>
#include "aquafs.h"
#include "ata.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);   /* lib/string.c */
void *memset(void *, int, size_t);

#define SECTOR        512
#define AQUAFS_MAGIC  0x41515541u      /* "AQUA" */
#define AQUAFS_VER    2
#define DIR_LBA       1
#define DIR_SECTORS   2
#define ENTRY_SIZE    64
#define MAX_FILES     16
#define DATA_LBA      3
#define SLOT_SECTORS  128              /* fixed 64 KiB capacity per file */

struct ondisk_entry {
    char     name[48];
    uint32_t start_lba;
    uint32_t size;
    uint8_t  pad[8];
} __attribute__((packed));

/* All 16 slots are kept in memory; a slot is free when name[0] == 0. */
static struct {
    char     name[48];
    uint32_t start_lba;
    uint32_t size;
} slot[MAX_FILES];

static uint8_t bounce[SLOT_SECTORS * SECTOR];   /* sector-aligned write staging */

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int slot_used(int i) { return slot[i].name[0] != 0; }

static int aquafs_mount(void)
{
    uint8_t sb[SECTOR];
    if (ata_read(0, 1, sb))
        return -1;
    if (*(uint32_t *)(sb + 0) != AQUAFS_MAGIC || *(uint32_t *)(sb + 4) != AQUAFS_VER)
        return -1;

    uint8_t dir[SECTOR * DIR_SECTORS];
    if (ata_read(DIR_LBA, DIR_SECTORS, dir))
        return -1;

    struct ondisk_entry *e = (struct ondisk_entry *)dir;
    for (int i = 0; i < MAX_FILES; i++) {
        memcpy(slot[i].name, e[i].name, 48);
        slot[i].name[47] = '\0';
        slot[i].start_lba = DATA_LBA + i * SLOT_SECTORS;   /* fixed per slot */
        slot[i].size      = e[i].size;
    }
    return 0;
}

/* Persist the directory + superblock file count back to disk. */
static int flush_dir(void)
{
    struct ondisk_entry *e = (struct ondisk_entry *)bounce;   /* reuse staging */
    memset(bounce, 0, SECTOR * DIR_SECTORS);
    int used = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        memcpy(e[i].name, slot[i].name, 48);
        e[i].start_lba = slot[i].start_lba;
        e[i].size = slot[i].size;
        if (slot_used(i)) used++;
    }
    if (ata_write(DIR_LBA, DIR_SECTORS, bounce))
        return -1;

    uint8_t sb[SECTOR];
    if (ata_read(0, 1, sb))
        return -1;
    *(uint32_t *)(sb + 8) = (uint32_t)used;
    return ata_write(0, 1, sb);
}

static int find(const char *name)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (slot_used(i) && streq(slot[i].name, name))
            return i;
    return -1;
}

/* --- read-only ops --- */
static int aquafs_size(const char *name)
{
    int i = find(name);
    return i < 0 ? -1 : (int)slot[i].size;
}

static int aquafs_read(const char *name, void *buf, int max)
{
    int i = find(name);
    if (i < 0)
        return -1;
    uint32_t size = slot[i].size;
    uint32_t sectors = (size + SECTOR - 1) / SECTOR;
    if (sectors == 0)
        sectors = 1;
    if ((int)(sectors * SECTOR) > max)
        return -1;
    if (ata_read(slot[i].start_lba, (uint8_t)sectors, buf))
        return -1;
    return (int)size;
}

/* Enumeration is compacted over used slots, so gaps from deletes are hidden. */
static int aquafs_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (slot_used(i)) n++;
    return n;
}

static int nth_used(int disp)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (slot_used(i) && disp-- == 0)
            return i;
    return -1;
}

static const char *aquafs_ent_name(int disp)
{
    int i = nth_used(disp);
    return i < 0 ? "" : slot[i].name;
}

static int aquafs_ent_size(int disp)
{
    int i = nth_used(disp);
    return i < 0 ? 0 : (int)slot[i].size;
}

/* --- write ops --- */
static int aquafs_write(const char *name, const void *buf, int size)
{
    if (size < 0 || size > (int)(SLOT_SECTORS * SECTOR))
        return -1;

    int i = find(name);
    if (i < 0) {                                   /* create: take a free slot */
        for (int s = 0; s < MAX_FILES; s++)
            if (!slot_used(s)) { i = s; break; }
        if (i < 0)
            return -1;                             /* directory full */
        int n = 0;
        while (name[n] && n < 47) { slot[i].name[n] = name[n]; n++; }
        slot[i].name[n] = '\0';
        slot[i].start_lba = DATA_LBA + i * SLOT_SECTORS;
    }

    uint32_t sectors = (size + SECTOR - 1) / SECTOR;
    if (sectors == 0)
        sectors = 1;
    memset(bounce, 0, sectors * SECTOR);
    memcpy(bounce, buf, (size_t)size);
    if (ata_write(slot[i].start_lba, (uint8_t)sectors, bounce))
        return -1;

    slot[i].size = (uint32_t)size;
    if (flush_dir())
        return -1;
    return size;
}

static int aquafs_delete(const char *name)
{
    int i = find(name);
    if (i < 0)
        return -1;
    slot[i].name[0] = '\0';                         /* mark free */
    return flush_dir();
}

static void aquafs_list(void)
{
    kprintf("[fs] %d file(s):\n", aquafs_count());
    for (int i = 0; i < MAX_FILES; i++)
        if (slot_used(i))
            kprintf("[fs]   %-16s %u bytes\n", slot[i].name, slot[i].size);
}

struct filesystem aquafs = {
    .name     = "aquafs",
    .mount    = aquafs_mount,
    .list     = aquafs_list,
    .size     = aquafs_size,
    .read     = aquafs_read,
    .count    = aquafs_count,
    .ent_name = aquafs_ent_name,
    .ent_size = aquafs_ent_size,
    .write    = aquafs_write,
    .del      = aquafs_delete,
};
