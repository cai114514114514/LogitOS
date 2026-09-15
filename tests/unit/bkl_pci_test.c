/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include "pci.h"
#include "driver.h"
#include "ioapic.h"
static _Atomic uint32_t cf8,ports[32][64],sel,regs[256];
static atomic_int bad,maps,checked_map,drop_init_mask;
static unsigned char *space;
static int mode,workers;
int test_ecam_ready(unsigned bus);
struct barrier { pthread_mutex_t m; pthread_cond_t c; unsigned count,gen; };
static struct barrier syncpoint={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0};
static void sync_all(void) {
    pthread_mutex_lock(&syncpoint.m);unsigned g=syncpoint.gen;
    if(++syncpoint.count==(unsigned)workers){syncpoint.count=0;syncpoint.gen++;pthread_cond_broadcast(&syncpoint.c);}
    else while(syncpoint.gen==g)pthread_cond_wait(&syncpoint.c,&syncpoint.m);
    pthread_mutex_unlock(&syncpoint.m);
}
void outl(uint16_t port,uint32_t value) {
    if(port==0xcf8){atomic_store(&cf8,value);sched_yield();return;}
    if(port==0xcfc){uint32_t a=atomic_load(&cf8);atomic_store(&ports[(a>>11)&31][(a&252)/4],value);sched_yield();}
}
uint32_t inl(uint16_t port) {
    if(port!=0xcfc)return ~0u;
    uint32_t a=atomic_load(&cf8);sched_yield();return atomic_load(&ports[(a>>11)&31][(a&252)/4]);
}
static void port_narrow(uint16_t p,uint32_t v,unsigned width){
    if(p<0xcfc||p+width>0xd00)return;
    uint32_t a=atomic_load(&cf8),sh=(p-0xcfc)*8,mask=(width==1?255u:65535u)<<sh;
    _Atomic uint32_t *reg=&ports[(a>>11)&31][(a&252)/4];
    uint32_t old=atomic_load(reg);
    while(!atomic_compare_exchange_weak(reg,&old,(old&~mask)|((v<<sh)&mask))){}
    sched_yield();
}
void outb(uint16_t p,uint8_t v){port_narrow(p,v,1);}
uint8_t inb(uint16_t p){(void)p;return 255;}
void outw(uint16_t p,uint16_t v){port_narrow(p,v,2);}
uint16_t inw(uint16_t p){(void)p;return 65535;}
void test_ioapic_select(uint32_t reg){atomic_store(&sel,reg);sched_yield();}
void test_ioapic_store(uint32_t value){
    unsigned r=atomic_load(&sel)&255;
    if(atomic_load(&drop_init_mask) && r==0x10 && (value&(1u<<16)))return;
    atomic_store(&regs[r],value);sched_yield();
}
uint32_t test_ioapic_load(void){return atomic_load(&regs[atomic_load(&sel)&255]);}
uint32_t acpi_ioapic_addr(void){return 0xfec00000u;}
uint32_t acpi_ioapic_gsibase(void){return 0;}
uint32_t acpi_gsi_for_irq(int i){return (uint32_t)i;}
uint16_t acpi_gsi_flags(int i){(void)i;return 0;}
void vmm_map_page(uint64_t v,uint64_t p,uint64_t f){(void)v;(void)p;(void)f;}
void kprintf(const char *fmt,...){(void)fmt;}
void vmm_map_range(uint64_t v,uint64_t p,uint64_t n,uint64_t f){
    (void)p;(void)n;(void)f;atomic_fetch_add(&maps,1);
    /* Observe actual publication at the mapping boundary. A sleep cannot
     * guarantee another reader runs during an incorrectly published map. */
    if(test_ecam_ready(1))atomic_fetch_add(&bad,1);
    /* An unrelated legacy transaction must work while mapping is in progress:
     * the production config spinlock may never cover allocation/sleep. */
    pci_cfg_read(250,0,0,0);atomic_fetch_add(&checked_map,1);
    struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,0);
    for(unsigned i=0;i<8;i++)*(uint32_t *)(uintptr_t)(v+((uint64_t)i<<15))=0xabc00000u+i;
}
struct device *dev_add(const struct device *d){(void)d;return NULL;}
int dev_count(void){return 0;}
struct device *dev_find_id(uint16_t v,uint16_t d,struct device *from){(void)v;(void)d;(void)from;return NULL;}
static void *worker(void *arg){
    unsigned id=(unsigned)(uintptr_t)arg;
    sync_all();
    if(mode==0)for(unsigned i=1;i<=1200;i++){
        uint32_t value=(id<<24)|i;pci_cfg_write(0,(uint8_t)id,0,0x40,value);
        if(pci_cfg_read(0,(uint8_t)id,0,0x40)!=value)atomic_fetch_add(&bad,1);
    }
    else if(mode==1||mode==2)for(unsigned i=1;i<=500;i++){
        if(id==0)pci_cfg_write(0,0,0,0x40,0);sync_all();
        if(mode==1)pci_cfg_write8(0,0,0,(uint16_t)(0x40+id),(uint8_t)i);
        else pci_cfg_write16(0,0,0,(uint16_t)(0x40+2*id),(uint16_t)i);
        sync_all();
        if(id==0){uint32_t expected=mode==1?0x01010101u*(i&255):0x00010001u*i;
            if(pci_cfg_read(0,0,0,0x40)!=expected)atomic_fetch_add(&bad,1);}
        sync_all();
    }
    else if(mode==3){if(pci_cfg_read(1,(uint8_t)id,0,0)!=0xabc00000u+id)atomic_fetch_add(&bad,1);}
    else if(mode==4)for(unsigned i=0;i<1000;i++){
        if(ioapic_route(id,(uint8_t)(0x60+id),id+1,1,1))atomic_fetch_add(&bad,1);
        if(atomic_load(&regs[0x10+2*id])!=(0xa060u+id) || atomic_load(&regs[0x11+2*id])!=((id+1)<<24))atomic_fetch_add(&bad,1);
    }
    return NULL;
}
int main(int argc,char **argv){
    if(argc!=2)return 2;mode=atoi(argv[1]);workers=mode==1?4:mode==2?2:(mode>=5?1:8);
    if(mode==3){space=calloc(1,2u<<20);if(!space)return 2;
        if(!pci_ecam_set((uint64_t)(uintptr_t)space,0,1,1))return 2;}
    if(mode==4){atomic_store(&regs[1],23u<<16);ioapic_init();}
    if(mode==5){
        atomic_store(&regs[1],0);atomic_store(&regs[0x10],0);atomic_store(&regs[0x11],0xa5000000u);
        atomic_store(&drop_init_mask,1);int s=ioapic_init();
        if(s!=IOAPIC_ROUTE_UNSAFE||ioapic_present()||atomic_load(&regs[0x11])!=0xa5000000u)atomic_fetch_add(&bad,1);
    }
    if(mode==6){
        atomic_store(&regs[1],255u<<16);atomic_store(&regs[0x10],0x4321u);atomic_store(&regs[0x11],0xa5000000u);
        int s=ioapic_init();
        if(s!=IOAPIC_ROUTE_SAFE_REJECT||ioapic_present()||atomic_load(&regs[0x10])!=0x4321u||atomic_load(&regs[0x11])!=0xa5000000u)atomic_fetch_add(&bad,1);
    }
    pthread_t threads[8];for(int i=0;i<workers;i++)pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i);
    for(int i=0;i<workers;i++)pthread_join(threads[i],NULL);
    const char *labels[]={"CF8/CFC address-data pairs stay together","8-bit writes preserve concurrent byte lanes","16-bit writes preserve concurrent halfwords","ECAM pointers are published after mapping","IOAPIC selector-window and RTE updates stay together","IOAPIC init rejects an unconfirmed initial mask before clearing destination","malformed IOAPIC VER is rejected before any RTE write"};
    int failed=atomic_load(&bad)!=0;if(failed)printf("FAIL: %s (%d)\n",labels[mode],atomic_load(&bad));
    if(mode==3){
        if(atomic_load(&maps)!=1){printf("FAIL: one mapper publishes each ECAM bus (%d)\n",atomic_load(&maps));failed=1;}
        if(atomic_load(&checked_map)!=1){printf("FAIL: mapping occurs outside config spinlock\n");failed=1;}
        if(!pci_ecam_set((uint64_t)(uintptr_t)space,0,1,1)||pci_ecam_set((uint64_t)(uintptr_t)space+4096,0,1,1)){printf("FAIL: ECAM setter preserves immutable aperture\n");failed=1;}
    }
    free(space);printf("BKL PCI mode %d: %s\n",mode,failed?"FAIL":"PASS");return failed;
}
