/* SPDX-License-Identifier: MIT
 * A CPU-only test driver bound to the otherwise unclaimed host bridge. Its
 * relocated callback calls low kernel exports while its state uses high heap.
 * It neither configures nor performs DMA to the bridge. */
#include "driver.h"
#include "kheap.h"
#include "kprintf.h"
#include "physmap.h"
static unsigned calls;
static const char label[]="heap-module";
static const char *message=label; /* relocated data pointer */
static int probe(struct device *d)
{
    (void)d;unsigned char *p=kmalloc(8192);int ok=p&&(uintptr_t)p>=PHYSMAP_BASE;
    if(p){for(unsigned i=0;i<8192;i++)p[i]=(unsigned char)i;for(unsigned i=0;i<8192;i++)if(p[i]!=(unsigned char)i)ok=0;}
    ++calls;ok=ok&&calls==1&&message[0]=='h'&&(uintptr_t)probe<PMM_LOW_LIMIT;
    kprintf("[highheap-module] %s text=%p cpu=%p calls=%u label=%s\n",ok?"PASS":"FAIL",probe,p,calls,message);
    kfree(p);return ok?0:-1;
}
static const struct dev_match ids[]={
    {DEV_ANY,DEV_ANY,0x06,0x00,DEV_ANYC,0},DEV_MATCH_END
};
static struct driver heap_driver={.name="heap-module",.bus_type=DEV_BUS_PCI,.match=ids,.probe=probe};
DRIVER_DECLARE(heap_driver);
