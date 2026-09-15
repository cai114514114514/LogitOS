/* SPDX-License-Identifier: MIT
 * Real futex comparison/enqueue/wake; only pinning and scheduler are fixtures.
 * Pinning simulates a sibling remapping the VA to a different word, leaving
 * the physical alias valid. Sleeping asserts no pin is held across the wait. */
#include <stdio.h>
static int checks,failures,pins,parks,wakes;
static uint32_t alias;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
uint64_t spin_lock_irqsave(spinlock_t *l){(void)l;return 0;}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)l;(void)f;}
int sched_current_tid(void){return 50;}
uint64_t sched_current_cr3(void){return 0x1000;}
int user_pin_word(const void *u,uint64_t *phys,const void **cpu)
{ alias=*(uint32_t *)u;*(uint32_t *)u=99;*phys=4096;*cpu=&alias;pins++;return 0; }
void user_unpin_word(uint64_t phys){(void)phys;pins--;}
void sched_block_self_unlock(spinlock_t *l,uint64_t f)
{(void)l;(void)f;parks++;CHECK(pins==0,"futex releases physical pin before parking");}
int sched_block_self_unlock_until(spinlock_t *l,uint64_t f,uint64_t until)
{(void)until;sched_block_self_unlock(l,f);return 1;}
uint64_t timer_ms(void){return 1;}
int sched_wake_id(int tid){(void)tid;wakes++;return 1;}
int main(void)
{
    uint32_t user=7;
    CHECK(futex_wait(&user,7,0)==0,"futex compares pinned page after concurrent VA replacement");
    CHECK(parks==1,"matching pinned word enters wait despite remapped user word");
    CHECK(pins==0,"successful futex leaves no pin");
    user=8;CHECK(futex_wait(&user,7,0)==FUTEX_E_AGAIN,"mismatch refuses wait");
    CHECK(pins==0,"mismatch drops physical pin");
    struct fwaiter ws[64];uint64_t key=(uint64_t)(uintptr_t)&user^(sched_current_cr3()<<1);
    struct fbucket *b=&g_fb[fhash(key)];b->head=&ws[0];
    for(int i=0;i<64;i++)ws[i]=(struct fwaiter){.next=i<63?&ws[i+1]:NULL,.key=key,.tid=i,.woken=0};
    CHECK(futex_wake(&user,64)==64,"broadcast counts all 64 waiters");
    CHECK(wakes==64,"broadcast unparks every counted waiter beyond 32");
    CHECK(futex_wake(&user,64)==0,"duplicate wake cannot count old completion twice");
    b->head=NULL;printf("BKL_FUTEX checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
