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

#define CMD_READ_SECTORS 0x20

static void wait_not_busy(void)
{
    while (inb(ATA_STATUS) & ST_BSY)
        ;
}

/* Wait until the drive is ready to transfer a sector, or report an error. */
static int wait_drq(void)
{
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR)
            return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ))
            return 0;
    }
    return -1;
}

int ata_read(uint32_t lba, uint8_t count, void *buf)
{
    uint16_t *out = (uint16_t *)buf;

    wait_not_busy();
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* LBA mode, master */
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, CMD_READ_SECTORS);

    /* ~400ns settle: read alternate status a few times. */
    for (int i = 0; i < 4; i++)
        (void)inb(ATA_CTRL);

    for (int s = 0; s < count; s++) {
        if (wait_drq())
            return -1;
        for (int i = 0; i < 256; i++)
            out[s * 256 + i] = inw(ATA_DATA);
    }
    return 0;
}
