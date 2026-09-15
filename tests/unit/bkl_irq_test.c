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
struct cpu { int in_kernel; };
static _Thread_local struct cpu cpu;
static struct cpu *this_cpu(void) { return &cpu; }
struct registers { uint64_t vector; };
static atomic_int eois;
static int lapic_ready(void) { return 1; }
static void lapic_eoi(void) { atomic_fetch_add(&eois,1); }
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
    printf("BKL IRQ: %d checks, %d failures\n",checks,failed);return failed!=0;
}
