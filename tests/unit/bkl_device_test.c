/* SPDX-License-Identifier: MIT */
/* Device.c itself; sleeping probe/remove and publish boundaries are driven
 * by pthreads. Registry exclusion must end before any driver callback. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "driver.h"
#include "pci.h"
static int checks,failures;
static unsigned add_arrivals,probe_started,release_probe,probe_calls[2],probe_active[2],removes,early_remove,irq_drained;
#define CHECK(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s\n",m);}}while(0)
void bkl_add_pause(void){
    __atomic_add_fetch(&add_arrivals,1,__ATOMIC_SEQ_CST);
    while(__atomic_load_n(&add_arrivals,__ATOMIC_SEQ_CST)<2)sched_yield();
}
static struct driver extra={.name="callback-registration"};
static int probe(struct device *d){
    unsigned k=d->device-1;
    unsigned n=__atomic_add_fetch(&probe_calls[k],1,__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&probe_active[k],1,__ATOMIC_SEQ_CST);
    driver_register(&extra); /* detects accidentally holding registry lock */
    if(k==0&&n==1){
        __atomic_store_n(&probe_started,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&release_probe,__ATOMIC_ACQUIRE))sched_yield();
    }
    __atomic_sub_fetch(&probe_active[k],1,__ATOMIC_SEQ_CST);return 0;
}
static void remove_dev(struct device *d){
    if(__atomic_load_n(&probe_active[d->device-1],__ATOMIC_SEQ_CST)||!__atomic_load_n(&irq_drained,__ATOMIC_ACQUIRE))
        __atomic_add_fetch(&early_remove,1,__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&removes,1,__ATOMIC_SEQ_CST);
}
static const struct dev_match matches[]={DEV_MATCH_CLASS(1,6),DEV_MATCH_END};
static struct driver test_driver={.name="test",.bus_type=DEV_BUS_PCI,.match=matches,.probe=probe,.remove=remove_dev};
static void *add(void *p){
    struct device d={0};d.bus_type=DEV_BUS_PCI;d.vendor=0x1234;d.device=(uint16_t)(uintptr_t)p;d.class_code=1;d.subclass=6;
    return dev_add(&d);
}
static void *probe_all(void *p){(void)p;return (void *)(intptr_t)dev_probe_all();}
static void *unbind(void *p){dev_unbind(p);return 0;}
int main(int argc,char **argv){
    setbuf(stdout,NULL);pthread_t a,b;void *ra,*rb;
    if(argc>1&&!strcmp(argv[1],"add")){
        pthread_create(&a,0,add,(void *)1);pthread_create(&b,0,add,(void *)2);
        pthread_join(a,&ra);pthread_join(b,&rb);
        CHECK(ra&&rb&&ra!=rb&&dev_count()==2,"device publication reserves distinct complete slots");
        CHECK(dev_at(0)&&dev_at(1)&&dev_at(0)->device!=dev_at(1)->device,"published identities remain independent");
    }else{
        struct device *one=add((void *)1);add((void *)2);driver_register(&test_driver);
        pthread_create(&a,0,probe_all,0);
        while(!__atomic_load_n(&probe_started,__ATOMIC_ACQUIRE))sched_yield();
        pthread_create(&b,0,probe_all,0);pthread_join(b,&rb);
        CHECK(probe_calls[0]==1&&probe_calls[1]==1,"same-device probe has one owner while other devices progress");
        pthread_create(&b,0,unbind,one);
        while(!__atomic_load_n(&one->unbinding,__ATOMIC_ACQUIRE)&&!__atomic_load_n(&removes,__ATOMIC_ACQUIRE))sched_yield();
        CHECK(removes==0,"teardown waits until probe releases its device");
        __atomic_store_n(&release_probe,1,__ATOMIC_RELEASE);
        pthread_join(a,&ra);pthread_join(b,0);
        CHECK(removes==1&&early_remove==0&&one->drv==NULL,"remove follows probe and IRQ drain before clearing binding");
        CHECK(dev_at(1)->drv==&test_driver,"unbinding one device preserves its neighbour");
    }
    printf("BKL_DEVICE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
uint32_t pci_cfg_read(uint8_t b, uint8_t s, uint8_t f, uint16_t o)
{ (void)b; (void)s; (void)f; (void)o; return 0; }
void pci_cfg_write(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint32_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; }
uint16_t pci_cfg_read16(uint8_t b, uint8_t s, uint8_t f, uint16_t o)
{ (void)b; (void)s; (void)f; (void)o; return 0; }
void pci_cfg_write16(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint16_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; }
const char *pci_class_name(uint8_t c, uint8_t s) { (void)c; (void)s; return "test"; }
int dev_irq_release(struct device *d) { (void)d; __atomic_store_n(&irq_drained,1,__ATOMIC_RELEASE); return 0; }
