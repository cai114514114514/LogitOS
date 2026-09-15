/* SPDX-License-Identifier: MIT
 * Scripted wire responses feed the production BOT engine, not a second BOT
 * implementation. The script captures CBW bytes and recovery request order. */
#include "usb_bot.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static int checks,failures;
#define CHECK(c,n) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",n);}}while(0)
static struct {
    uint8_t cbw[31],csw[13],data[64];
    int phase,cbw_len,data_len,csw_len,csw_failures,calls;
    int max_len,max_lun,reset_error,clear_error;
    char recovery[16];int recovery_n;
} wire;
static void le(uint8_t*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t getle(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int bulk(void *p,uint8_t ep,void *data,uint32_t n)
{
    (void)p;wire.calls++;
    if(wire.phase==0){CHECK(ep==2&&n==31,"CBW endpoint and exact 31-byte transfer");memcpy(wire.cbw,data,n);wire.phase=getle(wire.cbw+8)?1:2;return wire.cbw_len;}
    if(wire.phase==1){wire.phase=2;if(ep&0x80){if(wire.data_len>0)memcpy(data,wire.data,(unsigned)wire.data_len);}else if(n<=sizeof wire.data)memcpy(wire.data,data,n);return wire.data_len;}
    CHECK(ep==0x81&&n==13,"CSW endpoint and exact 13-byte request");
    if(wire.csw_failures){wire.csw_failures--;return -1;}
    if(wire.csw_len>0)memcpy(data,wire.csw,(unsigned)wire.csw_len);
    wire.phase=0;return wire.csw_len;
}
static int control(void*p,uint8_t rt,uint8_t req,uint16_t v,uint16_t i,void*d,uint16_t n)
{
    (void)p;CHECK(v==0&&i==7,"BOT control uses interface number and zero wValue");
    if(req==0xfe){CHECK(rt==0xa1&&n==1,"GET_MAX_LUN setup");*(uint8_t*)d=(uint8_t)wire.max_lun;return wire.max_len;}
    CHECK(rt==0x21&&req==0xff&&n==0,"Bulk-Only Reset setup");
    wire.recovery[wire.recovery_n++]='R';wire.phase=0;return wire.reset_error;
}
static int clear(void*p,uint8_t ep){(void)p;wire.recovery[wire.recovery_n++]=ep==0x81?'I':'O';return wire.clear_error;}
static struct usb_bot setup(uint32_t bytes)
{
    memset(&wire,0,sizeof wire);wire.cbw_len=31;wire.data_len=(int)bytes;wire.csw_len=13;wire.max_len=1;
    le(wire.csw,0x53425355);le(wire.csw+4,1);
    return (struct usb_bot){.bulk=bulk,.control=control,.clear_halt=clear,.in=0x81,.out=2,.interface=7};
}
static int exec(struct usb_bot*t,uint32_t bytes,int in,uint32_t*done)
{uint8_t cdb[10]={0x28},data[64]={0};return usb_bot_exec(t,3,cdb,10,data,bytes,in,done);}
static void invalid_status(int kind,const char *label)
{
    struct usb_bot t=setup(32);uint32_t done=99;
    if(kind==0)wire.csw[0]^=1;
    if(kind==1)wire.csw[4]^=1;
    if(kind==2){le(wire.csw+8,33);wire.csw[12]=1;}
    if(kind==3)wire.csw_len=12;
    if(kind==4)wire.csw[12]=2;
    if(kind==5)wire.csw[12]=3;
    if(kind==6)wire.data_len=16;
    CHECK(exec(&t,32,1,&done)<0&&done==0,label);
    CHECK(t.resets==1&&!t.dead&&memcmp(wire.recovery,"RIO",3)==0,"invalid CSW/phase performs ordered reset recovery");
}
int main(void)
{
    struct usb_bot t=setup(32);uint32_t done=0;
    CHECK(!exec(&t,32,1,&done)&&done==32,"valid IN command succeeds only after matching CSW");
    CHECK(getle(wire.cbw)==0x43425355&&getle(wire.cbw+4)==1&&getle(wire.cbw+8)==32,"CBW signature tag and byte count are little endian");
    CHECK(wire.cbw[12]==0x80&&wire.cbw[13]==3&&wire.cbw[14]==10&&wire.cbw[15]==0x28&&wire.cbw[30]==0,"CBW direction LUN CDB size and reserved padding");
    t=setup(32);CHECK(!exec(&t,32,0,&done)&&wire.cbw[12]==0,"OUT command uses outbound data direction");
    t=setup(0);CHECK(!exec(&t,0,0,&done)&&done==0&&wire.calls==2,"zero-data command sends only CBW and CSW");
    t=setup(32);wire.data_len=20;le(wire.csw+8,12);
    CHECK(!exec(&t,32,1,&done)&&done==20,"short IN with matching residue exposes only relevant bytes");
    t=setup(32);le(wire.csw+8,12);
    CHECK(!exec(&t,32,1,&done)&&done==20,"full USB data with padding honors CSW residue");
    t=setup(32);wire.csw[12]=1;le(wire.csw+8,32);
    CHECK(exec(&t,32,1,&done)==1&&!t.resets,"SCSI command failure remains distinct from transport failure");
    t=setup(32);wire.data_len=-1;wire.csw[12]=1;le(wire.csw+8,32);
    CHECK(exec(&t,32,1,&done)==1&&!t.resets&&wire.recovery[0]=='I',"data STALL clears endpoint then retrieves command-failed CSW");
    t=setup(32);wire.data_len=-1;
    CHECK(exec(&t,32,1,&done)<0&&t.resets==1,"unknown transfer progress cannot become successful full data");
    t=setup(0);wire.csw_failures=1;
    CHECK(!exec(&t,0,0,&done)&&wire.recovery_n==1&&wire.recovery[0]=='I',"single status STALL clears and retries CSW once");
    t=setup(0);wire.csw_failures=2;
    CHECK(exec(&t,0,0,&done)<0&&t.resets==1,"repeated status failure resets instead of unbounded retry");
    invalid_status(0,"reject wrong CSW signature");invalid_status(1,"reject stale CSW tag");
    invalid_status(2,"reject residue greater than requested data");invalid_status(3,"reject truncated CSW");
    invalid_status(4,"phase error requires reset");invalid_status(5,"reject reserved status");
    invalid_status(6,"reject fabricated full success after short data");
    t=setup(0);wire.cbw_len=30;
    CHECK(exec(&t,0,0,&done)<0&&t.resets==1&&wire.calls==1,"truncated CBW never advances to data or status");
    t=setup(0);wire.csw_len=0;wire.reset_error=-1;
    CHECK(exec(&t,0,0,&done)<0&&t.dead&&wire.recovery_n==3,"failed reset closes transport despite both halt-clear attempts");
    int calls=wire.calls;CHECK(exec(&t,0,0,&done)<0&&wire.calls==calls,"dead transport refuses subsequent CBW");
    t=setup(0);wire.max_lun=15;CHECK(usb_bot_max_lun(&t)==15,"GET_MAX_LUN supports all 16 BOT LUNs");
    wire.max_lun=16;CHECK(usb_bot_max_lun(&t)<0,"reject reserved GET_MAX_LUN bits");
    wire.max_len=0;CHECK(usb_bot_max_lun(&t)<0,"reject truncated GET_MAX_LUN");
    wire.max_len=-1;CHECK(usb_bot_max_lun(&t)==0,"single-LUN STALL fallback remains valid");
    uint8_t cdb[16],cap[32]={0},sense[18]={0},key,asc,ascq;uint64_t sectors=0;
    CHECK(usb_scsi_rw_cdb(cdb,0,0x12345678,0x3456)==10&&cdb[0]==0x28&&usb_scsi_be32(cdb+2)==0x12345678&&cdb[7]==0x34&&cdb[8]==0x56,"READ10 encodes big-endian LBA and transfer count");
    CHECK(usb_scsi_rw_cdb(cdb,1,UINT64_C(0x123456789a),64)==16&&cdb[0]==0x8a&&usb_scsi_be64(cdb+2)==UINT64_C(0x123456789a)&&usb_scsi_be32(cdb+10)==64,"WRITE16 preserves wide LBA");
    CHECK(usb_scsi_rw_cdb(cdb,0,UINT32_MAX,2)==16,"crossing 32-bit final LBA selects READ16");
    CHECK(usb_scsi_rw_cdb(cdb,0,0,65536)==16,"65536-block request never truncates 10-byte count");
    CHECK(usb_scsi_rw_cdb(cdb,0,0,0)<0&&usb_scsi_rw_cdb(cdb,0,UINT64_MAX,2)<0,"reject zero count and LBA overflow");
    cap[3]=15;cap[6]=2;
    CHECK(!usb_scsi_capacity10(cap,8,&sectors)&&sectors==16,"CAPACITY10 uses last-LBA plus one and 512-byte blocks");
    cap[6]=0x10;CHECK(usb_scsi_capacity10(cap,8,&sectors)<0,"refuse native 4Kn through 512-byte block API");
    cap[6]=2;memset(cap,0xff,4);CHECK(usb_scsi_capacity10(cap,8,&sectors)==1,"saturated CAPACITY10 requests CAPACITY16");
    memset(cap,0,32);cap[3]=1;cap[11]=0;cap[10]=2;
    CHECK(!usb_scsi_capacity16(cap,32,&sectors)&&sectors==UINT64_C(0x100000001),"CAPACITY16 retains more than 2TiB");
    cap[12]=1;CHECK(usb_scsi_capacity16(cap,32,&sectors)<0,"refuse SCSI protection-information format");
    cap[12]=0;memset(cap,0xff,8);CHECK(usb_scsi_capacity16(cap,32,&sectors)<0,"refuse 64-bit capacity overflow");
    CHECK(usb_scsi_capacity10(cap,7,&sectors)<0&&usb_scsi_capacity16(cap,31,&sectors)<0,"reject truncated capacities");
    sense[0]=0x70;sense[2]=6;sense[7]=10;sense[12]=0x29;
    CHECK(!usb_scsi_sense(sense,18,&key,&asc,&ascq)&&key==6&&asc==0x29&&ascq==0,"fixed sense decodes unit attention");
    CHECK(usb_scsi_sense(sense,13,&key,&asc,&ascq)<0,"short fixed sense does not read missing ASC");
    sense[0]=0x72;sense[1]=2;sense[2]=4;sense[3]=1;
    CHECK(!usb_scsi_sense(sense,8,&key,&asc,&ascq)&&key==2&&asc==4&&ascq==1,"descriptor sense decodes becoming-ready");
    printf("usb-bot: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
