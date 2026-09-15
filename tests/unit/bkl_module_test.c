/* SPDX-License-Identifier: MIT */
/* The real loader with deterministic sleeping-VFS/probe boundaries. ELF
 * arithmetic has its existing modreloc gate; this checks publication/lifetime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include "module.h"
#include "driver.h"
#include "vfs_cred.h"
#include "logit_abi.h"
static int checks,failures;
static unsigned pause_probe,probe_entered,release_probe,size_arrivals,registrations;
static struct driver driver;
static struct driver *drivers[]={&driver};
static struct device device;
#define CHECK(c,m) do {checks++;if(!(c)){failures++;printf("FAIL: %s\n",m);}}while(0)
void *kmalloc(size_t n){return malloc(n);}
void *kmalloc_low(size_t n){return malloc(n);}
void kfree(void *p){free(p);}
void kprintf(const char *fmt,...){(void)fmt;}
void *ksym_lookup(const char *s){(void)s;return NULL;}
int ksym_count(void){return 0;}
int vfs_size(const char *p){
#ifdef MOD_DUP_RACE
    if(strcmp(p,"bad")){
        __atomic_add_fetch(&size_arrivals,1,__ATOMIC_SEQ_CST);
        while(__atomic_load_n(&size_arrivals,__ATOMIC_SEQ_CST)<2)sched_yield();
    }
#endif
    return 16;
}
int vfs_read(const char *p,void *b,int n){memset(b,0,n);*(char *)b=!strcmp(p,"bad")?'!':'x';return n;}
long mod_elf_size(const void *p,uint32_t n){(void)n;return *(const char *)p=='!'?MOD_E_FORMAT:64;}
int mod_elf_load(const void *img,uint32_t ni,void *dst,uint32_t nd,mod_resolve_fn fn,void *ctx,struct mod_layout *o,const char **undef){
    (void)img;(void)ni;(void)fn;(void)ctx;(void)undef;
    *o=(struct mod_layout){.dst=dst,.dstlen=nd,.text_off=4,.text_size=8,.drv_start=drivers,.drv_stop=drivers+1};return 0;
}
void driver_register(struct driver *d){(void)d;__atomic_add_fetch(&registrations,1,__ATOMIC_RELAXED);}
int dev_probe_all(void){
    if(__atomic_load_n(&pause_probe,__ATOMIC_ACQUIRE)){
        __atomic_store_n(&probe_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&release_probe,__ATOMIC_ACQUIRE))sched_yield();
    }
    __atomic_store_n(&device.drv,&driver,__ATOMIC_RELEASE);return 1;
}
int dev_count(void){return 1;}
struct device *dev_at(int i){return i==0?&device:NULL;}
void vfs_cred_current(struct vcred *c){memset(c,0,sizeof *c);}
int user_copy_string(char *d,uint64_t cap,const char *s){size_t n=strlen(s);if(n>=cap)return -1;memcpy(d,s,n+1);return (int)n;}
int user_copy_to(void *d,const void *s,uint64_t n){memcpy(d,s,(size_t)n);return 0;}
static void *load(void *p){return (void *)(intptr_t)mod_load(p);}
int main(int argc,char **argv){
    setbuf(stdout,NULL);
    CHECK(mod_load("bad")==MOD_E_FORMAT,"failed image refused");
    CHECK(mod_count()==0&&mod_at(0)==NULL,"failed image never published");
    if(argc>1&&!strcmp(argv[1],"duplicates")){
        pthread_t a,b;pthread_create(&a,0,load,"same");pthread_create(&b,0,load,"same");
        void *ra,*rb;pthread_join(a,&ra);pthread_join(b,&rb);
        CHECK(((intptr_t)ra>0&&(intptr_t)rb==MOD_E_DUP)||((intptr_t)rb>0&&(intptr_t)ra==MOD_E_DUP),"duplicate concurrent load is rejected");
        CHECK(mod_count()==1&&registrations==1,"driver registered exactly once");
    }else{
        __atomic_store_n(&pause_probe,1,__ATOMIC_RELEASE);
        pthread_t a;pthread_create(&a,0,load,"first");
        while(!__atomic_load_n(&probe_entered,__ATOMIC_ACQUIRE))sched_yield();
        struct logit_modinfo info;
        CHECK(mod_count()==0&&mod_at(0)==NULL&&mod_syscall(SYS_MODULE_LIST,(long)&info,1,0)==0,"query hides the module until probe completes");
        __atomic_store_n(&release_probe,1,__ATOMIC_RELEASE);
        void *ra;pthread_join(a,&ra);
        const struct kmodule *m=mod_at(0);
        CHECK((intptr_t)ra>0&&mod_count()==1&&m&&m->size==64&&m->ndrivers==1&&m->nbound==1,"published module is complete");
        CHECK(mod_unload(m->id)==MOD_E_NOUNLOAD&&mod_at(0)==m&&m->base!=NULL,"refused unload preserves borrowed lifetime");
    }
    printf("BKL_MODULE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
