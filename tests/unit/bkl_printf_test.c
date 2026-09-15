/* SPDX-License-Identifier: MIT */
/* No test-side message lock: real kprintf and serial_puts share ownership. */
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "kprintf.h"
#include "serial.h"
static char output[32768];static unsigned used,arrivals;static _Thread_local unsigned first;
void vga_putc(char c){(void)c;}
void klog_putc(int level,char c){(void)level;(void)c;}
uint8_t inb(uint16_t port){(void)port;return 0x20;}
void outb(uint16_t port,uint8_t c){
    if(port!=0x3f8)return;
    unsigned n=__atomic_fetch_add(&used,1,__ATOMIC_RELAXED);if(n<sizeof(output)-1)output[n]=c;
}
void printf_byte_pause(void) {
#ifdef PRINTF_CTL_RACE
    if(!first++){
        __atomic_add_fetch(&arrivals,1,__ATOMIC_SEQ_CST);
        while(__atomic_load_n(&arrivals,__ATOMIC_SEQ_CST)<2)sched_yield();
    }
#endif
}
static void *writer(void *p){
    int cpu=(int)(intptr_t)p;
    for(int n=0;n<100;n++) {
        if(cpu<2)kprintf("MSG cpu=%d mask=%x seq=%d\n",cpu,0x11u*(cpu+1),n);
        else { char line[80];ksnprintf(line,sizeof line,"MSG cpu=%d mask=%x seq=%d\n",cpu,0x11u*(cpu+1),n);serial_puts(line); }
    }
    return 0;
}
int main(void){
    pthread_t a,b,c;pthread_create(&a,0,writer,(void *)0);pthread_create(&b,0,writer,(void *)1);pthread_create(&c,0,writer,(void *)2);pthread_join(a,0);pthread_join(b,0);pthread_join(c,0);
    unsigned seen[3][100]={{0}};int bad=0,nlines=0;char *line=output;
    while(*line){char *e=strchr(line,'\n');if(!e){bad++;break;}*e=0;int c=-1,n=-1;unsigned m=0;char extra;
        if(sscanf(line,"MSG cpu=%d mask=%x seq=%d%c",&c,&m,&n,&extra)!=3||c<0||c>=3||n<0||n>=100||m!=0x11u*(c+1))bad++;
        else seen[c][n]++;nlines++;line=e+1;
    }
    for(int c=0;c<3;c++)for(int n=0;n<100;n++)if(seen[c][n]!=1)bad++;
    if(bad)puts("FAIL: simultaneous kprintf messages stay parseable and complete");
    printf("BKL_PRINTF lines=%d failures=%d\n",nlines,bad);return bad?1:0;
}
