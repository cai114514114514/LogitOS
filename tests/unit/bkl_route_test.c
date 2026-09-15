/* SPDX-License-Identifier: MIT */
#define main route_sequential_main
#include "route_test.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
static atomic_int bad;
static void *mutate(void *arg) {
    unsigned base=(unsigned)(uintptr_t)arg;
    for (unsigned i=1;i<=20000;i++) {
        unsigned value=base*20000+i;
        struct route_entry r={.oif=1,.src=value,.flags=value,.metric=value};
        route_add(r);
        struct route_res out;
        if (route_lookup(0x01020304,&out)!=RT_OK || out.src!=out.flags || out.src!=out.metric) {
            atomic_fetch_add(&bad,1);break;
        }
    }
    return 0;
}
int main(void) {
    if(route_sequential_main())return 1;
    route_flush();
    pthread_t t[8];
    for(unsigned i=0;i<8;i++)pthread_create(&t[i],0,mutate,(void *)(uintptr_t)i);
    for(unsigned i=0;i<8;i++)pthread_join(t[i],0);
    ok(atomic_load(&bad)==0,"eight CPUs read complete route publications");
    printf("BKL route: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
