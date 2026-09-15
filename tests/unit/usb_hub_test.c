/* SPDX-License-Identifier: MIT */
/* Real core enumeration, topology, hub class and interface binder. The HCD
 * leaves describe two independent buses, including HS -> FS hub -> LS leaf. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "usb.h"
#include "driver.h"
static int checks,failed,opened,closed,leaf_count,hub_configs,backend_bulk[2],polls[2];
static unsigned address[2],reset_done[64][3],powered[64][3];
static struct usb_device *leaves[8],*root_hub;
static struct usb_device *closed_order[32];
static int wire_clear,backend_clear,fail_wire_clear;
static uint16_t tt_value,tt_index;
static struct usb_device *tt_target;
static unsigned tt_directions;
#define CHECK(c,m) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n",m); } } while (0)
void kprintf(const char *fmt,...) { (void)fmt; }
uint64_t timer_ms(void) { static uint64_t t; return ++t; }
int time_ready(void) { return 1; }
uint64_t time_mono_ns(void) { return timer_ms()*1000000; }
void sched_poll_wait(void) {}
int dev_irq_request(struct device *p,irq_handler_t fn,void *arg,const char *name)
{ (void)fn;(void)arg;(void)name;p->irq_mode=DEV_IRQ_INTX;p->irq_vec=96;return 96; }
int dev_irq_release(struct device *p) { p->irq_mode=DEV_IRQ_NONE; return 0; }
static int hc_id(struct usb_hc *h) { return (int)(uintptr_t)h->priv; }
static int is_hub(struct usb_device *d) { return hc_id(d->hc)==0 && d->port==1 && d->depth<2; }
static int root_count(struct usb_hc *h) { return hc_id(h)?1:2; }
static int connected(struct usb_hc *h,int p) { return p<=root_count(h); }
static int root_reset(struct usb_hc *h,int p,int *speed)
{ (void)h;(void)p;*speed=USB_SPEED_HIGH;return 0; }
static int device_open(struct usb_device *d)
{
    int id=hc_id(d->hc);
    if (d->parent) CHECK(reset_done[d->parent->addr][d->parent_port],"downstream reset precedes child addressing");
    d->addr=(uint8_t)(++address[id]+id*32); d->slot=d->addr; opened++;
    if (is_hub(d) && d->depth==0) root_hub=d;
    return 0;
}
static void device_close(struct usb_device *d) { closed_order[closed++]=d; }
static int set_ep0(struct usb_device *d,int packet) { (void)d;return packet==64?0:-1; }
static int configure(struct usb_device *d,const struct usb_interface *it) { (void)d;(void)it;return 0; }
static int configure_hub(struct usb_device *d,int n,int multi,int think)
{
    CHECK(n==2 && !multi && think==2,"selected single-TT hub metadata reaches backend");
    CHECK(!d->depth || (d->tt_hub==root_hub && d->tt_port==1),"FS hub receives its upstream HS translator");
    hub_configs++; return 0;
}
static int control(struct usb_device *d,uint8_t rt,uint8_t req,uint16_t val,uint16_t idx,void *buf,uint16_t len)
{
    if (req==USB_REQ_GET_DESCRIPTOR && (val>>8)==USB_DT_DEVICE) {
        uint8_t b[18]={18,1,0,2,0,0,0,64,0x34,0x12,1,0,0,1,0,0,0,1};
        if (is_hub(d)) b[4]=9;
        if (len>18)len=18;memcpy(buf,b,len);return len;
    }
    if (req==USB_REQ_GET_DESCRIPTOR && (val>>8)==USB_DT_CONFIG) {
        uint8_t b[18]={9,2,18,0,1,1,0,0x80,50,9,4,0,0,0,0xff,0,0,0};
        if (is_hub(d)) { b[14]=9;b[16]=1; }
        if(len>18)len=18;memcpy(buf,b,len);return len;
    }
    if (req==USB_REQ_GET_DESCRIPTOR && (val>>8)==0x29) {
        uint8_t b[9]={9,0x29,2,0x40,0,1,0,0,0xff};
        if(len>9)len=9;memcpy(buf,b,len);return len;
    }
    if (rt==(USB_RT_TYPE_CLASS|3) && req==USB_REQ_SET_FEATURE) {
        if (idx>2)return -1;
        if (val==8)powered[d->addr][idx]=1;
        if (val==4) { CHECK(powered[d->addr][idx],"hub powers port before reset");reset_done[d->addr][idx]=1; }
        return 0;
    }
    if (rt==(USB_RT_DIR_IN|USB_RT_TYPE_CLASS|3) && req==USB_REQ_GET_STATUS) {
        uint8_t *b=buf;memset(b,0,4);
        if (idx==(d->depth?2:1)) {
            b[0]=1|(reset_done[d->addr][idx]?2:0);
            if(d->depth)b[1]=2; /* LS leaf behind the FS hub */
            b[2]=reset_done[d->addr][idx]?16:1;
        }
        return 4;
    }
    if (rt==USB_RT_RECIP_EP && req==USB_REQ_CLEAR_FEATURE) {
        wire_clear++;return fail_wire_clear?-1:0;
    }
    if (rt==(USB_RT_TYPE_CLASS|3) && req==8) { tt_value=val;tt_index=idx;tt_target=d;tt_directions|=(val&0x8000)?2:1; }
    return 0;
}
static int bulk(struct usb_device *d,uint8_t ep,void *p,uint32_t len)
{ (void)ep;(void)p;backend_bulk[hc_id(d->hc)]++;return len?(int)len-1:0; }
static int clear_halt(struct usb_device *d,uint8_t ep)
{ (void)d;(void)ep;CHECK(wire_clear>backend_clear,"wire CLEAR_FEATURE precedes backend toggle reset");backend_clear++;return 0; }
static void events(struct usb_hc *h) { (void)h; }
static int stop(struct usb_hc *h) { (void)h;return 0; }
static const struct usb_hc_ops ops={.name="fixture",.root_port_count=root_count,.root_port_connected=connected,
    .root_port_reset=root_reset,.device_open=device_open,.device_close=device_close,.set_ep0_packet=set_ep0,
    .configure=configure,.configure_hub=configure_hub,.control=control,.bulk=bulk,.clear_halt=clear_halt,
    .events=events,.shutdown=stop};
static int leaf_probe(struct usb_device *d,int ifno)
{ (void)ifno;leaves[leaf_count++]=d;return 0; }
static void leaf_poll(struct usb_device *d,int ifno) { (void)ifno;polls[hc_id(d->hc)]++; }
static const struct usb_match leaf_ids[]={ {0xff,0,0},{0,0,0} };
static const struct usb_driver leaf_driver={.name="leaf",.match=leaf_ids,.probe=leaf_probe,.poll=leaf_poll};
void usb_hid_register(void) {}
void usb_msc_register(void) { usb_register_driver(&leaf_driver); }
int main(void)
{
    struct usb_hc a={0},b={0};struct device pa={0},pb={0};
    CHECK(!usb_hc_register(&a,&pa,&ops,(void *)0),"first controller registers");
    CHECK(opened==4 && leaf_count==2 && hub_configs==2,"nested hub boot enumerates both leaf devices");
    struct usb_device *leaf=NULL,*other=NULL;
    for(int i=0;i<leaf_count;i++)if(leaves[i]->depth==2)leaf=leaves[i];
    CHECK(leaf!=NULL,"nested low-speed leaf exists");
    if(leaf) {
        CHECK(leaf->route==0x21 && leaf->port==1 && leaf->parent_port==2,"route retains root port and both downstream hops");
        CHECK(leaf->tt_hub==root_hub && leaf->tt_port==1 && !leaf->tt_multi,"nested leaf inherits nearest HS hub translator port");
        CHECK(leaf->speed==USB_SPEED_LOW,"hub status chooses low-speed child encoding");
        leaf->tt_port=2;
        CHECK(!usb_clear_tt_buffer(leaf,0x81,USB_XFER_BULK) && tt_target==root_hub &&
            tt_index==1 && tt_value==(uint16_t)(0x9001|leaf->addr<<4),"single TT clear uses USB address and selector one");
        leaf->tt_multi=1;
        CHECK(!usb_clear_tt_buffer(leaf,0x81,USB_XFER_BULK) && tt_index==2,"multi TT clear selects the upstream translator port");
        leaf->tt_multi=0;leaf->tt_port=1;
        tt_directions=0;
        CHECK(!usb_clear_tt_buffer(leaf,0,USB_XFER_CONTROL) && tt_directions==3,"control error clears both TT directions");
    }
    CHECK(!usb_hc_register(&b,&pb,&ops,(void *)1),"second controller registers independently");
    for(int i=0;i<leaf_count;i++)if(leaves[i]->hc==&b)other=leaves[i];
    CHECK(other && other->hc->priv==(void *)1,"second bus keeps its backend context");
    char data[32];
    if(leaf)CHECK(usb_bulk(leaf,0x81,data,32)==31,"bulk preserves a backend short packet result");
    if(other)CHECK(usb_bulk(other,0x81,data,32)==31,"bulk selects second controller backend");
    CHECK(backend_bulk[0]==1 && backend_bulk[1]==1,"bulk never routes through a global controller");
    if(other) {
        CHECK(!usb_clear_halt(other,0x81) && wire_clear==1 && backend_clear==1,"clear halt resets wire and backend state");
        fail_wire_clear=1;
        CHECK(usb_clear_halt(other,0x81)<0 && backend_clear==1,"failed wire clear never reports successful toggle reset");
        CHECK(usb_bulk(other,0,data,32)<0,"bulk rejects endpoint zero");
    }
    usb_hc_irq(&a);CHECK(polls[0]==2 && polls[1]==0,"IRQ only polls interfaces on its own controller");
    usb_hc_unregister(&a);
    CHECK(closed==4 && (!leaf || closed_order[0]==leaf),"controller removal closes deepest child before hubs");
    if(other)CHECK(usb_bulk(other,0x81,data,32)==31,"removing one controller preserves the other bus");
    usb_hc_irq(&b);CHECK(polls[1]==1,"remaining controller still delivers callbacks");
    CHECK(usb_present() && usb_devices_found()==1,"present count survives independent removal");
    usb_hc_unregister(&b);CHECK(!usb_present() && !usb_devices_found(),"last controller removal clears presence");
    printf("USB hub/core: %d checks, %d failures\n",checks,failed);return failed!=0;
}
