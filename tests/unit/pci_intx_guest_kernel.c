/* SPDX-License-Identifier: MIT */
/* Test-only hook after dev_probe_all. Hardware events come from TWO QEMU EDU
 * functions, not from calling the dispatcher. Static contexts stay readable
 * after retirement so a stale callback is detected without creating a guest
 * use-after-free. Only the test kmain object renames dev_dump to this wrapper. */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "irq.h"
#include "pci.h"
#include "acpi.h"
#include "ioapic.h"
#include "ktime.h"
#include "kprintf.h"

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8
#define EDU_IRQ_STATUS (0x24 / 4)
#define EDU_IRQ_RAISE (0x60 / 4)
#define EDU_IRQ_ACK (0x64 / 4)
#define TOKEN_A 0x21u
#define TOKEN_B 0x42u

struct edu_irq_context {
    volatile uint32_t *mmio;
    unsigned callbacks, received, payload, retired, stale;
};
static struct edu_irq_context contexts[2];
static unsigned checks, failures, sentinel_calls;
static const struct driver test_owner = {.name="pci-intx-guest",.bus_type=DEV_BUS_PCI};

static void verify(int yes, const char *what)
{
    ++checks;
    if (!yes) ++failures;
    kprintf("PCI_INTX_GUEST_CHECK %s %s\n", what, yes ? "PASS" : "FAIL");
}
static unsigned read_counter(unsigned *counter)
{ return __atomic_load_n(counter, __ATOMIC_ACQUIRE); }
static void edu_irq(void *arg)
{
    struct edu_irq_context *ctx=arg;
    __atomic_fetch_add(&ctx->callbacks,1,__ATOMIC_RELAXED);
    if (read_counter(&ctx->retired)) __atomic_fetch_add(&ctx->stale,1,__ATOMIC_RELAXED);
    uint32_t status=ctx->mmio[EDU_IRQ_STATUS];
    if (!status) return;
    ctx->mmio[EDU_IRQ_ACK]=status;
    __atomic_fetch_or(&ctx->payload,status,__ATOMIC_RELAXED);
    __atomic_fetch_add(&ctx->received,1,__ATOMIC_RELEASE);
}
static void sentinel_irq(void *arg)
{ (void)arg; __atomic_fetch_add(&sentinel_calls,1,__ATOMIC_RELEASE); }
static void settle_ms(unsigned milliseconds)
{
    uint64_t until=time_mono_ms()+milliseconds;
    while(time_mono_ms()<until) __asm__ volatile("pause" ::: "memory");
}
static int wait_payload(struct edu_irq_context *ctx, unsigned mask)
{
    uint64_t until=time_mono_ms()+1000;
    while ((read_counter(&ctx->payload)&mask)!=mask && time_mono_ms()<until)
        __asm__ volatile("pause" ::: "memory");
    return (read_counter(&ctx->payload)&mask)==mask;
}
static void source_enable(struct device *dev, int enable)
{
    uint16_t command=pci_cfg_read16(dev->bus,dev->slot,dev->func,PCI_CFG_COMMAND);
    command=enable ? command&~PCI_CMD_INTX_DIS : command|PCI_CMD_INTX_DIS;
    pci_cfg_write16(dev->bus,dev->slot,dev->func,PCI_CFG_COMMAND,command);
}
static int source_disabled(struct device *dev)
{ return !!(pci_cfg_read16(dev->bus,dev->slot,dev->func,PCI_CFG_COMMAND)&PCI_CMD_INTX_DIS); }
static uint32_t gsi_of(const struct device *dev)
{ return dev->irq_line<16 ? acpi_gsi_for_irq(dev->irq_line) : dev->irq_line; }

void pci_intx_guest_dev_dump(void)
{
    dev_dump();
    struct device *a=dev_find_id(EDU_VENDOR,EDU_DEVICE,NULL);
    struct device *b=a ? dev_find_id(EDU_VENDOR,EDU_DEVICE,a) : NULL;
    verify(a&&b,"two-physical-functions");
    if (!a||!b) goto done;
    uint32_t ga=gsi_of(a),gb=gsi_of(b);
    kprintf("PCI_INTX_GUEST_ROUTE a=%s pin=%u line=%u gsi=%u b=%s pin=%u line=%u gsi=%u\n",
            a->name,a->irq_pin,a->irq_line,ga,b->name,b->irq_pin,b->irq_line,gb);
    verify(a->irq_pin&&b->irq_pin&&a->irq_line!=255&&b->irq_line!=255&&ga==gb,"actual-shared-gsi");
    if (!a->irq_pin||!b->irq_pin||ga!=gb) goto done;

    /* The ordinary edu worked example has a singleton context. Unbind its
     * self-test registrations before installing our two independent cookies. */
    dev_unbind(a);dev_unbind(b);
    struct device *device[2]={a,b};
    for(int i=0;i<2;++i) {
        dev_enable(device[i],1);
        contexts[i].mmio=(volatile uint32_t *)(uintptr_t)dev_bar_map(device[i],0);
        if(!contexts[i].mmio) { verify(0,"mapped-edu-registers");goto done; }
        contexts[i].mmio[EDU_IRQ_ACK]=0xffffffffu;
    }
    int old_preference=dev_irq_prefer(DEV_IRQ_INTX);
    int va=dev_irq_request(a,edu_irq,&contexts[0],"shared-edu-a");
    int vb=dev_irq_request(b,edu_irq,&contexts[1],"shared-edu-b");
    dev_irq_prefer(old_preference);
    verify(va>=0&&vb>=0&&a->irq_mode==DEV_IRQ_INTX&&b->irq_mode==DEV_IRQ_INTX,"both-use-intx");
    verify(va>=0&&va==vb,"same-vector-for-shared-gsi");
    if(va<0||vb<0) {dev_irq_release(a);dev_irq_release(b);goto done;}
    a->drv=&test_owner;b->drv=&test_owner;
    kprintf("PCI_INTX_GUEST_BOUND gsi=%u vector_a=%d vector_b=%d\n",ga,va,vb);
    uint64_t flags;__asm__ volatile("pushfq; pop %0":"=r"(flags)::"memory");
    /* Assert both devices before delivery. A dispatcher that calls only its
     * first member leaves B's hardware status pending and fails the payload
     * assertion, even though a real interrupt arrived on the shared vector. */
    __asm__ volatile("cli");
    contexts[0].mmio[EDU_IRQ_RAISE]=TOKEN_A;
    contexts[1].mmio[EDU_IRQ_RAISE]=TOKEN_B;
    __asm__ volatile("sti");
    verify(wait_payload(&contexts[0],TOKEN_A),"dual-a-payload");
    verify(wait_payload(&contexts[1],TOKEN_B),"dual-b-payload");
    contexts[0].mmio[EDU_IRQ_ACK]=0xffffffffu;
    contexts[1].mmio[EDU_IRQ_ACK]=0xffffffffu;
    settle_ms(20);
    verify(read_counter(&contexts[0].received)&&read_counter(&contexts[1].received),"both-device-statuses-serviced");

    dev_unbind(a);
    __atomic_store_n(&contexts[0].retired,1,__ATOMIC_RELEASE);
    verify(source_disabled(a),"first-owner-source-disabled");
    verify(ioapic_is_masked(ga)==0,"remaining-owner-gsi-unmasked");
    unsigned before=read_counter(&contexts[1].received);
    contexts[1].mmio[EDU_IRQ_RAISE]=0x84u;
    verify(wait_payload(&contexts[1],0x84u)&&read_counter(&contexts[1].received)>before,"survivor-after-peer-unbind");
    settle_ms(20);
    verify(!read_counter(&contexts[0].stale),"retired-first-cookie-never-called");
    contexts[1].mmio[EDU_IRQ_ACK]=0xffffffffu;

    dev_unbind(b);
    __atomic_store_n(&contexts[1].retired,1,__ATOMIC_RELEASE);
    verify(source_disabled(b),"last-owner-source-disabled");
    verify(ioapic_is_masked(ga)==1,"last-owner-gsi-masked");
    /* Reuse the released vector, then deliberately re-enable BOTH hardware
     * sources. Leaving PCI_CMD_INTX_DIS set would hide a missing route mask and
     * make the negative control pass without testing the I/O APIC at all. */
    int allocated[IRQ_NVEC],nallocated=0,got_vector=0;
    for(int i=0;i<IRQ_NVEC;++i) {
        int vector=irq_alloc_vector(sentinel_irq,NULL,"released-intx-sentinel");
        if(vector<0)break;
        allocated[nallocated++]=vector;
        if(vector==va){got_vector=1;break;}
    }
    verify(got_vector,"released-vector-can-be-reused");
    source_enable(a,1);source_enable(b,1);
    verify(!source_disabled(a)&&!source_disabled(b),"retired-hardware-sources-reenabled-for-mask-test");
    contexts[0].mmio[EDU_IRQ_RAISE]=0x10u;
    contexts[1].mmio[EDU_IRQ_RAISE]=0x20u;
    settle_ms(60);
    verify(!read_counter(&sentinel_calls),"masked-gsi-cannot-hit-reused-vector");
    verify(!read_counter(&contexts[0].stale)&&!read_counter(&contexts[1].stale),"no-retired-cookie-after-last-unbind");
    contexts[0].mmio[EDU_IRQ_ACK]=0xffffffffu;
    contexts[1].mmio[EDU_IRQ_ACK]=0xffffffffu;
    source_enable(a,0);source_enable(b,0);
    settle_ms(20);
    for(int i=0;i<nallocated;++i)irq_free_vector(allocated[i]);
    if(!(flags&0x200))__asm__ volatile("cli");
    kprintf("PCI_INTX_GUEST_COUNTS callbacks=%u/%u received=%u/%u stale=%u/%u sentinel=%u\n",
            contexts[0].callbacks,contexts[1].callbacks,contexts[0].received,contexts[1].received,
            contexts[0].stale,contexts[1].stale,sentinel_calls);
 done:
    kprintf("PCI_INTX_GUEST_DONE result=%s checks=%u failed=%u\n",failures?"FAIL":"PASS",checks,failures);
}
