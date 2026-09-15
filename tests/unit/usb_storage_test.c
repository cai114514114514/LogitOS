/* SPDX-License-Identifier: MIT
 * Real class probe/ops/removal over captured USB leaf transactions. The fake
 * SCSI device supplies media; the production BOT and SCSI class do all framing,
 * discovery, chunking and sense recovery. This is not HCD/DMA emulation. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "usb_msc_under_test.inc"
static int checks,failures;
#define CHECK(c,n) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",n);}}while(0)
static const struct usb_driver *registered;
static struct blkdev disks[16];static int ndisks,part_scans,offline_count;
static uint8_t media[2][512*512],cbw[31],sense_data[18];
static unsigned phase,tag,bytes,lun,status,residue,max_lun=1;
static unsigned commands[256],bulk_calls,control_calls,halt_calls,delays,flushes;
static int unit_attention=1,write_fail,capacity_4kn,force_residue,delay_error;
static void putle(uint8_t*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t getle(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void be32(uint8_t*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
void usb_register_driver(const struct usb_driver*d){registered=d;}
void kprintf(const char*f,...){(void)f;}
int usb_delay_ms(unsigned n){delays+=n;return delay_error;}
struct blkdev *blk_register(const char*n,const struct blk_ops*o,void*c,uint64_t sectors)
{struct blkdev*d=&disks[ndisks++];memset(d,0,sizeof*d);strcpy(d->name,n);d->ops=o;d->ctx=c;d->nsectors=sectors;return d;}
void blk_probe_partitions(struct blkdev*d){CHECK(d->parent==NULL,"late partition scan receives whole disk");part_scans++;}
void blk_dev_offline(struct blkdev*d){d->offline=1;offline_count++;}
int usb_control(struct usb_device*d,uint8_t rt,uint8_t req,uint16_t v,uint16_t i,void*data,uint16_t n)
{
    (void)d;control_calls++;
    CHECK(v==0&&i==5,"class control uses bInterfaceNumber rather than array index");
    if(req==0xfe){CHECK(rt==0xa1&&n==1,"class asks GET_MAX_LUN");*(uint8_t*)data=(uint8_t)max_lun;return 1;}
    CHECK(req==0xff&&rt==0x21&&!n,"class uses BOT reset recovery");phase=0;return 0;
}
int usb_clear_halt(struct usb_device*d,uint8_t ep){(void)d;CHECK(ep==0x81||ep==2,"recover one of the configured bulk endpoints");halt_calls++;return 0;}
int usb_bulk(struct usb_device*d,uint8_t ep,void*data,uint32_t n)
{
    (void)d;bulk_calls++;
    if(!phase) {
        CHECK(ep==2&&n==31&&getle(data)==0x43425355,"real class submits framed CBW");
        memcpy(cbw,data,31);tag=getle(cbw+4);bytes=getle(cbw+8);lun=cbw[13];
        CHECK(lun<=max_lun&&lun<2,"class never submits an unreported LUN");
        unsigned op=cbw[15];commands[op]++;status=residue=0;
        if(op==0 && unit_attention){unit_attention=0;status=1;sense_data[0]=0x70;sense_data[2]=6;sense_data[7]=10;sense_data[12]=0x29;}
        if(op==0x2a && write_fail){status=1;residue=bytes;sense_data[0]=0x70;sense_data[2]=7;sense_data[7]=10;sense_data[12]=0x27;}
        if(op==0x35)flushes++;
        phase=bytes?1:2;return 31;
    }
    if(phase==1) {
        phase=2;CHECK(n==bytes,"data transfer length matches CBW");
        uint8_t*p=data;unsigned op=cbw[15];
        if(ep&0x80)memset(p,0,n);
        if(op==0x12){p[0]=0;p[1]=0x80;p[2]=5;p[3]=2;p[4]=31;memcpy(p+8,"USBTEST ",8);}
        else if(op==0x03)memcpy(p,sense_data,n);
        else if(op==0x25){be32(p,511);be32(p+4,capacity_4kn?4096:512);}
        else if(op==0x28 || op==0x2a) {
            uint32_t lba=usb_scsi_be32(cbw+17),count=(unsigned)cbw[22]<<8|cbw[23];
            CHECK(count*512==n&&lba+count<=512,"class READ/WRITE CDB bounds match data payload");
            if(status)return -1; /* data halt then REQUEST SENSE */
            if(op==0x28)memcpy(p,media[lun]+lba*512,n);else memcpy(media[lun]+lba*512,p,n);
            if(force_residue)residue=512;
        } else CHECK(0,"unexpected data command");
        return (int)n;
    }
    CHECK(ep==0x81&&n==13,"real class receives CSW on bulk IN");
    uint8_t*p=data;memset(p,0,13);putle(p,0x53425355);putle(p+4,tag);putle(p+8,residue);p[12]=(uint8_t)status;
    phase=0;return 13;
}
int main(void)
{
    struct usb_device d={.used=1};d.cfg.n_if=1;
    struct usb_interface *it=&d.cfg.iface[0];it->num=5;it->if_class=8;it->if_subclass=6;it->if_proto=0x50;it->n_ep=2;
    it->ep[0]=(struct usb_endpoint){.addr=0x81,.attr=2,.max_packet=512};
    it->ep[1]=(struct usb_endpoint){.addr=2,.attr=2,.max_packet=512};
    usb_msc_register();CHECK(registered&&!registered->poll,"BOT class performs no synchronous I/O from USB ISR polling");
    it->if_proto=0x62;CHECK(registered->probe(&d,0)<0&&!bulk_calls,"UAS is refused before sending BOT bytes");
    it->if_proto=0x50;it->ep[1].addr=0x82;
    CHECK(registered->probe(&d,0)<0&&!bulk_calls,"missing bulk OUT cannot bind");it->ep[1].addr=2;
    CHECK(!registered->probe(&d,0)&&ndisks==2,"both reported LUNs become distinct block devices");
    CHECK(part_scans==2&&disks[0].nsectors==512&&!strcmp(disks[0].name,"usb0"),"capacity and late partition discovery reach block registry");
    CHECK(commands[3]==1&&commands[0]==3&&delays==100,"UNIT ATTENTION fetches sense and retries only discovery");
    unsigned char data[130*512],back[130*512];
    for(unsigned j=0;j<sizeof data;j++)data[j]=(unsigned char)((j*37u)^(j>>8)^0x5a);
    unsigned nwrite=commands[0x2a];
    CHECK(!disks[0].ops->write(disks[0].ctx,17,130,data)&&commands[0x2a]==nwrite+3,"130 sectors split into 64/64/2 real WRITE10 commands");
    memset(back,0,sizeof back);
    CHECK(!disks[0].ops->read(disks[0].ctx,17,130,back)&&!memcmp(data,back,sizeof data),"multi-command data bytes roundtrip exactly");
    CHECK(media[0][16*512]==0&&media[0][147*512]==0&&media[1][17*512]==0,"write preserves adjacent sectors and another LUN");
    CHECK(!disks[0].ops->flush(disks[0].ctx)&&flushes==1,"block flush issues SYNCHRONIZE CACHE rather than success stub");
    unsigned before=bulk_calls;
    CHECK(disks[0].ops->read(disks[0].ctx,511,2,back)<0&&bulk_calls==before,"out-of-range block access sends no USB command");
    force_residue=1;
    CHECK(disks[0].ops->read(disks[0].ctx,17,1,back)<0,"block API rejects command-passed CSW with incomplete residue");force_residue=0;
    write_fail=1;nwrite=commands[0x2a];
    CHECK(disks[0].ops->write(disks[0].ctx,17,1,data)<0&&commands[0x2a]==nwrite+1&&commands[3]==2,"failed WRITE fetches sense and never blindly retries");write_fail=0;
    registered->remove(&d,0);before=bulk_calls;
    CHECK(offline_count==2&&!d.binding[0].drvdata,"removal offlines both media and clears USB binding");
    CHECK(disks[0].ops->read(disks[0].ctx,17,1,back)<0&&bulk_calls==before,"stale block context cannot access a removed USB device");
    capacity_4kn=1;max_lun=0;
    CHECK(registered->probe(&d,0)<0&&ndisks==2,"native 4Kn media is refused before block publication");
    capacity_4kn=0;unit_attention=1;delay_error=-1;before=commands[0];
    CHECK(registered->probe(&d,0)<0&&ndisks==2&&commands[0]==before+1,
          "failed discovery delay does not pretend to back off or retry");
    printf("usb-storage: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
