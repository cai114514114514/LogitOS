/* SPDX-License-Identifier: GPL-3.0-or-later
 * The runner extracts the real ptrace copy helper AND vmm_copy_in_space.
 * The host provides page lookup/pinning primitives; actual copy, alias choice,
 * guard scope and permission routing are production code. Stop/reap scheduling
 * remains a guest gate, not something this fixture claims to simulate. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "mmhost.h"
#include "mm.h"
#include "ptrace.h"
uint64_t mm_host_base, mm_host_kend, mm_host_cr3;
static uint64_t fixture_pte;
static int live=1, guards, pins, copies, bad_alias, checks, failures;
static void check(int yes,const char *why) { checks++; if(!yes){printf("FAIL: %s\n",why);failures++;} }
struct fixture_guard { int held; };
static struct fixture_guard guard_start(uint64_t cr3) { (void)cr3;guards++;return (struct fixture_guard){1}; }
static void guard_end(struct fixture_guard *g) { if(g->held)guards--; }
#define MM_GUARD(cr3) struct fixture_guard guard __attribute__((cleanup(guard_end)))=guard_start(cr3)
static int mm_space_live(uint64_t cr3) { (void)cr3;return live; }
static int vmm_user_range_ok(uint64_t cr3,const void *va,uint64_t len,int write)
{ (void)cr3;return mm_user_range((uint64_t)(uintptr_t)va,len) && (fixture_pte&5)==5 && (!write || (fixture_pte&2)); }
static int user_page_ok(uint64_t cr3,uint64_t va,int write)
{ return vmm_user_range_ok(cr3,(void *)(uintptr_t)va,1,write); }
static int vmm_pin_user_page(uint64_t cr3,uint64_t va,int write,uint64_t *phys)
{ if(!user_page_ok(cr3,va,write))return -1;*phys=fixture_pte&0x000ffffffffff000ull;pins++;return 0; }
static void pmm_unpin(uint64_t phys) { (void)phys;pins--; }
static void pmm_free(uint64_t phys) { (void)phys; }
static void *fixture_copy(void *dst,const void *src,size_t n)
{
    copies++;
    check(guards>0&&pins>0,"copy retains both AS guard and physical pin");
    uintptr_t d=(uintptr_t)dst,s=(uintptr_t)src;
    /* Let the old physical-pointer control fail assertions, not crash in an
     * invalid host access. No false data is supplied when its alias is wrong. */
    if(d<0x100100000ull || s<0x100100000ull){bad_alias++;return dst;}
    return memcpy(dst,src,n);
}
#define memcpy fixture_copy
#include "physmap_vmm_copy.inc"
#undef memcpy
#include "physmap_ptrace_xlate.inc"
int main(void)
{
    uint64_t extent=0x100001000ull;
    void *arena=mmap(NULL,extent,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(arena==MAP_FAILED){perror("ptrace arena");return 2;}
    mm_host_base=(uint64_t)(uintptr_t)arena;
    uint64_t va=0x10000001000ull, value=0x5566778899aabbccull;
    fixture_pte=0x100000000ull|7|(1ull<<63);
    check(xlate(0x1000,va+16,1,&value)==PT_OK,"high physical page is traceable");
    check(!bad_alias,"ptrace high frame uses CPU alias rather than physical pointer");
    check(*(uint64_t *)mm_p2v(0x100000010ull)==value,"tracer write reaches exact high physical word");
    value=0;check(xlate(0x1000,va+16,0,&value)==PT_OK&&value==0x5566778899aabbccull,"remote read copies high page contents");
    fixture_pte=0x200000|7;value=123;
    check(xlate(0x1000,0x50001000,1,&value)==PT_OK,"legacy tracee remains writable");
    check(*(uint64_t *)mm_p2v(0x200000)==123,"legacy page uses RAM alias");
    fixture_pte=0x100000000ull|5;
    check(xlate(0x1000,va,1,&value)==PT_E_FAULT,"readonly/COW pages still refuse writes");
    check(xlate(0x1000,va,0,&value)==PT_OK,"readonly high page remains readable");
    fixture_pte=0x100000000ull|3;
    check(xlate(0x1000,va,0,&value)==PT_E_FAULT,"supervisor page remains inaccessible");
    fixture_pte=0;check(xlate(0x1000,va,0,&value)==PT_E_FAULT,"absent page remains unfaulted");
    check(xlate(0x1000,va+1,0,&value)==PT_E_ALIGN,"unaligned requests remain rejected");
    fixture_pte=0x100000000ull|7;live=0;
    check(xlate(0x1000,va,0,&value)==PT_E_FAULT,"retired address space refuses copy");
    check(pins==0&&guards==0,"all success and refusal paths release guards and pins");
    check(copies>=4,"real VMM copy path is exercised");
    munmap(arena,extent);printf("physmap ptrace: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
