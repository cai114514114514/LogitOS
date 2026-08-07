#ifndef LOGIT_AHCI_H
#define LOGIT_AHCI_H

#include <stdint.h>

/* AHCI/SATA host bus adapter driver (PCI class 01:06:01).
 *
 * This is the one storage controller a physical x86 machine is essentially
 * certain to have: virtio-blk only exists under a hypervisor, NVMe only on
 * machines new enough, and IDE PIO is legacy compatibility that modern
 * chipsets increasingly do not offer at all. Everything else in this tree found
 * its disk because QEMU was told to provide a specific device.
 *
 * Probe every implemented port, bring up the ones with a SATA disk on them, and
 * register each as a block device. Returns the number of disks found. */
int ahci_init(void);

/* Number of SATA disks brought up (0 before ahci_init). */
int ahci_disk_count(void);

#endif /* LOGIT_AHCI_H */
