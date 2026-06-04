#include <stdint.h>
#include "ata.h"
#include "io.h"

#define ATA_DATA     0x1F0
#define ATA_FEATURES 0x1F1
#define ATA_SECCOUNT 0x1F2
#define ATA_LBA0     0x1F3
#define ATA_LBA1     0x1F4
#define ATA_LBA2     0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_CMD      0x1F7   /* write: command */
#define ATA_STATUS   0x1F7   /* read: status   */
#define ATA_CTRL     0x3F6   /* alternate status / device control */

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7

/* Bounded BSY wait. Returns 0 when idle, -1 on timeout. The bound is generous
 * because under QEMU TCG SMP the IDE device thread competes with the AP cores
 * (framebuffer present) for the big QEMU lock, so command completion can lag. */
static int wait_not_busy(void)
{
    for (long i = 0; i < 50000000; i++)
        if (!(inb(ATA_STATUS) & ST_BSY))
            return 0;
    return -1;
}

/* Wait until the drive is ready to transfer a sector, or report an error. */
static int wait_drq(void)
{
    for (long i = 0; i < 50000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR)
            return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ))
            return 0;
    }
    return -1;
}

/* One read attempt; 0 on success, <0 on any controller hiccup. */
static int ata_read_once(uint32_t lba, uint8_t count, uint16_t *out)
{
    if (wait_not_busy()) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* LBA mode, master */
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, CMD_READ_SECTORS);

    for (int i = 0; i < 4; i++)                     /* ~400ns settle */
        (void)inb(ATA_CTRL);

    for (int s = 0; s < count; s++) {
        if (wait_drq())
            return -1;
        for (int i = 0; i < 256; i++)
            out[s * 256 + i] = inw(ATA_DATA);
    }
    return 0;
}

/* Retry transient failures: a timed-out PIO transfer under SMP contention must
 * not be reported as a read error (callers like dir_lookup would treat it as
 * "block missing" and silently corrupt a lookup). Re-issue the whole command. */
int ata_read(uint32_t lba, uint8_t count, void *buf)
{
    for (int attempt = 0; attempt < 8; attempt++)
        if (ata_read_once(lba, count, (uint16_t *)buf) == 0)
            return 0;
    return -1;
}

static int ata_write_once(uint32_t lba, uint8_t count, const uint16_t *in)
{
    if (wait_not_busy()) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* LBA mode, master */
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, CMD_WRITE_SECTORS);

    for (int s = 0; s < count; s++) {
        if (wait_drq())
            return -1;
        for (int i = 0; i < 256; i++)
            outw(ATA_DATA, in[s * 256 + i]);
    }
    outb(ATA_CMD, CMD_FLUSH_CACHE);                 /* commit to media */
    return wait_not_busy();
}

int ata_write(uint32_t lba, uint8_t count, const void *buf)
{
    for (int attempt = 0; attempt < 8; attempt++)
        if (ata_write_once(lba, count, (const uint16_t *)buf) == 0)
            return 0;
    return -1;
}
