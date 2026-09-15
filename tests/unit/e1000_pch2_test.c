/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production-TU MMIO apparatus. Register constants in the device
 * model below intentionally use datasheet addresses, including SHRA=5438,
 * so a plausible but wrong driver's register constant cannot agree with it.
 * This checks driver decisions; it does not model analog link or ME firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define LOGIT_NET_HOST 1
#define E1000_PCH2_HOST 1
#include "../../c/drivers/net/e1000_pch2.c"

static uint32_t regs[0x20000/4];
static uint16_t config[128], phys[33][32], emi[65536], km[32];
static unsigned selected,emi_addr,checks,failures,resets,mdios,early_reads;
static unsigned frees,allocs,submits,quarantines,quiesces,unsafe_frees,schedules,delivered;
static unsigned bad_dma_start,bad_phy_access,bad_rar_access;
static uint64_t now,reset_due,next_dma;
static int no_clock,stopped_clock,hold_reset,hold_master,hold_lan,deny_sw,hold_sw_release;
static int bad_mdio,mdio_timeout,stale_mdio,hold_d0,bad_bar,fail_alloc,fail_submit;
static int deny_ring,lose_intx,pm_missing,late_fw,drop_imc,tiny_tx_fifo;
static struct device card;
static char logs[16384];static unsigned log_n;
static uint8_t received[2048];static unsigned received_len;
#define CHECK(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s\n",m);}}while(0)

void kprintf(const char *fmt,...)
{
    /* Kernel %x arguments here are integer-sized; record only the message
     * template, avoiding host/kernel formatter ABI differences for %p. */
    size_t n=strlen(fmt);if(n>sizeof(logs)-log_n-1)n=sizeof(logs)-log_n-1;
    memcpy(logs+log_n,fmt,n);log_n+=n;logs[log_n]=0;
}
int time_ready(void){return !no_clock;}
uint64_t time_mono_ns(void)
{
    if(!stopped_clock)now+=NS_PER_MS;
    if(reset_due && now>=reset_due && !hold_reset) {
        regs[0/4]&=~((1u<<26)|(1u<<31));
        if(!hold_lan)regs[8/4]|=1u<<9;
        reset_due=0;
    }
    return now;
}
uint32_t pch_model_read(unsigned r)
{
    if(reset_due && now<reset_due)early_reads++;
    if(r>=sizeof(regs) || (r&3))return UINT32_MAX;
    uint32_t v=regs[r/4];
    if(r==0xc0 && ((v&(1u<<31)) || !regs[0xd0/4]))regs[r/4]=0;
    return v;
}
static void mdio_model(uint32_t command)
{
    unsigned addr=(command>>21)&31,reg=(command>>16)&31;
    int write=!!(command&(1u<<26));uint16_t data=command;
    mdios++;
    if(mdio_timeout){regs[0x20/4]=command;return;}
    uint32_t result=command|(1u<<28);
    if(!(regs[0xf00/4]&(1u<<5)) || (addr!=1 && addr!=2))bad_phy_access++;
    if(addr==1 && reg==31 && write)selected=data>>5;
    else {
        unsigned page=addr==2?0:(selected?selected:768);
        if(addr==2 && reg>15 && selected)bad_phy_access++;
        if(addr==1 && reg<16){result|=1u<<30;data=0xffff;}
        else if(page && (page<768 || page>=800)){result|=1u<<30;data=0xffff;}
        else if(addr==2 && reg==16 && write)emi_addr=data;
        else if(addr==2 && reg==17) {if(write)emi[emi_addr]=data;else data=emi[emi_addr];}
        else {
            unsigned p=page?page-767:0;
            if(write)phys[p][reg]=data;
            else data=phys[p][reg];
            if(write && page==0 && reg==0)phys[p][reg]&=~0x8200;
        }
    }
    if(bad_mdio)result|=1u<<30;
    if(stale_mdio)result^=1u<<16;
    regs[0x20/4]=(result&0xffff0000u)|data;
}
void pch_model_write(unsigned r,uint32_t v)
{
    if(reset_due && now<reset_due)early_reads++;
    if(r>=sizeof(regs) || (r&3))return;
    if(r>=0x5408 && r<0x5438)bad_rar_access++;
    if(r==0x20){mdio_model(v);return;}
    if(r==0x34) {
        unsigned idx=(v>>16)&31;
        if(v&(1u<<21))regs[r/4]=(v&0xffff0000u)|km[idx];
        else{km[idx]=v;regs[r/4]=v;}return;
    }
    if(r==0xc0){CHECK(0,"driver never depends on ICR write acknowledgement");return;}
    if(r==0xd8){if(!drop_imc)regs[0xd0/4]&=~v;return;}
    if(r==0xd0){regs[r/4]|=v;return;}
    if(r==0x1000){regs[r/4]=(v&31u)|((tiny_tx_fifo?4u:8u)<<16);return;}
    if(r==0xf00) {
        if(deny_sw)v&=~(1u<<5);
        if(hold_sw_release && (regs[r/4]&(1u<<5)))v|=1u<<5;
    }
    if(deny_ring && r==0x2800)return;
    regs[r/4]=v;
    if(r==0) {
        if((v&(1u<<2)) && !hold_master)regs[8/4]&=~(1u<<19);
        if(v&(1u<<26)) {
            if(regs[8/4]&(1u<<19))bad_dma_start++;
            resets++;reset_due=now+20*NS_PER_MS;
            regs[8/4]&=~(1u<<9);regs[0xf00/4]&=~(1u<<5);
            regs[0x100/4]=regs[0x400/4]=regs[0xd0/4]=0;
            regs[0x18/4]&=~(1u<<28);
            if(late_fw)regs[0x5b54/4]|=1u<<15;
        }
    }
}
uint8_t pci_cap_find(uint8_t b,uint8_t s,uint8_t f,uint8_t id)
{(void)b;(void)s;(void)f;return id==1?(pm_missing?0:0x40):id==5?0x50:id==0x11?0x60:0;}
uint16_t pci_cfg_read16(uint8_t b,uint8_t s,uint8_t f,uint16_t off)
{(void)b;(void)s;(void)f;return config[off/2];}
void pci_cfg_write16(uint8_t b,uint8_t s,uint8_t f,uint16_t off,uint16_t v)
{
    (void)b;(void)s;(void)f;
    if(off==0x44) {
        /* PME_Status is W1C; a D0 transition must not acknowledge it. */
        uint16_t status=(config[off/2]&0x8000)&~v;
        if(hold_d0)v=(v&~3u)|(config[off/2]&3);
        v=(v&0x7fff)|status;
    }
    if(off==4 && (v&4)) {
        if(submits<3 || !pch.rings || !pch.rxbuf || !pch.txbuf)bad_dma_start++;
        if(lose_intx)v&=~0x400u;
    }
    config[off/2]=v;
}
uint64_t dev_bar_map(struct device *d,int idx)
{(void)d;(void)idx;return bad_bar?0:(uintptr_t)regs;}
void net_rx_schedule(void){schedules++;}
void dma_wmb(void){}void dma_rmb(void){}
void dma_device_init(struct dma_device *d,const char *n,uint64_t mask)
{memset(d,0,sizeof(*d));d->name=n;d->mask=mask;}
struct dma_buffer *dma_alloc_coherent(struct dma_device *d,size_t n,size_t align,size_t boundary)
{
    (void)boundary;allocs++;if((int)allocs==fail_alloc)return NULL;
    struct dma_buffer *b=calloc(1,sizeof(*b));
    if(!b || posix_memalign(&b->cpu,align,n))abort();
    memset(b->cpu,0,n);b->size=n;b->pages=(n+4095)/4096;b->dev=d;b->dma.value=next_dma;
    b->phys=next_dma;next_dma+=n+4096;b->state=DMA_READY;b->next=d->buffers;d->buffers=b;
    CHECK(b->dma.value+n-1<=d->mask,"coherent allocation remains within requested DMA mask");
    return b;
}
uint64_t dma_buffer_submit(struct dma_buffer *b)
{submits++;if((int)submits==fail_submit)return 0;b->state=DMA_DEVICE_OWNED;return b->token=submits;}
int dma_free_coherent(struct dma_buffer *b)
{
    if((regs[8/4]&(1u<<19)) || (config[4/2]&4))unsafe_frees++;
    if(b->state==DMA_DEVICE_OWNED || b->state==DMA_QUARANTINED){unsafe_frees++;return -1;}
    struct dma_buffer **p=&b->dev->buffers;while(*p && *p!=b)p=&(*p)->next;
    if(*p)*p=b->next;free(b->cpu);free(b);frees++;return 0;
}
void dma_device_quiesced(struct dma_device *d)
{quiesces++;d->blocked=1;for(struct dma_buffer *b=d->buffers;b;b=b->next){b->state=DMA_QUIESCED;b->token=0;}}
void dma_device_quarantine(struct dma_device *d)
{quarantines++;d->blocked=1;for(struct dma_buffer *b=d->buffers;b;b=b->next)b->state=DMA_QUARANTINED;}
static void rx_cb(const uint8_t *buf,uint16_t len)
{delivered++;received_len=len;memcpy(received,buf,len);}
static void fixture(void)
{
    /* Apparatus teardown between independent emulated machines. Production
     * quarantine is checked BEFORE this, and is never counted as a free. */
    struct dma_buffer *b=pch.dma.buffers;
    while(b){struct dma_buffer *next=b->next;free(b->cpu);free(b);b=next;}
    memset(&pch,0,sizeof(pch));memset(regs,0,sizeof(regs));memset(config,0,sizeof(config));
    memset(phys,0,sizeof(phys));memset(emi,0,sizeof(emi));memset(km,0,sizeof(km));memset(&card,0,sizeof(card));
    now=reset_due=0;next_dma=0x40000000;selected=emi_addr=0;
    resets=mdios=early_reads=frees=allocs=submits=quarantines=quiesces=unsafe_frees=0;
    schedules=delivered=received_len=bad_dma_start=bad_phy_access=bad_rar_access=0;
    no_clock=stopped_clock=hold_reset=hold_master=hold_lan=deny_sw=hold_sw_release=0;
    bad_mdio=mdio_timeout=stale_mdio=hold_d0=bad_bar=fail_alloc=fail_submit=0;
    deny_ring=lose_intx=pm_missing=late_fw=drop_imc=tiny_tx_fifo=0;log_n=0;logs[0]=0;
    card.bus_type=DEV_BUS_PCI;card.vendor=0x8086;card.device=0x1502;card.class_code=2;card.slot=25;
    card.res[0].flags=DEV_RES_MEM;card.res[0].size=0x20000;card.irq_line=11;
    config[4/2]=6;config[0x44/2]=0x8003;config[0x52/2]=1;config[0x62/2]=0x8000;
    regs[8/4]=(1u<<19)|0x83;regs[0x5b54/4]=1u<<6;regs[0xf00/4]=8;
    regs[0x5400/4]=0x12005452;regs[0x5404/4]=0x80005634;
    for(unsigned i=0x5408;i<0x5438;i+=4)regs[i/4]=0x12345678;
    phys[0][2]=0x0154;phys[0][3]=0x0090;phys[0][0]=0x0800;phys[3][17]=0x4000;
    phys[5][20]=0x6000;phys[1][25]=0x0004;
}
static int bringup(void)
{int rc=e1000_pch2_probe(&card);CHECK(rc==0,"complete non-managed 82579 probe succeeds");return rc;}
static void normal(void)
{
    fixture();if(bringup())return;
    CHECK(card.drvdata==&pch_net && pch.online,"netdev published only after full initialization");
    CHECK(resets==1 && !early_reads,"MAC PHY reset receives 20ms quiet window");
    CHECK(!bad_phy_access && mdios>50,"all MDIO operations use SWFLAG and correct HV address");
    CHECK(!bad_rar_access && regs[0x5408/4]==0x12345678,"ME-owned RAR1 through RAR6 are untouched");
    CHECK(emi[0x084f]==0x34 && emi[0x2411]==5,"82579 MSE errata parameters programmed");
    CHECK((phys[2][16]&0x400) && !(phys[3][17]&0x4000),"MDIO slow mode enabled and K1 disabled");
    CHECK(!(phys[5][20]&0x6000) && emi[0x040e]==0,"EEE advertisement and LPI disabled");
    CHECK((config[0x44/2]&0x8003)==0x8000,"D3 to D0 preserves W1C PME status");
    CHECK(!(config[0x52/2]&1) && !(config[0x62/2]&0x8000),"MSI and MSI-X are disabled with readback");
    CHECK((config[4/2]&0x406)==0x406 && !regs[0xd0/4],"INTx remains masked until common route admission");
    CHECK(!(regs[0xf00/4]&(1u<<5)) && (regs[0x18/4]&(1u<<28)),"SWFLAG released while driver identity remains claimed");
    CHECK(!bad_dma_start && pch.dma.mask==0xffffffffu,"DMA starts only after three submitted DMA32 buffers");
    CHECK(pch.rx[0].addr==pch.rxbuf->dma.value && pch.rx[0].addr!=(uintptr_t)pch.rxbuf->cpu,
          "descriptors contain DMA addresses instead of CPU pointers");
    CHECK(pch.link==0x83,"reported link reads negotiated gigabit full duplex status");
    uint8_t frame[1518];for(unsigned i=0;i<sizeof(frame);i++)frame[i]=(uint8_t)(i*37+3);
    CHECK(!pch_net.tx(frame,sizeof(frame)) && pch.tx[0].cmd==0x0b && pch.tx[0].length==1518,
          "TX uses legacy EOP IFCS RS descriptor");
    CHECK(!memcmp(pch.txbuf->cpu,frame,sizeof(frame)) && pch.tx_good==0,"TX copies exact bytes and does not invent completion");
    pch.tx[0].status=1;reap();CHECK(pch.tx_good==1 && !pch.pending,"TX DD completes one queued frame");
    for(unsigned i=0;i<31;i++)CHECK(!pch_net.tx(frame,60),"TX admits available ring slots");
    CHECK(pch_net.tx(frame,60)<0 && pch.pending==31,"TX reserves one empty slot across wrap");
    for(unsigned i=0;i<32;i++)pch.tx[i].status=1;reap();
    CHECK(pch.tx_good==32 && !pch.pending,"TX wrap completion drains all queued frames");
    CHECK(!pch_net.tx(frame,60),"TX accepts a frame after wrapped completions");
    pch.tx[0].status=3;reap();CHECK(pch.tx_bad==1 && pch.tx_good==32,"TX descriptor errors are counted as failure");
    CHECK(pch_net.tx(frame,13)<0 && pch_net.tx(frame,1519)<0,"invalid Ethernet lengths rejected");
    memcpy(pch.rxbuf->cpu,frame,127);pch.rx[0].length=127;pch.rx[0].status=3;
    CHECK(pch_net.rx_poll(rx_cb)==1 && received_len==127 && !memcmp(received,frame,127),"RX delivers exact DMA payload bytes");
    CHECK(pch.rx[0].status==0 && regs[0x2818/4]==0,"RX reposts consumed descriptor and tail");
    pch.rx[1].length=64;pch.rx[1].status=3;pch.rx[1].errors=1;
    CHECK(pch_net.rx_poll(rx_cb)==0 && delivered==1 && pch.rx_bad==1,"RX hardware errors are never delivered");
    pch.rx[2].length=3000;pch.rx[2].status=3;
    CHECK(pch_net.rx_poll(rx_cb)==0 && pch.rx_bad==2,"RX overlength is rejected before callback");
    pch.rx[3].length=64;pch.rx[3].status=1;
    CHECK(pch_net.rx_poll(rx_cb)==0 && pch.rx_bad==3,"fragment without EOP is rejected");
    pch_net.irq_enable(rx_cb);pch_net.irq();CHECK(schedules==0,"shared IRQ with zero cause is ignored");
    regs[0xc0/4]=UINT32_MAX;pch_net.irq();CHECK(schedules==0,"removed-device all-ones cause is ignored");
    regs[0xc0/4]=0x80000080;pch_net.irq();
    CHECK(schedules==1 && !regs[0xc0/4],"asserted RX IRQ is read-cleared and schedules deferred receive");
    regs[8/4]=0x42;pch_net.rx_poll(rx_cb);CHECK(pch.link==0x42 && ((regs[0x400/4]>>12)&0x3ff)==511,"100M half-duplex adjusts collision distance");
    regs[8/4]=0;pch_net.rx_poll(rx_cb);CHECK(pch_net.tx(frame,60)<0,"link down refuses new transmit");
    regs[8/4]=0x83;regs[0x5b54/4]=0;pch_net.rx_poll(rx_cb);
    CHECK(pch.online,"loss of PHY reset permission alone does not stop a running link");
    e1000_pch2_remove(&card);
    CHECK(frees==3 && !unsafe_frees && !pch.dma.buffers,"remove frees all DMA only after master drain");
    CHECK(!card.drvdata && !pch.pci && !(config[4/2]&4) && !(regs[0x18/4]&(1u<<28)),"remove releases driver identity and bus mastering");
}
static void faults(void)
{
    fixture();card.device=0x1503;CHECK(!e1000_pch2_probe(&card),"82579V uses the same explicit PCH2 path");e1000_pch2_remove(&card);
    const unsigned ids[]={0x10d3,0x153b,0x15b8,0x100e};
    for(unsigned i=0;i<4;i++){fixture();card.device=ids[i];CHECK(e1000_pch2_probe(&card)<0 && !resets && !allocs,"other Intel MAC families rejected before access");}
    fixture();card.vendor=0x1234;CHECK(e1000_pch2_probe(&card)<0 && !resets,"wrong vendor refused");
    fixture();no_clock=1;CHECK(e1000_pch2_probe(&card)<0 && !resets,"unready monotonic clock refuses probe");
    fixture();hold_d0=1;CHECK(e1000_pch2_probe(&card)<0 && !mdios && !resets && !(config[2]&4),"D0 readback failure refuses all MAC activity");
    fixture();pm_missing=1;CHECK(e1000_pch2_probe(&card)<0 && !resets,"missing PM capability is diagnosed");
    fixture();bad_bar=1;CHECK(e1000_pch2_probe(&card)<0 && !resets,"unmappable BAR is refused");
    fixture();card.res[0].size=0x4000;CHECK(e1000_pch2_probe(&card)<0 && !resets,"short BAR is rejected before MMIO");
    fixture();regs[0x5b54/4]|=1u<<15;
    CHECK(e1000_pch2_probe(&card)<0 && !resets && !mdios && !allocs,"managed firmware is explicitly refused without PHY reset");
    CHECK(strstr(logs,"managed-firmware path not implemented")!=NULL,"managed refusal is a capability boundary not false semaphore denial");
    fixture();regs[0x5b54/4]=0;CHECK(e1000_pch2_probe(&card)<0 && !resets,"firmware PHY reset denial never triggers reset");
    fixture();regs[0xf00/4]|=1u<<5;
    CHECK(e1000_pch2_probe(&card)<0 && !resets && !mdios && (regs[0xf00/4]&(1u<<5)),"busy SWFLAG is neither stolen nor cleared");
    fixture();deny_sw=1;CHECK(e1000_pch2_probe(&card)<0 && !resets && !mdios,"unlatched SWFLAG refuses PHY access");
    fixture();regs[0x28/4]|=1u<<27;CHECK(e1000_pch2_probe(&card)<0 && !resets,"software NVM configuration requirement is refused");
    fixture();regs[0xf08/4]=1u<<16;CHECK(e1000_pch2_probe(&card)<0 && !resets,"board-specific extended LCD script is refused");
    fixture();regs[0xf00/4]=0;CHECK(e1000_pch2_probe(&card)<0 && !resets,"missing OEM autoload capability is refused");
    fixture();regs[0x18/4]|=1u<<28;CHECK(e1000_pch2_probe(&card)<0 && !resets,"another loaded driver is never reset");
    fixture();hold_master=1;CHECK(e1000_pch2_probe(&card)<0 && !resets,"initial in-flight PCI master timeout prevents reset");
    fixture();tiny_tx_fifo=1;CHECK(e1000_pch2_probe(&card)<0 && !resets && !allocs,"insufficient reported TX FIFO is refused");
    fixture();hold_reset=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"unacknowledged global reset cannot allocate DMA");
    CHECK(resets==1 && !early_reads && pch.poisoned && pch.reset_unconfirmed,
          "reset timeout never restarts reset or performs an early flush");
    fixture();hold_lan=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"NVM LAN initialization timeout cannot enable DMA");
    fixture();late_fw=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"firmware policy rechecked after reset");
    fixture();phys[0][3]=0x0cb0;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"non-82579 PHY ID refuses DMA");
    fixture();bad_mdio=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"MDIO READY plus ERROR is rejected");
    fixture();mdio_timeout=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"MDIO timeout is bounded");
    fixture();stale_mdio=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"stale MDIO echoed register is rejected");
    fixture();hold_sw_release=1;CHECK(e1000_pch2_probe(&card)<0 && pch.poisoned && quarantines==1 && !allocs,
                                   "unconfirmed SWFLAG release quarantines the ownership state");
    fixture();regs[0x5404/4]&=~(1u<<31);CHECK(e1000_pch2_probe(&card)<0 && !allocs,"invalid autoloaded MAC is never invented");
    fixture();regs[0x5400/4]|=1;CHECK(e1000_pch2_probe(&card)<0 && !allocs,"multicast MAC is refused");
    fixture();regs[0x5400/4]=0;regs[0x5404/4]=1u<<31;
    CHECK(e1000_pch2_probe(&card)<0 && !allocs,"zero MAC is refused even with address-valid set");
    for(int i=1;i<=3;i++){fixture();fail_alloc=i;CHECK(e1000_pch2_probe(&card)<0 && !pch.dma.buffers && !unsafe_frees,"partial DMA allocation unwinds safely");}
    fixture();fail_submit=2;CHECK(e1000_pch2_probe(&card)<0 && frees==3 && !bad_dma_start && !unsafe_frees,"failed DMA submit never starts mastering and unwinds");
    fixture();deny_ring=1;CHECK(e1000_pch2_probe(&card)<0 && frees==3 && !(config[2]&4),"ring base readback failure stops probe");
    fixture();lose_intx=1;CHECK(e1000_pch2_probe(&card)<0 && !pch.online && !card.drvdata && frees==3,"INTx mask lost during master enable prevents RX TX start");
    fixture();if(!bringup()) {
        regs[8/4]|=1u<<19;hold_master=1;e1000_pch2_remove(&card);
        CHECK(!frees && quarantines==1 && pch.dma.buffers && pch.poisoned,"unacknowledged master drain retains every DMA buffer");
        CHECK(!unsafe_frees && !pch.online && e1000_pch2_probe(&card)<0,"quarantined adapter cannot transmit or rebind");
    }
    fixture();if(!bringup()) {
        card.irq_mode=DEV_IRQ_INTX;e1000_pch2_remove(&card);
        CHECK(!frees && quarantines==1 && card.drvdata && pch.pci==&card,"live IRQ route prevents descriptor reclamation");
        card.irq_mode=DEV_IRQ_NONE;e1000_pch2_remove(&card);
        CHECK(frees==3 && !unsafe_frees,"remove can finish after IRQ route retirement");
    }
    fixture();if(!bringup()) {
        regs[0x5b54/4]|=1u<<15;pch_net.rx_poll(rx_cb);
        CHECK(!pch.online && pch.poisoned && pch.pci==&card && card.drvdata && !frees,"late firmware mode silences but retains bound identity");
        e1000_pch2_remove(&card);CHECK(frees==3 && !unsafe_frees,"late firmware refusal remains removable");
    }
    fixture();stopped_clock=1;CHECK(e1000_pch2_probe(&card)<0 && !resets,"stopped monotonic clock fails D0 delay instead of hanging");
    fixture();if(!bringup()) {
        card.irq_mode=DEV_IRQ_INTX;pch_net.irq_enable(rx_cb);
        drop_imc=1;regs[0xc0/4]=0x80000080;regs[0x5b54/4]|=1u<<15;
        pch_net.rx_poll(rx_cb);
        CHECK(!regs[0xc0/4] && !pch.online && quarantines==1 && !frees,
              "late firmware with lost IMC clears pending and retains DMA");
        regs[0xc0/4]=0x80000080;pch_net.irq();
        CHECK(!regs[0xc0/4] && !schedules,"offline retained IRQ acknowledges pending without scheduling");
        drop_imc=0;card.irq_mode=DEV_IRQ_NONE;e1000_pch2_remove(&card);
        CHECK(frees==3 && !unsafe_frees,"lost-mask quarantine can be removed after confirmed silence");
    }
}
int main(void)
{
    normal();faults();fixture();
    printf("E1000_PCH2: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
