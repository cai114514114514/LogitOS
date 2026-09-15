/* SPDX-License-Identifier: MIT */
/* Actual VGA/serial bodies, with the framebuffer and port leaves in RAM. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "vga.h"
#include "serial.h"
/* This gate measures hardware byte transactions; the separate printf gate
 * links both real modules to test their shared whole-message domain. */
uint64_t kprintf_console_enter(void) { return UINT64_MAX; }
void kprintf_console_leave(uint64_t flags) { (void)flags; }
volatile uint16_t vga_host_cells[80*25];
static unsigned vga_arrivals,tx_statuses,tx_busy,tx_completed,tx_lost,tx_writes;
static int failures;
int bkl_vga_index(int index){
    __atomic_add_fetch(&vga_arrivals,1,__ATOMIC_SEQ_CST);
    while(__atomic_load_n(&vga_arrivals,__ATOMIC_SEQ_CST)<2)sched_yield();
    return index;
}
uint8_t inb(uint16_t port){
    if(port!=0x3fd)return 0;
    unsigned ready=!__atomic_load_n(&tx_busy,__ATOMIC_SEQ_CST);
#ifdef SERIAL_CTL_RACE
    if(ready){
        __atomic_add_fetch(&tx_statuses,1,__ATOMIC_SEQ_CST);
        while(__atomic_load_n(&tx_statuses,__ATOMIC_SEQ_CST)<2)sched_yield();
    }
#endif
    return ready?0x20:0;
}
void outb(uint16_t port,uint8_t value){
    (void)value;if(port!=0x3f8)return;
    if(__atomic_exchange_n(&tx_busy,1,__ATOMIC_SEQ_CST))__atomic_add_fetch(&tx_lost,1,__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&tx_writes,1,__ATOMIC_SEQ_CST);
#ifdef SERIAL_CTL_RACE
    __atomic_add_fetch(&tx_completed,1,__ATOMIC_SEQ_CST);
    while(__atomic_load_n(&tx_completed,__ATOMIC_SEQ_CST)<2)sched_yield();
#endif
    __atomic_store_n(&tx_busy,0,__ATOMIC_SEQ_CST);
}
static void *vga_writer(void *p){vga_putc((char)(intptr_t)p);return 0;}
static void *tx_writer(void *p){serial_putc((char)(intptr_t)p);return 0;}
int main(int argc,char **argv){
    setbuf(stdout,NULL);pthread_t a,b;
    if(argc>1&&!strcmp(argv[1],"serial")){
        pthread_create(&a,0,tx_writer,(void *)'a');pthread_create(&b,0,tx_writer,(void *)'b');
        pthread_join(a,0);pthread_join(b,0);
        if(tx_writes!=2||tx_lost){puts("FAIL: UART ready and write are one transaction");failures++;}
    }else{
        vga_clear();pthread_create(&a,0,vga_writer,(void *)'a');pthread_create(&b,0,vga_writer,(void *)'b');
        pthread_join(a,0);pthread_join(b,0);
        int seen_a=0,seen_b=0;
        for(int i=0;i<80*25;i++){seen_a+=(vga_host_cells[i]&255)=='a';seen_b+=(vga_host_cells[i]&255)=='b';}
        if(seen_a!=1||seen_b!=1){puts("FAIL: VGA simultaneous writers preserve both characters");failures++;}
        for(int i=0;i<5000;i++)vga_putc(i%79==0?'\n':'x'); /* ASan checks scroll bounds. */
        vga_panic_takeover();vga_clear();vga_putc('p');
        if((vga_host_cells[0]&255)!='p'){puts("FAIL: VGA panic sink remains usable");failures++;}
    }
    printf("BKL_CONSOLE failures=%d\n",failures);return failures?1:0;
}
