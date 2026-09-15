/* SPDX-License-Identifier: MIT */
/* The real power control flow; hardware ports/IDT and VFS admission are leaves.
 * Actual drain semantics are covered by the VFS mount concurrency gate. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "power.h"
#include "vfs.h"
#include "acpi.h"
static unsigned begins,ends,active,overlap,syncs,writes,unsafe_write;static int mode;
int vfs_drain_begin(struct vfs_drain *t){
    __atomic_add_fetch(&begins,1,__ATOMIC_SEQ_CST);if(mode==2)return -16;
    t->active=1;if(__atomic_fetch_add(&active,1,__ATOMIC_SEQ_CST))__atomic_add_fetch(&overlap,1,__ATOMIC_SEQ_CST);return 0;
}
void vfs_drain_end(struct vfs_drain *t){if(t->active){t->active=0;__atomic_sub_fetch(&active,1,__ATOMIC_SEQ_CST);__atomic_add_fetch(&ends,1,__ATOMIC_SEQ_CST);}}
int logitfs_sync(void){
    __atomic_add_fetch(&syncs,1,__ATOMIC_SEQ_CST);
#ifdef POWER_CTL_RACE
    while(__atomic_load_n(&syncs,__ATOMIC_SEQ_CST)<2)sched_yield();
#endif
    return mode==0?-1:0;
}
void kprintf(const char *fmt,...){(void)fmt;}
uint64_t time_mono_ns(void){static uint64_t t;return __atomic_add_fetch(&t,1000000,__ATOMIC_RELAXED);}
int acpi_pm1_cnt(uint32_t *a,uint32_t *b){*a=0x604;*b=0;return 0;}
int acpi_reset_reg(struct acpi_gas *r,uint8_t *v){(void)r;(void)v;return -1;}
static void write_port(void){
    if(__atomic_load_n(&active,__ATOMIC_SEQ_CST)!=1||!__atomic_load_n(&syncs,__ATOMIC_SEQ_CST))__atomic_add_fetch(&unsafe_write,1,__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&writes,1,__ATOMIC_SEQ_CST);
}
void outb(uint16_t p,uint8_t v){(void)p;(void)v;write_port();}
void outw(uint16_t p,uint16_t v){(void)p;(void)v;write_port();}
static void *power(void *arg){return (void *)(intptr_t)((intptr_t)arg?kernel_reboot():kernel_poweroff());}
int main(int argc,char **argv){
    mode=argc>1&&!strcmp(argv[1],"hardware")?1:argc>1&&!strcmp(argv[1],"begin")?2:0;
    pthread_t a,b;pthread_create(&a,0,power,0);pthread_create(&b,0,power,0);void *ra,*rb;pthread_join(a,&ra);pthread_join(b,&rb);
    int bad=0;
    if(overlap){puts("FAIL: power requests have one transition owner");bad++;}
    if(active||(mode!=2&&ends!=2)||((intptr_t)ra>=0)||((intptr_t)rb>=0)){puts("FAIL: failed power action restores VFS admission");bad++;}
    if(unsafe_write||(mode!=1&&writes)){puts("FAIL: hardware operation follows drain and successful sync");bad++;}
    if(mode==1){if(kernel_reboot()>=0||active||ends!=3){puts("FAIL: returned reset attempt restores admission");bad++;}}
    printf("BKL_POWER begins=%u ends=%u active=%u failures=%d\n",begins,ends,active,bad);return bad?1:0;
}
