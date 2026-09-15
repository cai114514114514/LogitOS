/* SPDX-License-Identifier: MIT */
#define main unix_sequential_main
#include "unix_test.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
struct duplex_arg { struct usock *s; unsigned side; atomic_int *errors; };
static void *duplex_worker(void *v)
{
    struct duplex_arg *a=v;
    unsigned sent=0,received=0;unsigned char out[128],in[128];
    for(unsigned spins=0;spins<2000000 && (received<4000 || sent<4000);spins++) {
        if(sent<4000) {
            memset(out,(int)(sent&255),sizeof out);out[0]=(unsigned char)a->side;
            long n=unix_write(a->s,out,sizeof out,1);
            if(n==sizeof out)sent++;else if(n!=EAGAIN_RC){atomic_fetch_add(a->errors,1);break;}
        }
        long n=unix_read(a->s,in,sizeof in,1);
        if(n==sizeof in) {
            int valid=in[0]==(unsigned char)(1-a->side);
            for(unsigned k=1;k<sizeof in;k++)valid &= in[k]==(unsigned char)(received&255);
            if(!valid)atomic_fetch_add(a->errors,1);
            received++;
        }else if(n!=EAGAIN_RC){atomic_fetch_add(a->errors,1);break;}
        if((spins&31)==0)sched_yield();
    }
    if(sent!=4000||received!=4000)atomic_fetch_add(a->errors,1);
    return 0;
}
int main(void)
{
    if(unix_sequential_main())return 1;
    reset_all();
    struct usock *a[4],*b[4];int err;
    pthread_t t[8];struct duplex_arg args[8];atomic_int errors=0;
    for(int i=0;i<4;i++) {
        CHECK_EQ("parallel pair claim",unix_pair(LOGIT_SOCK_SEQPACKET,100+i,&a[i],&b[i],&err),0);
        args[2*i]=(struct duplex_arg){a[i],0,&errors};args[2*i+1]=(struct duplex_arg){b[i],1,&errors};
    }
    for(int i=0;i<8;i++)pthread_create(&t[i],0,duplex_worker,&args[i]);
    for(int i=0;i<8;i++)pthread_join(t[i],0);
    CHECK_EQ("32000 duplex records preserve sequence and payload",atomic_load(&errors),0);
    for(int i=0;i<4;i++){unix_release(a[i]);unix_release(b[i]);}
    CHECK_EQ("parallel socket backing returns to baseline",ustub_live_allocs,0);
    printf("BKL Unix: %d checks, %d failures\n",checks,fails);
    return fails!=0;
}
