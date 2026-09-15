/* SPDX-License-Identifier: MIT
 * Real PCI fanout + IRQ retirement + IOAPIC RTE functions. Only config/MMIO,
 * IDT, CPU context and EOI leaves are doubles; the runner extracts no policy. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include "pci.h"
#include "driver.h"
#include "irq.h"
#include "io_lock.h"
#include "ioapic.h"
#include "apic_model.h"

static int checks, failures;
#define CHECK(c,m) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n",m); } } while (0)
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static uint64_t spin_lock_irqsave(spinlock_t *l) { pthread_mutex_lock(l); return 0; }
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) { (void)f; pthread_mutex_unlock(l); }
#define IRQ_STUB_STRIDE 16
static uint8_t irq_stub_base[IRQ_NVEC * IRQ_STUB_STRIDE];
static uint8_t *irq_stub_end = irq_stub_base + sizeof irq_stub_base;
static void install_gate(int v,const void *s) { (void)v; (void)s; }
void kprintf(const char *fmt,...) { (void)fmt; }
struct cpu { int in_kernel; };
static _Thread_local struct cpu cpu;
static struct cpu *this_cpu(void) { return &cpu; }
struct registers { uint64_t vector; };
static atomic_int eois, hold_eoi, eoi_entered, release_eoi;
int lapic_ready(void) { return 1; }
uint32_t lapic_id(void) { return 3; }
void lapic_eoi(void)
{
    if (atomic_load(&hold_eoi)) {
        atomic_store(&eoi_entered, 1);
        while (!atomic_load(&release_eoi)) sched_yield();
    }
    atomic_fetch_add(&eois, 1);
}
void pic_eoi(int v) { (void)v; }
#include "irq_body.c"

/* Register leaves run under the real ioapic_gate. Keep sequence numbers to
 * check that source disable precedes the last-owner mask transaction. */
static io_lock_t ioapic_gate;
static unsigned ioapic_ready = 1;
static uint32_t gsi_base;
static int max_rte = 47;
static uint32_t rte[256];
static atomic_uint sequence, disable_seq[32], mask_seq[48];
static atomic_int reject_mask, reject_route, reject_program_low;
static atomic_int duplicate_legacy_gsi;
static atomic_int poison_after_enable, reject_remask;
static unsigned enabled_once[48];
static unsigned route_writes[48], mask_reads[48];
static uint32_t rd(uint8_t reg)
{
    if (reg >= 0x10 && !(reg & 1) && (rte[reg] & (1u << 16)))
        mask_reads[(reg - 0x10) / 2]++;
    return rte[reg];
}
static void wr(uint8_t reg,uint32_t value)
{
    if (reg >= 0x11 && (reg & 1) &&
        atomic_load(&reject_route) == (int)((reg - 0x11) / 2) + 1)
        return;
    if (reg >= 0x10 && !(reg & 1)) {
        unsigned n = (reg - 0x10) / 2;
        if (value & (1u << 16)) {
            atomic_store(&mask_seq[n], atomic_fetch_add(&sequence, 1) + 1);
            if ((value & 0xffffu) && atomic_load(&reject_program_low) == (int)n + 1)
                return;
            if (enabled_once[n] && atomic_load(&reject_remask) == (int)n + 1)
                return;
            if (atomic_load(&reject_mask) == (int)n + 1) return;
        } else {
            route_writes[n]++;
            enabled_once[n] = 1;
            if (atomic_load(&poison_after_enable) == (int)n + 1)
                rte[reg + 1] ^= 1u << 24;
        }
    }
    rte[reg] = value;
}
uint32_t acpi_gsi_for_irq(int irq)
{
    if (atomic_load(&duplicate_legacy_gsi) && irq == 1) return 0;
    return (uint32_t)irq;
}
uint16_t acpi_gsi_flags(int irq) { (void)irq; return 0; }
#include "ioapic_body.c"

static atomic_uint commands[32];
static atomic_int ignore_command_disable[32], ignore_command_enable[32];
uint16_t pci_cfg_read16(uint8_t b,uint8_t s,uint8_t f,uint16_t off)
{ (void)b; (void)f; return off == PCI_CFG_COMMAND ? (uint16_t)atomic_load(&commands[s]) : 0; }
void pci_cfg_write16(uint8_t b,uint8_t s,uint8_t f,uint16_t off,uint16_t value)
{
    (void)b; (void)f;
    if (off != PCI_CFG_COMMAND) return;
    if ((value & PCI_CMD_INTX_DIS) && atomic_load(&ignore_command_disable[s])) return;
    if (!(value & PCI_CMD_INTX_DIS) && atomic_load(&ignore_command_enable[s])) return;
    atomic_store(&commands[s], value);
    if (value & PCI_CMD_INTX_DIS)
        atomic_store(&disable_seq[s], atomic_fetch_add(&sequence, 1) + 1);
}
uint32_t pci_cfg_read(uint8_t b,uint8_t s,uint8_t f,uint16_t off)
{ return pci_cfg_read16(b,s,f,off); }
void pci_cfg_write(uint8_t b,uint8_t s,uint8_t f,uint16_t off,uint32_t value)
{ pci_cfg_write16(b,s,f,off,(uint16_t)value); }
uint64_t dev_bar_map(struct device *d,int i) { (void)d; (void)i; return 0; }

struct context { struct device dev; atomic_int pending, calls, delivered, hold, entered, allow, retired; };
static struct context *sources[32];
static void init_device(struct context *c,int slot,int gsi)
{
    memset(c, 0, sizeof *c);
    c->dev.bus_type=DEV_BUS_PCI; c->dev.slot=(uint8_t)slot;
    c->dev.irq_pin=1; c->dev.irq_line=(uint8_t)gsi; c->dev.irq_vec=-1;
    snprintf(c->dev.name,sizeof c->dev.name,"intx-%d",slot);
    atomic_store(&commands[slot], PCI_CMD_MASTER | PCI_CMD_MEM | PCI_CMD_INTX_DIS);
    sources[slot]=c;
}
static void handler(void *arg)
{
    struct context *c=arg;
    atomic_fetch_add(&c->calls,1);
    if (!atomic_exchange(&c->pending,0)) return;
    atomic_fetch_add(&c->delivered,1);
    if (atomic_load(&c->hold)) {
        atomic_store(&c->entered,1);
        while (!atomic_load(&c->allow)) sched_yield();
    }
}
static int bind(struct context *c) { return dev_irq_request(&c->dev,handler,c,"host-intx"); }
static void deliver(int gsi)
{
    int pending=0;
    for (int i=0;i<32;i++) if (sources[i] && sources[i]->dev.irq_line==gsi &&
        !(atomic_load(&commands[i]) & PCI_CMD_INTX_DIS) && atomic_load(&sources[i]->pending)) pending=1;
    uint64_t f=io_lock_enter(&ioapic_gate);
    uint32_t low=rte[0x10+2*gsi];
    io_lock_leave(&ioapic_gate,f);
    if (pending && !(low & (1u<<16))) {
        struct registers r={ .vector=low & 255 };
        irq_isr_entry(&r);
    }
}
static void *dispatch_thread(void *arg) { deliver((int)(uintptr_t)arg); return NULL; }
static void *retire_thread(void *arg)
{ struct context *c=arg; dev_irq_release(&c->dev); atomic_store(&c->retired,1); return NULL; }
static void pause_for_retire(void) { struct timespec ts={.tv_nsec=20000000}; nanosleep(&ts,NULL); }
static int wait_flag(atomic_int *flag)
{
    struct timespec begin,now; clock_gettime(CLOCK_MONOTONIC,&begin);
    while (!atomic_load(flag)) {
        sched_yield(); clock_gettime(CLOCK_MONOTONIC,&now);
        if (now.tv_sec-begin.tv_sec>=3) return 0;
    }
    return 1;
}
static atomic_int sentinel_calls;
static void sentinel(void *p) { (void)p; atomic_fetch_add(&sentinel_calls,1); }
static int report(void)
{
    printf("PCI INTx: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}

int main(void)
{
    for (unsigned i=0;i<48;i++) rte[0x10+2*i]=1u<<16;
    dev_irq_prefer(DEV_IRQ_INTX);

#if defined(LOGIT_X2APIC_NEGCTL_SKIP_INTX_SOURCE_READBACK)
    struct context focus_setup_fault,focus_enable_fault,focus_release_fault;
    init_device(&focus_setup_fault,12,18);
    atomic_fetch_and(&commands[12],~PCI_CMD_INTX_DIS);
    atomic_store(&ignore_command_disable[12],1);
    unsigned focus_setup_routes=route_writes[18];
    int setup_result=bind(&focus_setup_fault);
    atomic_store(&ignore_command_disable[12],0);
    int setup_safe=setup_result==-1 && focus_setup_fault.dev.irq_mode==DEV_IRQ_NONE &&
                   route_writes[18]==focus_setup_routes;
    if(setup_result>=0)dev_irq_release(&focus_setup_fault.dev);

    init_device(&focus_enable_fault,13,19);
    atomic_store(&ignore_command_enable[13],1);
    int enable_result=bind(&focus_enable_fault);
    atomic_store(&ignore_command_enable[13],0);
    int enable_safe=enable_result==-1 && focus_enable_fault.dev.irq_mode==DEV_IRQ_NONE &&
                    (atomic_load(&commands[13])&PCI_CMD_INTX_DIS) &&
                    ioapic_is_masked(19)==1;
    if(enable_result>=0)dev_irq_release(&focus_enable_fault.dev);

    init_device(&focus_release_fault,14,20);
    int focus_release_vec=bind(&focus_release_fault);
    atomic_store(&ignore_command_disable[14],1);
    int focus_release_result=dev_irq_release(&focus_release_fault.dev);
    atomic_store(&focus_release_fault.pending,1);
    deliver(20);
    int release_safe=focus_release_result==-1 && focus_release_fault.dev.irq_mode==DEV_IRQ_INTX &&
                     focus_release_fault.dev.irq_vec==focus_release_vec &&
                     atomic_load(&focus_release_fault.delivered)==1;
    atomic_store(&ignore_command_disable[14],0);
    int retry_safe=dev_irq_release(&focus_release_fault.dev)==0 &&
                   focus_release_fault.dev.irq_mode==DEV_IRQ_NONE;
    CHECK(setup_safe && enable_safe && release_safe && retry_safe,
          "PCI INTx command readback guards setup and release ownership");
    return report();
#elif defined(LOGIT_X2APIC_NEGCTL_SKIP_ROUTE_READBACK)
    atomic_store(&reject_route,15);
    CHECK(ioapic_route(14,0x50,3,1,1)==IOAPIC_ROUTE_SAFE_REJECT &&
          ioapic_is_masked(14)==1,
          "dropped IOAPIC route readback blocks source enable");
    return report();
#elif defined(LOGIT_X2APIC_NEGCTL_SKIP_LEGACY_ROLLBACK)
    atomic_store(&reject_route,13);
    CHECK(ioapic_route_legacy_set(3)==IOAPIC_ROUTE_SAFE_REJECT &&
          ioapic_is_masked(0)==1 && ioapic_is_masked(1)==1 && ioapic_is_masked(12)==1,
          "legacy route failure rolls back every earlier unmasked GSI");
    return report();
#elif defined(LOGIT_X2APIC_NEGCTL_ALLOW_DUPLICATE_GSI)
    atomic_store(&duplicate_legacy_gsi,1);
    CHECK(ioapic_route_legacy_set(3)==IOAPIC_ROUTE_SAFE_REJECT &&
          route_writes[0]+route_writes[1]+route_writes[12]==0,
          "duplicate MADT legacy GSIs are rejected before any route is enabled");
    return report();
#elif defined(LOGIT_X2APIC_NEGCTL_FREE_UNSAFE_INTX)
    int focus_doomed=irq_alloc_vector(sentinel,NULL,"unsafe-probe");
    irq_free_vector(focus_doomed);
    struct context focus_unsafe,focus_peer;
    init_device(&focus_unsafe,10,17);
    atomic_store(&poison_after_enable,18);
    atomic_store(&reject_remask,18);
    int focus_result=bind(&focus_unsafe);
    atomic_store(&poison_after_enable,0);
    atomic_store(&reject_remask,0);
    int focus_after=irq_alloc_vector(sentinel,NULL,"after-unsafe");
    init_device(&focus_peer,11,17);
    int focus_peer_result=bind(&focus_peer);
    CHECK(focus_result==-1 && focus_after!=focus_doomed && focus_peer_result==-1 &&
          (atomic_load(&commands[10])&PCI_CMD_INTX_DIS),
          "unsafe first INTx route quarantines its destination vector");
    if(focus_after>=0)irq_free_vector(focus_after);
    if(focus_peer_result>=0)dev_irq_release(&focus_peer.dev);
    return report();
#endif

    /* A device whose Command.INTX_DISABLE write is ignored must never publish
     * a route/member, because firmware may still have a live source. */
    struct context setup_fault;
    init_device(&setup_fault,12,18);
    atomic_fetch_and(&commands[12],~PCI_CMD_INTX_DIS);
    atomic_store(&ignore_command_disable[12],1);
    unsigned setup_routes=route_writes[18];
    CHECK(bind(&setup_fault)==-1 && setup_fault.dev.irq_mode==DEV_IRQ_NONE &&
          route_writes[18]==setup_routes,
          "ignored initial INTx disable leaves no published route or member");
    atomic_store(&ignore_command_disable[12],0);
    sources[12]=NULL;

    /* If clearing INTX_DISABLE is ignored, the source is still safely quiet;
     * remove the just-published member and mask the first-owner route. */
    struct context enable_fault;
    init_device(&enable_fault,13,19);
    atomic_store(&ignore_command_enable[13],1);
    CHECK(bind(&enable_fault)==-1 && enable_fault.dev.irq_mode==DEV_IRQ_NONE &&
          (atomic_load(&commands[13])&PCI_CMD_INTX_DIS) && ioapic_is_masked(19)==1,
          "ignored final INTx enable rolls back the first route safely");
    atomic_store(&ignore_command_enable[13],0);
    sources[13]=NULL;

    /* Release failure preserves the exact callback ownership. Once the config
     * fault clears, the same release can retry and complete. */
    struct context release_fault;
    init_device(&release_fault,14,20);
    int release_vec=bind(&release_fault);
    atomic_store(&ignore_command_disable[14],1);
    int release_result=dev_irq_release(&release_fault.dev);
    atomic_store(&release_fault.pending,1);
    deliver(20);
    CHECK(release_result==-1 && release_fault.dev.irq_mode==DEV_IRQ_INTX &&
          release_fault.dev.irq_vec==release_vec &&
          atomic_load(&release_fault.delivered)==1,
          "ignored release disable preserves the live INTx handler ownership");
    atomic_store(&ignore_command_disable[14],0);
    CHECK(dev_irq_release(&release_fault.dev)==0 &&
          release_fault.dev.irq_mode==DEV_IRQ_NONE,
          "INTx release retries after the config fault clears");
    sources[14]=NULL;
    atomic_store(&eois,0);

    unsigned before_duplicate=route_writes[0]+route_writes[1]+route_writes[12];
    atomic_store(&duplicate_legacy_gsi,1);
    CHECK(ioapic_route_legacy_set(3)==IOAPIC_ROUTE_SAFE_REJECT &&
          route_writes[0]+route_writes[1]+route_writes[12]==before_duplicate,
          "duplicate MADT legacy GSIs are rejected before any route is enabled");
    atomic_store(&duplicate_legacy_gsi,0);

    /* The BSP's three legacy routes are one transaction. If the third route
     * fails, the first two must be masked again before retaining the PIC. */
    atomic_store(&reject_route,13); /* ignore GSI12's destination write */
    int legacy_status=ioapic_route_legacy_set(3);
    CHECK(legacy_status==IOAPIC_ROUTE_SAFE_REJECT && ioapic_is_masked(0)==1 &&
          ioapic_is_masked(1)==1 && ioapic_is_masked(12)==1,
          "legacy route failure rolls back every earlier unmasked GSI");
    atomic_store(&reject_route,0);

    /* The route stays masked while high/low fields are verified. Both a lost
     * masked-low write and a lost destination write are safe rejects. */
    atomic_store(&reject_program_low,16);
    CHECK(ioapic_route(15,0x50,3,1,1)==IOAPIC_ROUTE_SAFE_REJECT &&
          ioapic_is_masked(15)==1,
          "ignored masked-low programming leaves the IOAPIC route disabled");
    atomic_store(&reject_program_low,0);

    /* If a route becomes live and the verification/remask sequence is also
     * ignored, the distinct UNSAFE result forces the kernel to stop before it
     * treats PIC fallback as safe. */
    atomic_store(&poison_after_enable,17);
    atomic_store(&reject_remask,17);
    CHECK(ioapic_route(16,0x51,3,1,1)==IOAPIC_ROUTE_UNSAFE &&
          ioapic_is_masked(16)==0,
          "ignored route rollback is reported unsafe instead of PIC-safe");
    atomic_store(&poison_after_enable,0);
    atomic_store(&reject_remask,0);
    rte[0x10+2*16]=1u<<16;

    struct context a,b;
    init_device(&a,1,11); init_device(&b,2,11);
    int vec=bind(&a);
    CHECK(vec==IRQ_VEC_BASE,"first line allocates a vector");
    CHECK(bind(&b)==vec,"same GSI shares one vector");
    CHECK(route_writes[11]==1,"second owner does not overwrite IOAPIC route");
    CHECK((rte[0x10+22] & ((1u<<15)|(1u<<13)))==((1u<<15)|(1u<<13)),"physical INTx uses level and active low");
    atomic_store(&a.pending,1); atomic_store(&b.pending,1); deliver(11);
    CHECK(atomic_load(&a.delivered)==1 && atomic_load(&b.delivered)==1,"one shared interrupt services both pending devices");
    CHECK(!atomic_load(&a.pending) && !atomic_load(&b.pending),"both shared sources are acknowledged");
    CHECK(atomic_load(&eois)==1,"fanout sends one EOI for the wire");
    dev_irq_release(&a.dev);
    CHECK((atomic_load(&commands[1]) & PCI_CMD_INTX_DIS)!=0,"non-last removal disables its PCI source");
    CHECK(ioapic_is_masked(11)==0,"non-last removal keeps the shared GSI enabled");
    int old_a=atomic_load(&a.calls);
    atomic_store(&b.pending,1); deliver(11);
    CHECK(atomic_load(&b.delivered)==2,"remaining owner receives after peer removal");
    CHECK(atomic_load(&a.calls)==old_a,"removed member is never called again");
    dev_irq_release(&b.dev);
    CHECK(ioapic_is_masked(11)==1,"last owner masks the shared GSI");
    CHECK(atomic_load(&disable_seq[2])<atomic_load(&mask_seq[11]),"last source is disabled before GSI masking");
    CHECK(mask_reads[11]>0,"GSI mask is read back before retirement");
    int reused=irq_alloc_vector(sentinel,NULL,"sentinel");
    CHECK(reused==vec,"last owner returns the drained vector to its pool");
    atomic_fetch_and(&commands[1],~PCI_CMD_INTX_DIS); atomic_store(&a.pending,1);
    deliver(11);
    CHECK(atomic_load(&sentinel_calls)==0,"masked stale route cannot hit a reused vector");
    irq_free_vector(reused); sources[1]=sources[2]=NULL;

    /* The pool may be exhausted and still accept a peer on an existing wire. */
    init_device(&a,3,40); init_device(&b,4,40); vec=bind(&a);
    CHECK(vec>=IRQ_VEC_BASE,"IOAPIC GSIs above the old hard-coded 23 are supported");
    int held[IRQ_NVEC], n=0, v;
    while ((v=irq_alloc_vector(sentinel,NULL,"pool"))>=0) held[n++]=v;
    CHECK(n==IRQ_NVEC-1 && bind(&b)==vec,"shared registration needs no additional vector");
    dev_irq_release(&a.dev); dev_irq_release(&b.dev);
    for (int i=0;i<n;i++) irq_free_vector(held[i]);
    sources[3]=sources[4]=NULL;

    /* A non-last owner cannot free its callback data on another CPU while
     * the old dispatcher still holds that member's reference. */
    init_device(&a,5,10); init_device(&b,6,10); vec=bind(&a); bind(&b);
    atomic_store(&a.hold,1); atomic_store(&a.pending,1);
    pthread_t dispatch,retire; pthread_create(&dispatch,NULL,dispatch_thread,(void *)(uintptr_t)10);
    CHECK(wait_flag(&a.entered),"held callback is running before concurrent removal");
    pthread_create(&retire,NULL,retire_thread,&a); pause_for_retire();
    CHECK(!atomic_load(&a.retired),"non-last removal waits for its active callback");
    CHECK(ioapic_is_masked(10)==0,"waiting non-last removal does not stop peer interrupts");
    atomic_store(&a.allow,1); pthread_join(dispatch,NULL); pthread_join(retire,NULL);
    CHECK(atomic_load(&a.retired),"non-last removal completes after its callback returns");
    atomic_store(&b.pending,1); deliver(10);
    CHECK(atomic_load(&b.delivered)==1,"shared peer remains alive after concurrent retirement");
    dev_irq_release(&b.dev); sources[5]=sources[6]=NULL;

    /* Starting a PCI source is safe only after the destination/vector route
     * reads back. A dropped IOAPIC destination write must unwind the member,
     * vector and line while INTx remains disabled. */
    struct context dropped;
    init_device(&dropped,9,14);
    atomic_store(&reject_route,15);
    int dropped_result=bind(&dropped);
    CHECK(dropped_result==-1 && dropped.dev.irq_mode==DEV_IRQ_NONE &&
          (atomic_load(&commands[9]) & PCI_CMD_INTX_DIS),
          "dropped IOAPIC route readback blocks source enable");
    atomic_store(&reject_route,0);
    if(dropped_result>=0)dev_irq_release(&dropped.dev);
    sources[9]=NULL;

    /* EOI is part of the ISR lifetime even after the callback has returned. */
    init_device(&a,7,9); vec=bind(&a); atomic_store(&a.pending,1);
    atomic_store(&hold_eoi,1); pthread_create(&dispatch,NULL,dispatch_thread,(void *)(uintptr_t)9);
    CHECK(wait_flag(&eoi_entered),"ISR reached the controlled EOI boundary");
    pthread_create(&retire,NULL,retire_thread,&a); pause_for_retire();
    CHECK(ioapic_is_masked(9)==1,"last-owner route is masked while old EOI drains");
    CHECK(!atomic_load(&a.retired),"last-owner retirement waits for the old EOI");
    v=irq_alloc_vector(sentinel,NULL,"while-eoi");
    CHECK(v!=vec,"vector cannot be recycled before old EOI completes");
    atomic_store(&release_eoi,1); pthread_join(dispatch,NULL); pthread_join(retire,NULL);
    atomic_store(&hold_eoi,0); irq_free_vector(v);
    reused=irq_alloc_vector(sentinel,NULL,"after-eoi");
    CHECK(reused==vec,"vector is reusable after the complete ISR drains");
    irq_free_vector(reused); sources[7]=NULL;

    /* Refuse unsafe reuse if an IOAPIC write did not reach the controller. */
    init_device(&a,8,13); vec=bind(&a); atomic_store(&reject_mask,14);
    dev_irq_release(&a.dev);
    CHECK(a.dev.irq_mode==DEV_IRQ_NONE && (atomic_load(&commands[8])&PCI_CMD_INTX_DIS),"failed mask still detaches and suppresses the removed source");
    v=irq_alloc_vector(sentinel,NULL,"after-failed-mask");
    CHECK(v!=vec,"failed mask quarantines its old vector");
    init_device(&b,9,13);
    CHECK(bind(&b)<0,"failed mask quarantines its GSI from new owners");
    irq_free_vector(v); sources[8]=sources[9]=NULL;

    /* The PCI wrapper must preserve the vector if a first route becomes live
     * but cannot be re-masked. Otherwise the stale RTE can target a new owner. */
    int doomed=irq_alloc_vector(sentinel,NULL,"unsafe-probe");
    irq_free_vector(doomed);
    struct context unsafe,unsafe_peer;
    init_device(&unsafe,10,17);
    atomic_store(&poison_after_enable,18);
    atomic_store(&reject_remask,18);
    int unsafe_result=bind(&unsafe);
    atomic_store(&poison_after_enable,0);
    atomic_store(&reject_remask,0);
    int after_unsafe=irq_alloc_vector(sentinel,NULL,"after-unsafe");
    init_device(&unsafe_peer,11,17);
    int unsafe_peer_result=bind(&unsafe_peer);
    CHECK(unsafe_result==-1 && after_unsafe!=doomed && unsafe_peer_result==-1 &&
          (atomic_load(&commands[10])&PCI_CMD_INTX_DIS),
          "unsafe first INTx route quarantines its destination vector");
    if(after_unsafe>=0)irq_free_vector(after_unsafe);
    if(unsafe_peer_result>=0)dev_irq_release(&unsafe_peer.dev);
    sources[10]=sources[11]=NULL;

    CHECK(ioapic_mask(48)==-1 && ioapic_is_masked(48)==-1,"IOAPIC masks reject out-of-range GSIs");
    CHECK(ioapic_gsi_valid(47) && !ioapic_gsi_valid(48),"GSI range comes from discovered hardware capacity");
    return report();
}
