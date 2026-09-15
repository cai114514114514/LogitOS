/* SPDX-License-Identifier: MIT */
/* Production PCI configuration writes against a byte-enable/W1C device model.
 * A plain RAM model cannot catch Command writes clearing adjacent Status bits. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pci.h"
#include "pci_platform.h"
#include "driver.h"
static unsigned checks, fails, reads, writes, narrow;
static uint32_t address;
static uint8_t config[256];
static uint64_t mapped;
static unsigned maps;
#define CHECK(x,msg) do { checks++; if(!(x)){fails++;printf("FAIL: %s\n",msg);} } while(0)
static void store(unsigned off,uint32_t value,unsigned size) {
    for(unsigned i=0;i<size;i++) {
        if(off+i>=sizeof config)continue;
        uint8_t byte=(uint8_t)(value>>(8*i));
        if(off+i==6||off+i==7)config[off+i]&=(uint8_t)~byte;
        else config[off+i]=byte;
    }
}
void outl(uint16_t port,uint32_t v) {
    if(port==0xcf8){address=v;return;}
    if(port==0xcfc){writes++;store(address&252,v,4);}
}
uint32_t inl(uint16_t port) {
    reads++;uint32_t result=~0u;
    if(port==0xcfc)memcpy(&result,config+(address&252),4);
    return result;
}
void outw(uint16_t p,uint16_t v){narrow++;store((address&252)+(p-0xcfc),v,2);}
void outb(uint16_t p,uint8_t v){narrow++;store((address&252)+(p-0xcfc),v,1);}
uint16_t inw(uint16_t p){(void)p;return 0xffff;}
uint8_t inb(uint16_t p){(void)p;return 0xff;}
void vmm_map_range(uint64_t v,uint64_t p,uint64_t n,uint64_t f) {
    (void)p;(void)n;(void)f;mapped=v;maps++;
}
struct device *dev_add(const struct device *d){(void)d;return NULL;}
int dev_count(void){return 0;}
struct device *dev_find_id(uint16_t v,uint16_t d,struct device *from)
{(void)v;(void)d;(void)from;return NULL;}
static void ports(void) {
    config[6]=0xa5;config[7]=0xc3;
    pci_cfg_write16(0,3,0,4,7);
    CHECK(config[4]==7&&config[5]==0,"Command halfword written");
    CHECK(config[6]==0xa5&&config[7]==0xc3,"Command write preserves W1C Status");
    CHECK(reads==0&&writes==0&&narrow==1,"halfword uses one native write without neighbour read");
    pci_cfg_write8(0,3,0,5,4);
    CHECK(config[4]==7&&config[5]==4,"byte write preserves Command neighbour");
    CHECK(config[6]==0xa5&&config[7]==0xc3,"byte write preserves W1C Status");
    pci_cfg_write8(0,3,0,6,1);
    CHECK(config[6]==0xa4&&config[7]==0xc3,"explicit Status byte clear touches selected lane only");
    unsigned before=narrow+writes;
    pci_cfg_write16(0,3,0,0x100,0xffff);
    CHECK(narrow+writes==before,"legacy extended config write is refused");
}
static void ecam(void) {
    size_t size=4u<<20;uint8_t *space=calloc(1,size);if(!space)exit(2);
    CHECK(!pci_ecam_set((uint64_t)(uintptr_t)space,1,2,3),"unsupported segment is refused");
    CHECK(pci_ecam_set((uint64_t)(uintptr_t)space,0,2,3),"nonzero-start MCFG accepted");
    size_t offset=(2u<<20)|(7u<<15)|(1u<<12);
    *(uint32_t *)(space+offset)=0x12348086;
    CHECK(pci_cfg_read(2,7,1,0)==0x12348086,"MCFG base remains relative to bus zero");
    CHECK(mapped==(uint64_t)(uintptr_t)space+(2u<<20)&&maps==1,"maps actual bus-two physical window");
    pci_cfg_write16(2,7,1,0x104,0xbeef);
    CHECK(*(uint16_t *)(space+offset+0x104)==0xbeef,"extended halfword targets actual selected function");
    pci_cfg_write8(2,7,1,0x107,0xab);
    CHECK(space[offset+0x107]==0xab,"extended byte targets actual selected lane");
    CHECK(maps==1,"repeated ECAM access does not remap");
    free(space);
}
static void intx(void) {
    uint32_t vendor[3];memcpy(vendor,"TCGTCGTCGTCG",12);
    CHECK(pci_intx_level_for_cpu(0,0x40000001,vendor)==1,"physical PCI stays level-triggered");
    CHECK(pci_intx_level_for_cpu(1u<<31,0x40000001,vendor)==0,"explicit TCG retains historical edge workaround");
    memcpy(vendor,"KVMKVMKVM\0\0\0",12);
    CHECK(pci_intx_level_for_cpu(1u<<31,0x40000001,vendor)==1,"other hypervisors use normal PCI level trigger");
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"ports"))ports();else if(!strcmp(argv[1],"ecam"))ecam();else if(!strcmp(argv[1],"intx"))intx();else return 2;
    printf("PCI hardware %s: %u checks, %u failed\n",argv[1],checks,fails);return fails?1:0;
}
