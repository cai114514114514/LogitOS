/* SPDX-License-Identifier: MIT
 * Destructive-to-guest, terminal WIDEVERIFY-only probe. The launcher attaches
 * private disks and ends QEMU after the serial result. Never resume userspace
 * once the root disk and display have been removed. */
#include "driver.h"
#include "dma.h"
#include "pmm.h"
#include "kprintf.h"
#include "serial.h"
static int equal(const char *a,const char *b) {
    while(*a&&*a==*b){a++;b++;}return *a==*b;
}
static int stage(const char *name) {
    if(equal(name,"xhci"))return 0;
    if(equal(name,"hda"))return 1;
    if(equal(name,"e1000")||equal(name,"rtl8139")||equal(name,"rtl8169")||equal(name,"virtio-net"))return 2;
    if(equal(name,"virtio-balloon")||equal(name,"virtio-rng"))return 3;
    if(equal(name,"virtio-gpu"))return 4;
    if(equal(name,"virtio-blk")||equal(name,"nvme")||equal(name,"ahci"))return 6;
    return 5;
}
int dma_shutdown_verify(void) {
    struct dma_stats before,after;dma_get_stats(&before);
    serial_puts("DMA_SHUTDOWN_BEGIN\n");
    unsigned removed=0;
    for(int pass=0;pass<7;pass++)for(int i=0;i<dev_count();i++) {
        struct device *dev=dev_at(i);
        if(!dev->drv||!dev->drv->remove||stage(dev->drv->name)!=pass)continue;
        char line[160];
        ksnprintf(line,sizeof line,"DMA_SHUTDOWN_REMOVE device=%s driver=%s stage=%d\n",dev->name,dev->drv->name,pass);
        serial_puts(line);dev_unbind(dev);removed++;
    }
    dma_get_stats(&after);
    int audit=pmm_audit();
    int ok=removed&&before.coherent_buffers&&after.coherent_buffers==0&&after.coherent_bytes==0&&
        after.active_mappings==0&&after.pinned_pages==0&&after.direct_bytes==0&&after.bounce_bytes==0&&
        after.quarantined_buffers==0&&after.quarantined_mappings==0&&after.quarantined_bytes==0&&
        audit==0&&pmm_bugs()==0;
    char line[320];
    ksnprintf(line,sizeof line,"DMA_SHUTDOWN_COUNTS removed=%u coherent=%llu bytes=%llu mappings=%llu pins=%llu direct=%llu bounce=%llu quarantine=%llu/%llu/%llu audit=%d bugs=%llu\n",
        removed,after.coherent_buffers,after.coherent_bytes,after.active_mappings,after.pinned_pages,
        after.direct_bytes,after.bounce_bytes,after.quarantined_buffers,after.quarantined_mappings,
        after.quarantined_bytes,audit,pmm_bugs());
    serial_puts(line);
    serial_puts(ok?"DMA_SHUTDOWN_PASS\n":"DMA_SHUTDOWN_FAIL\n");
    for(;;)__asm__ volatile("cli; hlt");
}
