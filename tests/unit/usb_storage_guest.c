/* SPDX-License-Identifier: MIT
 * Opt-in kernel verification after actual USB enumeration. A private marker,
 * specific partition geometry and usb prefix are all prerequisites for writes.
 * The runner only initializes metadata/guards. Every payload is written,
 * flushed and compared by the guest through the production block/BOT/HCD path.
 * Phase in media selects cold boot 2; no harness-supplied success flag exists. */
#include "usb_storage_fixture.h"
#include "blkdev.h"
#include "driver.h"
#include "dma.h"
#include "pmm.h"
#include "xhci.h"
#include "kprintf.h"
#include <stdint.h>
#include <stddef.h>
static unsigned checks,failures;
static void check(int ok,const char *what)
{checks++;if(!ok)failures++;kprintf("USB_MSC_GUEST_CHECK %s %s\n",what,ok?"PASS":"FAIL");}
static uint8_t pattern(size_t i,unsigned generation)
{return (uint8_t)((i*(37+generation*2))^(i>>8)^(0x53+generation*11));}
static int all(const uint8_t *p,size_t n,uint8_t value)
{for(size_t i=0;i<n;i++)if(p[i]!=value)return 0;return 1;}

static unsigned verify_disk(unsigned index)
{
    unsigned phase=0,identity=0,before_checks=checks,before_failures=failures;
    struct dma_device owner;dma_device_init(&owner,"usb-msc-guest-data",DMA_MASK_64);
    const size_t bytes=USB_TEST_DATA_SECTORS*BLK_SECTOR;
    struct dma_buffer *write=NULL,*read=NULL;
    char disk_name[]="usb0",part_name[]="usb0p1";
    disk_name[3]=(char)('0'+index);part_name[3]=(char)('0'+index);
    struct blkdev *whole=blk_find(disk_name),*part=blk_find(part_name);
    check(whole&&part&&part->parent==whole,"late-usb-partition-published");
    if(!whole||!part||part->parent!=whole)goto done;
    check(part->start==USB_TEST_PART_START&&part->nsectors==USB_TEST_PART_SECTORS&&
          whole->nsectors==USB_TEST_SECTORS&&whole!=blk_root(),"private-medium-geometry-and-root-exclusion");
    if(failures)goto done;
    write=dma_alloc_coherent(&owner,bytes+4096,4096,0);
    read=dma_alloc_coherent(&owner,bytes+4096,4096,0);
    check(write&&read,"independent-physical-payload-buffers");
    if(!write||!read)goto done;
    uint8_t *w=(uint8_t*)write->cpu+37,*r=(uint8_t*)read->cpu+123;
    int high=pmm_total_bytes()>(UINT64_C(1)<<32);
    check(!high||(write->phys>UINT32_MAX&&read->phys>UINT32_MAX),"payloads-use-high-physical-RAM-when-present");
    kprintf("USB_MSC_DMA_BUFFER write=%p read=%p cpu_write=%p bytes=%u high_required=%u\n",
            (void*)(uintptr_t)(write->phys+37),(void*)(uintptr_t)(read->phys+123),w,(unsigned)bytes,(unsigned)high);
    if(failures)goto done;
    uint8_t header[512];
    check(!blk_dev_read(part,0,1,header),"read-private-partition-header");
    if(failures)goto done;
    static const char magic[]=USB_TEST_MAGIC;
    int valid=1;
    for(unsigned i=0;i<sizeof magic;i++)if(header[i]!=(uint8_t)magic[i])valid=0;
    phase=header[USB_TEST_PHASE_OFFSET];
    identity=header[USB_TEST_ID_OFFSET];
    check(valid&&phase<=1&&identity<2,"private-marker-and-cold-boot-phase");
    if(failures)goto done;
    check(!blk_dev_read(part,USB_TEST_DATA_START,USB_TEST_DATA_SECTORS,r),"read-existing-data-through-partition-offset");
    int equal=1;
    for(size_t i=0;i<bytes;i++)if(r[i]!=(phase?pattern(i,phase+identity*3):0))equal=0;
    check(equal,phase?"cold-boot-retained-every-written-byte":"initial-private-data-is-zero");
    if(failures)goto done;
    for(size_t i=0;i<bytes;i++)w[i]=pattern(i,phase+1+identity*3);
    check(!blk_dev_write(part,USB_TEST_DATA_START,USB_TEST_DATA_SECTORS,w),"write-crosses-BOT-command-chunks");
    check(!blk_dev_flush(part),"SYNCHRONIZE-CACHE-completed");
    for(size_t i=0;i<bytes;i++)r[i]=0xa5;
    check(!blk_dev_read(part,USB_TEST_DATA_START,USB_TEST_DATA_SECTORS,r),"read-back-completed-write");
    equal=1;for(size_t i=0;i<bytes;i++)if(r[i]!=w[i])equal=0;
    check(equal,"compare-every-byte-after-flush");
    check(!blk_dev_read(whole,USB_TEST_PART_START-1,1,r)&&all(r,512,USB_TEST_GUARD_PRE),"partition-leading-guard-unchanged");
    check(!blk_dev_read(part,USB_TEST_DATA_START+USB_TEST_DATA_SECTORS,1,r)&&all(r,512,USB_TEST_GUARD_POST),"payload-trailing-guard-unchanged");
    if(failures)goto done;
    header[USB_TEST_PHASE_OFFSET]=(uint8_t)(phase+1);
    check(!blk_dev_write(part,0,1,header)&&!blk_dev_flush(part),"persist-next-cold-boot-phase-last");
    /* Read the real DMA address used by Normal TRBs, after payload equality.
     * With low staging this is an explicit high-CPU/low-device copy path;
     * with high staging it proves that the real controller reads high RAM.
     * QEMU has exactly one storage device, so these are its two bulk queues. */
    if(g_xhci.up)for(int slot=1;slot<=XHCI_MAX_SLOTS;slot++)for(int ep=2;ep<32;ep++){
        struct xhci_ep *e=g_xhci.slot[slot].ep[ep];
        if(e&&e->xfer==2)kprintf("USB_MSC_XHCI_DMA ep=%u dma=%p mask=%p\n",(unsigned)ep,
                 (void*)(uintptr_t)e->buf_dma,(void*)(uintptr_t)g_xhci.dma.mask);
    }
    phase++;
done:
    dma_free_coherent(write);dma_free_coherent(read);
    kprintf("USB_MSC_DISK_RESULT disk=%s identity=%u phase=%u checks=%u failed=%u\n",
            disk_name,identity,phase,checks-before_checks,failures-before_failures);
    return phase;
}
void usb_msc_verify_dev_dump(void)
{
    dev_dump();
    unsigned first=verify_disk(0);
    if(blk_find("usb1")) {
        unsigned second=verify_disk(1);
        check(first==second,"independent-media-complete-the-same-cold-boot-phase");
    }
    kprintf("USB_MSC_GUEST_RESULT phase=%u checks=%u failed=%u\n",first,checks,failures);
}
