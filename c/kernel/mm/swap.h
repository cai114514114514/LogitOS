#ifndef LOGIT_SWAP_H
#define LOGIT_SWAP_H

#include <stdint.h>

/* THE BACKING STORE. Somewhere to put a page whose contents nothing else can
 * reproduce.
 *
 * ---------------------------------------------------------------------------
 * SWAP IS A PLACE, RECLAIM IS THE MECHANISM. This file is only the place. It
 * knows nothing about page tables, victims or pressure; it hands out numbered
 * 4 KiB slots on a block device and moves bytes in and out of them. reclaim.c
 * decides what goes in one. Keeping that boundary is what lets the first tier of
 * reclaim -- dropping a page that needs no backing store at all -- exist
 * without this file being involved.
 *
 * ---------------------------------------------------------------------------
 * WHICH DEVICE, AND THE REFUSAL THAT MATTERS MOST
 *
 * Swap needs a raw block device. c/fs/ and c/drivers/block/ belong to a
 * live filesystem line, so nothing here edits either: swap uses ONLY the
 * public registry in blkdev.h (blk_at / blk_dev_read / blk_dev_write), which
 * already bounds every access to the device's own length. No new interface was
 * needed and none was taken.
 *
 * Picking the device is the dangerous part, because getting it wrong overwrites
 * a filesystem. A device is eligible only if ALL of these hold:
 *
 *   1. it is not the root device;
 *   2. it is not a partition of the root device, and the root is not a
 *      partition of it (a partition write lands in the parent either way);
 *   3. it does not carry a LogitFS superblock;
 *   4. its first sector is either already our swap header, or ALL ZEROES.
 *
 * (4) is the one that does the real work. A device with anything at all in
 * sector 0 that we do not recognise is left alone -- an unknown filesystem, a
 * partition table, a boot sector. Swap would rather not exist than eat a disk,
 * so the failure mode of every ambiguity here is "no swap this boot", said out
 * loud on the console.
 *
 * ---------------------------------------------------------------------------
 * SWAP IS NOT PERSISTENT. The header is rewritten at every boot and the slot
 * table lives only in RAM. A swapped page belongs to a process, and no process
 * survives the reboot, so there is nothing on that device worth keeping -- and
 * a swap area that IS read back after a reboot is a way to hand one boot's
 * memory to the next boot's processes.
 *
 * ---------------------------------------------------------------------------
 * SLOT REFERENCE COUNTS. A slot is refcounted for the same reason a frame is:
 * when a shared (copy-on-write) frame is evicted, every address space that
 * mapped it gets a PTE pointing at the SAME slot, and the slot may only be
 * reused when the last of them has faulted it back in or gone away. The count
 * mirrors the frame refcount at eviction time and is decremented by exactly the
 * places a swap PTE is destroyed.
 *
 * KNOWN SEMANTIC COST, stated rather than discovered: a shared page that makes
 * a full round trip through swap comes back PRIVATE to each faulter. The
 * contents are identical, so nothing observable changes; the sharing is lost,
 * so a frame that was one becomes several. Restoring the sharing would need a
 * swap-cache keyed by slot, which is real work and is not here. */

#define SWAP_PAGE_SIZE   4096
#define SWAP_SECTORS     (SWAP_PAGE_SIZE / 512)     /* sectors per slot */
#define SWAP_HDR_SECTORS 8                          /* one page of header */
#define SWAP_MAGIC       0x5753474Cu                /* "LGSW" little-endian */
#define SWAP_VERSION     1

/* No slot ever has this value; it is what "not swapped" means in a PTE and what
 * swap_alloc_slot() returns when the area is full. */
#define SWAP_NOSLOT      ((uint64_t)0)
/* Slot numbers therefore start at 1. Slot 0 is never handed out, so a zeroed
 * PTE can never be mistaken for a valid swap entry -- the same reason frame 0
 * is reserved in pmm.c. */

/* Find, validate and format a swap device. Safe to call when there is none;
 * swap_ready() then stays 0 and reclaim runs with the drop tier only. */
int  swap_init(void);
int  swap_ready(void);
const char *swap_dev_name(void);

/* Reserve a slot (refcount 1), or SWAP_NOSLOT if the area is full.
 *
 * ALLOCATES NOTHING. The refcount table is sized and taken from the PMM at
 * init, before any pressure can exist, precisely because this runs on the path
 * that is trying to FREE memory -- see reclaim.h on the allocate-to-free
 * deadlock. */
uint64_t swap_alloc_slot(void);
void     swap_slot_ref(uint64_t slot);      /* one more PTE points here */
void     swap_slot_put(uint64_t slot);      /* one fewer; freed at zero */
unsigned swap_slot_refs(uint64_t slot);

/* Move one 4 KiB page. `page` is a kernel-addressable pointer to the whole
 * frame. Return 0 on success.
 *
 * BOTH CAN SLEEP. Concurrent callers queue on a busy flag using bkl_hlt_wait(),
 * which DROPS THE BIG KERNEL LOCK while it waits -- a fault that needs the disk
 * must not stop every other core. What it does not yet do is drop the BKL
 * across the single in-flight transfer itself; see the long comment in swap.c
 * for the measurement and for the exact block-layer interface that would fix
 * it. */
int  swap_write_page(uint64_t slot, const void *page);
int  swap_read_page(uint64_t slot, void *page);

/* --- accounting --------------------------------------------------------- */
uint64_t swap_slots_total(void);
uint64_t swap_slots_used(void);
uint64_t swap_writes(void);          /* pages written out */
uint64_t swap_reads(void);           /* pages read back in */
uint64_t swap_io_errors(void);
uint64_t swap_waits(void);           /* times a caller queued behind another */
uint64_t swap_bkl_cycles(void);      /* cycles the BKL was held inside the device */
uint64_t swap_bkl_worst(void);       /* worst single transfer, cycles */
void     swap_report(const char *tag);

#endif /* LOGIT_SWAP_H */
