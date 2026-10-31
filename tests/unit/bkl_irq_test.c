/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <sched.h>
#include <time.h>
#define IRQ_NVEC 32
#define IRQ_VEC_BASE 0x60
#define IRQ_STUB_STRIDE 16
typedef void (*irq_handler_t)(void *);
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static uint64_t spin_lock_irqsave(spinlock_t *l) { pthread_mutex_lock(l);return 0; }
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) { (void)f;pthread_mutex_unlock(l); }
static void io_relax(void) { sched_yield(); }
static unsigned char irq_stub_base[IRQ_NVEC*IRQ_STUB_STRIDE];
static unsigned char *irq_stub_end=irq_stub_base+sizeof irq_stub_base;
static void install_gate(int v,const void *s) { (void)v;(void)s; }
static void kprintf(const char *fmt,...) { (void)fmt; }
struct cpu {
    int in_kernel;
    int index;
};
static _Thread_local struct cpu cpu;
static struct cpu *this_cpu(void) { return &cpu; }
struct registers { uint64_t vector; };
static atomic_int eois;
static int lapic_ready(void) { return 1; }
static void (*eoi_hook)(void);
static int online_cpus = 1;
static atomic_uint cleanup_kicks;
static atomic_uint pending_vectors[2];
static _Thread_local uint32_t in_service;
static int service_bsp_during_eoi;
static void service_cleanup_kick(int index);

/* IRR belongs to the modeled CPU, not the thread retiring another CPU's IRQ.
 * ISR remains set until the EOI leaf below acknowledges the old delivery. */
static int lapic_vector_pending(uint8_t vector)
{
    uint32_t bit = 1u << (vector - IRQ_VEC_BASE);
    return !!((atomic_load(&pending_vectors[cpu.index]) | in_service) & bit);
}

/* Mirror the production pre-EOI hook rather than bypass vector quarantine. */
static void lapic_set_eoi_hook(void (*fn)(void))
{
    __atomic_store_n(&eoi_hook, fn, __ATOMIC_RELEASE);
}

/* The original callback lifetime scenario uses CPU zero; the PIC fallback
 * scenario publishes two CPUs to expose a missing remote cleanup trigger. */
static int smp_cpu_count(void)
{
    return online_cpus;
}

/* Mirror smp.c smp_kick_others(): queue vector 243 on remote online CPUs.
 * Sending alone must not certify retirement; each recipient executes EOI. */
static void smp_kick_others(void)
{
    for (int i = 0; i < online_cpus; i++) {
        if (i != cpu.index) {
            atomic_fetch_or(&cleanup_kicks, 1u << i);
        }
    }
}

/* Execute cleanup while the dispatch pin still covers EOI, as LAPIC does. */
static void lapic_eoi(void)
{
    void (*cleanup)(void) = __atomic_load_n(&eoi_hook, __ATOMIC_ACQUIRE);
    if (cleanup) {
        cleanup();
    }
    if (service_bsp_during_eoi) {
        /* The remote kick can run while this CPU's old delivery is pinned.
         * Its rejected cleanup must get another explicit trigger later. */
        service_bsp_during_eoi = 0;
        int saved_index = cpu.index;
        uint32_t saved_isr = in_service;
        in_service = 0;
        service_cleanup_kick(0);
        cpu.index = saved_index;
        in_service = saved_isr;
    }
    in_service = 0;
    atomic_fetch_add(&eois, 1);
}
static void pic_eoi(int v) { (void)v; }
#include "irq_body.c"
static atomic_int entered,release_callback,free_done,callback_ok,depth_ok;
static int vector;
static void callback(void *arg) {
    atomic_store(&depth_ok,cpu.in_kernel==3);
    atomic_store(&entered,1);
    while(!atomic_load(&release_callback))sched_yield();
    atomic_store(&callback_ok,*(int *)arg==0x3571);
}
static void *dispatch(void *arg) {
    (void)arg;cpu.in_kernel=2;struct registers r={.vector=(uint64_t)vector};irq_isr_entry(&r);
    if(cpu.in_kernel!=2)atomic_store(&depth_ok,0);
    return 0;
}
static void *retire(void *arg) { (void)arg;irq_free_vector(vector);atomic_fetch_add(&free_done,1);return 0; }
static int checks,failed;
#define CHECK(c,msg) do{checks++;if(!(c)){failed++;printf("FAIL: %s\n",msg);}}while(0)

/* A quiet PIC-mode BSP receives no periodic LAPIC EOI. Execute only a kick
 * actually queued by production retirement, following interrupts.c vector
 * 243's LAPIC EOI path; a PIT/PIC EOI deliberately does not run that hook. */
static void service_cleanup_kick(int index)
{
    cpu.index = index;
    uint32_t bit = 1u << index;
    if (atomic_fetch_and(&cleanup_kicks, ~bit) & bit) {
        lapic_eoi();
    }
}

/* Count callbacks independently of vector delivery: a quarantined old IRQ
 * may be counted/acknowledged but must not reach a replacement callback. */
static void count_callback(void *arg)
{
    int *calls = arg;
    (*calls)++;
}

/* Reproduce AP release with BSP PIT/PIC ticks only. More than the 32-vector
 * pool's worth of cycles proves that remote quarantine cannot leak slots.
 * Host model evidence: not verified on X79-VG4 hardware. */
static void check_pic_retirement(void)
{
    int calls = 0;
    online_cpus = 2;
    cpu.index = 1;
    int released = irq_alloc_vector(count_callback, &calls, "PIC-BSP-target");
    irq_free_vector(released);
    CHECK(atomic_load(&cleanup_kicks) & 1u,
          "AP retirement actively kicks the PIC fallback BSP");
    cpu.index = 0;
    pic_eoi(0);
    CHECK(g_slot[released - IRQ_VEC_BASE].retiring,
          "PIC EOI alone cannot certify remote LAPIC retirement");
    service_cleanup_kick(0);
    cpu.index = 1;
    int reused = irq_alloc_vector(count_callback, &calls, "PIC-reused");
    CHECK(reused == released,
          "PIC fallback BSP cleanup permits cross-CPU vector reuse");
    irq_free_vector(reused);
    service_cleanup_kick(0);

    cpu.index = 1;
    int pending = irq_alloc_vector(count_callback, &calls, "PIC-pending");
    if (pending >= 0) {
        uint32_t bit = 1u << (pending - IRQ_VEC_BASE);
        atomic_store(&pending_vectors[0], bit);
        irq_free_vector(pending);
        service_cleanup_kick(0);
        CHECK(g_slot[pending - IRQ_VEC_BASE].retiring,
              "cleanup kick preserves a queued BSP interrupt");
        atomic_store(&pending_vectors[0], 0);
        in_service = bit;
        struct registers registers = {.vector = (uint64_t)pending};
        irq_isr_entry(&registers);
        CHECK(calls == 0 && irq_vector_count(pending) == 1,
              "late BSP delivery never calls a replacement device");
        CHECK(!g_slot[pending - IRQ_VEC_BASE].retiring,
              "late BSP delivery finishes quarantine without another LAPIC tick");
    } else {
        CHECK(0, "pending-delivery specimen obtains a vector");
    }

    cpu.index = 1;
    int racing = irq_alloc_vector(count_callback, &calls, "PIC-racing-delivery");
    if (racing >= 0) {
        uint32_t bit = 1u << (racing - IRQ_VEC_BASE);
        atomic_store(&pending_vectors[1], bit);
        irq_free_vector(racing);
        /* Consume the actual retirement kick while the AP still owns its
         * old dispatch pin. No timer or synthetic cleanup bit is supplied. */
        atomic_store(&pending_vectors[1], 0);
        in_service = bit;
        service_bsp_during_eoi = 1;
        struct registers registers = {.vector = (uint64_t)racing};
        irq_isr_entry(&registers);
        CHECK(atomic_load(&cleanup_kicks) & 1u,
              "old IRQ completion re-kicks CPUs whose cleanup saw an active pin");
        service_cleanup_kick(0);
        CHECK(!g_slot[racing - IRQ_VEC_BASE].retiring,
              "overlapping cleanup and old dispatch converge without timer ticks");
    } else {
        CHECK(0, "overlapping-cleanup specimen obtains a vector");
    }
    int completed = 0;
    for (int i = 0; i < IRQ_NVEC * 2; i++) {
        cpu.index = 1;
        int next = irq_alloc_vector(count_callback, &calls, "PIC-cycle");
        if (next < 0) {
            break;
        }
        irq_free_vector(next);
        service_cleanup_kick(0);
        completed++;
    }
    CHECK(completed == IRQ_NVEC * 2,
          "PIC fallback cross-CPU release cannot exhaust the vector pool");

    online_cpus = 1;
    cpu.index = 0;
}
int main(void) {
    int magic=0x3571;vector=irq_alloc_vector(callback,&magic,"held");
    CHECK(vector==IRQ_VEC_BASE,"first vector is allocated");
    pthread_t d,a,b;pthread_create(&d,0,dispatch,0);
    while(!atomic_load(&entered))sched_yield();
    pthread_create(&a,0,retire,0);pthread_create(&b,0,retire,0);
    struct timespec pause={.tv_nsec=20000000};nanosleep(&pause,0);
    CHECK(atomic_load(&free_done)==0,"retirement waits for an active callback");
    int other=irq_alloc_vector(callback,&magic,"new");
    CHECK(other!=vector,"active callback vector is not recycled");
    atomic_store(&release_callback,1);
    pthread_join(d,0);pthread_join(a,0);pthread_join(b,0);
    CHECK(atomic_load(&free_done)==2,"both retirement callers complete after drain");
    CHECK(atomic_load(&callback_ok),"callback argument survives until return");
    CHECK(atomic_load(&depth_ok),"IRQ entry preserves independent CPU nesting depth");
    CHECK(atomic_load(&eois)==1 && irq_vector_count(vector)==1,"one delivered IRQ has one EOI and count");
    int again=irq_alloc_vector(callback,&magic,"reused");
    CHECK(again==vector && g_slot[again-IRQ_VEC_BASE].arg==&magic,"drained vector can be reused with its new argument");
    irq_free_vector(again);if(other!=again)irq_free_vector(other);
    check_pic_retirement();
    printf("BKL IRQ: %d checks, %d failures\n",checks,failed);return failed!=0;
}
