#ifndef LOGIT_PART_H
#define LOGIT_PART_H

#include <stdint.h>
#include <stddef.h>

/* Partition-table parsing: MBR (including the extended/logical chain) and GPT
 * (including the header and entry-array CRC32s and the protective-MBR case).
 *
 * This file knows nothing about controllers. It reads sectors through a
 * callback, which is what makes it the part of the storage stack that can be
 * tested exhaustively on the host against synthetic images -- and it is the part
 * that most needs it, because every field here comes off a disk somebody else
 * formatted and none of it can be trusted.
 *
 * Sector size is fixed at 512: every device the block layer registers reports
 * 512-byte sectors (nvme.c rejects anything else at init), and both table
 * formats are defined in terms of LBAs of that size. */

#define PART_SECTOR     512
#define PART_MAX        16      /* partitions kept per device */
#define PART_NAME_MAX   37      /* GPT name: 36 UTF-16 units + NUL */

enum { PART_NONE = 0, PART_MBR = 1, PART_GPT = 2 };

struct part_entry {
    uint64_t start;             /* absolute first LBA on the device */
    uint64_t count;             /* length in 512-byte sectors */
    uint8_t  type_mbr;          /* MBR partition type byte (0 for GPT) */
    uint8_t  type_guid[16];     /* GPT partition type GUID (zero for MBR) */
    uint8_t  bootable;          /* MBR status 0x80 / GPT legacy-BIOS-bootable attr */
    uint8_t  logical;           /* MBR: found by walking the extended chain */
    char     name[PART_NAME_MAX]; /* GPT name, transliterated to ASCII; "" for MBR */
};

struct part_table {
    int scheme;                 /* PART_NONE / PART_MBR / PART_GPT */
    int count;                  /* entries in e[] */
    int skipped;                /* entries rejected: empty, out of range, malformed */
    int overlaps;               /* pairs of accepted partitions that overlap */
    int protective;             /* GPT was reached through a protective MBR (type 0xEE) */
    int backup_used;            /* the primary GPT header was bad; the backup was used */
    int truncated;              /* the device has more partitions than PART_MAX */
    struct part_entry e[PART_MAX];
};

/* Read `count` 512-byte sectors at `lba`. Must return 0 on success, non-zero on
 * failure -- a failed read is treated as "no table here", never as zeroes. */
typedef int (*part_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* Scan the table on a device of `dev_sectors` 512-byte sectors. Always fills
 * *t (zeroed first), so a device with no table comes back as scheme PART_NONE
 * and count 0 rather than as an error. Returns the number of partitions found,
 * or -1 if the arguments are unusable. */
int part_scan(part_read_fn rd, void *ctx, uint64_t dev_sectors, struct part_table *t);

const char *part_scheme_name(int scheme);
/* "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" into a 37-byte buffer. */
void part_guid_str(const uint8_t guid[16], char out[37]);

#endif /* LOGIT_PART_H */
