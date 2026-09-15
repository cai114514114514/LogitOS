/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usb_storage.h"
#include "usb_bot.h"
#include "usb.h"
#include "usb_hc.h"
#include "../core/io_domain.h"
#include "blkdev.h"
#include "kprintf.h"
#include <stddef.h>

/* The exact interface triple matters: UAS (protocol 0x62) carries tagged SCSI
 * information units and cannot be driven by writing BOT CBWs to its endpoints.
 * No per-vendor guessing or alternate-setting change is performed here.
 * SCSI command layouts follow SPC/SBC: INQUIRY, REQUEST SENSE, TEST UNIT READY,
 * READ CAPACITY(10/16), READ/WRITE(10/16), SYNCHRONIZE CACHE(10), IMMED=0.
 * Non-512 logical blocks, optical devices and protection information are
 * refused before publication; BLK_SECTOR is a contract, not a conversion. */
#define MSC_INTERFACES 16
#define MSC_LUNS 16
#define MSC_CHUNK_SECTORS 64
struct msc_interface;
struct msc_lun {
    struct msc_interface *owner;
    struct blkdev *disk;
    uint64_t sectors;
    uint8_t lun, sense_key, asc, ascq;
};
struct msc_interface {
    struct usb_bot bot;
    struct io_domain gate;
    struct usb_device *usb;
    unsigned offline;
    struct msc_lun lun[MSC_LUNS];
};
/* Block registry entries are permanent and may be retained by stale callers.
 * Keep their contexts permanent too; never recycle one for a different stick.
 * Removal closes admission, drains each block gate, then clears the USB
 * pointer under the shared BOT gate. This also covers the gaps between bulk
 * transfers, when the HCD has no active transfer reference of its own. */
static struct msc_interface interfaces[MSC_INTERFACES];
static unsigned next_interface, next_disk;
static io_lock_t allocation_gate=IO_LOCK_INIT;

static int bulk(void *ctx,uint8_t ep,void *data,uint32_t len)
{
    struct msc_interface *s=ctx;
    if (__atomic_load_n(&s->offline,__ATOMIC_ACQUIRE) || !s->usb) return -1;
    return usb_bulk(s->usb,ep,data,len);
}
static int control(void *ctx,uint8_t rt,uint8_t req,uint16_t val,uint16_t idx,void *data,uint16_t len)
{
    struct msc_interface *s=ctx;
    if (__atomic_load_n(&s->offline,__ATOMIC_ACQUIRE) || !s->usb) return -1;
    return usb_control(s->usb,rt,req,val,idx,data,len);
}
static int clear_halt(void *ctx,uint8_t ep)
{
    struct msc_interface *s=ctx;
    if (__atomic_load_n(&s->offline,__ATOMIC_ACQUIRE) || !s->usb) return -1;
    return usb_clear_halt(s->usb,ep);
}

static int command(struct msc_lun *l,const uint8_t *cdb,uint8_t cdb_len,
                    void *data,uint32_t len,int in,uint32_t *done)
{
    struct msc_interface *s=l->owner;
    IO_DOMAIN_GUARD(&s->gate);
    if (__atomic_load_n(&s->offline,__ATOMIC_ACQUIRE) || !s->usb) return -1;
    l->sense_key=l->asc=l->ascq=0;
    int rc=usb_bot_exec(&s->bot,l->lun,cdb,cdb_len,data,len,in,done);
    if (rc==1) {
        uint8_t sense_cdb[6]={0x03,0,0,0,18,0},sense[18]={0};
        uint32_t n=0;
        if (!usb_bot_exec(&s->bot,l->lun,sense_cdb,6,sense,sizeof sense,1,&n))
            (void)usb_scsi_sense(sense,n,&l->sense_key,&l->asc,&l->ascq);
    }
    if (rc) kprintf("[usb-msc] LUN %u cmd %x failed transport=%d sense=%x/%x/%x\n",
                    l->lun,cdb[0],rc,l->sense_key,l->asc,l->ascq);
    return rc;
}

/* Only discovery commands are retried, and only for transient sense codes.
 * A WRITE failure can represent partial processing and is always returned to
 * the caller. Blind retry would turn an uncertain write into a false success. */
static int discover(struct msc_lun *l,const uint8_t *cdb,uint8_t size,
                     void *buf,uint32_t len,uint32_t *done)
{
    for (int attempt=0;attempt<5;attempt++) {
        int rc=command(l,cdb,size,buf,len,len!=0,done);
        if (!rc) return 0;
        if (rc<0 || (l->sense_key!=6 && !(l->sense_key==2 && l->asc==4))) return -1;
        if (attempt<4 && usb_delay_ms(100)) return -1;
    }
    return -1;
}

static int transfer(struct msc_lun *l,uint64_t lba,uint32_t count,void *buf,int write)
{
    if (!l || !buf || !count || lba>=l->sectors || count>l->sectors-lba) return -1;
    uint8_t *p=buf;
    while (count) {
        uint32_t chunk=count>MSC_CHUNK_SECTORS?MSC_CHUNK_SECTORS:count;
        uint8_t cdb[16];uint32_t done=0;
        int cdb_len=usb_scsi_rw_cdb(cdb,write,lba,chunk);
        if (cdb_len<0 || command(l,cdb,(uint8_t)cdb_len,p,chunk*BLK_SECTOR,!write,&done) ||
            done!=chunk*BLK_SECTOR) return -1;
        lba+=chunk;count-=chunk;p+=chunk*BLK_SECTOR;
    }
    return 0;
}
static int read_blocks(void *ctx,uint64_t lba,uint32_t n,void *buf)
{ return transfer(ctx,lba,n,buf,0); }
static int write_blocks(void *ctx,uint64_t lba,uint32_t n,const void *buf)
{ return transfer(ctx,lba,n,(void *)buf,1); }
static int flush(void *ctx)
{
    uint8_t cdb[10]={0x35};uint32_t done=0;
    /* IMMED stays clear: success means the device completed its cache sync.
     * Unsupported sync is reported as an error, never an invented barrier. */
    return command(ctx,cdb,sizeof cdb,NULL,0,0,&done) ? -1 : 0;
}
static const struct blk_ops ops={.read=read_blocks,.write=write_blocks,.flush=flush};

static int probe_lun(struct msc_lun *l)
{
    uint8_t data[36]={0},cdb[16]={0x12,0,0,0,36,0};uint32_t n=0;
    if (discover(l,cdb,6,data,36,&n) || n<36 || (data[0]&0xe0) || (data[0]&31)!=0) return -1;
    for (int i=0;i<16;i++) cdb[i]=0; /* TEST UNIT READY */
    if (discover(l,cdb,6,NULL,0,&n)) return -1;
    cdb[0]=0x25; /* READ CAPACITY(10) */
    if (discover(l,cdb,10,data,8,&n)) return -1;
    int cap=usb_scsi_capacity10(data,n,&l->sectors);
    if (cap==1) {
        for (int i=0;i<16;i++) cdb[i]=0;
        cdb[0]=0x9e;cdb[1]=0x10;cdb[13]=32;
        if (discover(l,cdb,16,data,32,&n)) return -1;
        cap=usb_scsi_capacity16(data,n,&l->sectors);
    }
    if (cap) {
        kprintf("[usb-msc] LUN %u: unsupported capacity/block format (512-byte logical blocks required)\n",l->lun);
        return -1;
    }
    unsigned number=__atomic_fetch_add(&next_disk,1,__ATOMIC_RELAXED);
    if (number>=256) return -1;
    char name[BLK_NAME_MAX]={'u','s','b',0};
    int pos=3;
    if (number>=100) name[pos++]=(char)('0'+number/100);
    if (number>=10) name[pos++]=(char)('0'+number/10%10);
    name[pos++]=(char)('0'+number%10);name[pos]=0;
    l->disk=blk_register(name,&ops,l,l->sectors);
    if (!l->disk) return -1;
    kprintf("[usb-msc] disk=%s LUN=%u sectors=%llu sector=512\n",name,l->lun,l->sectors);
    blk_probe_partitions(l->disk);
    return 0;
}

static int probe(struct usb_device *d,int ifno)
{
    if (!d || ifno<0 || ifno>=d->cfg.n_if) return -1;
    const struct usb_interface *it=&d->cfg.iface[ifno];
    if (it->if_class!=8 || it->if_subclass!=6 || it->if_proto!=0x50 || it->alt) return -1;
    const struct usb_endpoint *in=NULL,*out=NULL;
    for (int i=0;i<it->n_ep;i++) {
        const struct usb_endpoint *e=&it->ep[i];
        if (USB_EP_XFER(e->attr)!=USB_XFER_BULK) continue;
        if (!USB_EP_NUM(e->addr) || (e->addr&0x70) || !e->max_packet) return -1;
        if (USB_EP_IS_IN(e->addr)) { if (in) return -1;in=e; }
        else { if (out) return -1;out=e; }
    }
    if (!in || !out) return -1;
    struct msc_interface *s;
    {
        IO_GUARD(&allocation_gate);
        if (next_interface==MSC_INTERFACES) return -1;
        s=&interfaces[next_interface++];
    }
    s->usb=d;
    s->gate=(struct io_domain)IO_DOMAIN_INIT;
    s->bot=(struct usb_bot){.ctx=s,.bulk=bulk,.control=control,.clear_halt=clear_halt,
                            .in=in->addr,.out=out->addr,.interface=it->num};
    d->binding[ifno].drvdata=s;
    int max=usb_bot_max_lun(&s->bot),found=0;
    if (max>=0) for (int i=0;i<=max;i++) {
        s->lun[i].owner=s;s->lun[i].lun=(uint8_t)i;
        if (!probe_lun(&s->lun[i])) found++;
        if (s->bot.dead) break;
    }
    if (!found) {
        s->offline=1;s->usb=NULL;d->binding[ifno].drvdata=NULL;
        return -1;
    }
    kprintf("[usb-msc] interface=%u max-lun=%d online=%d\n",it->num,max,found);
    return 0;
}
static void remove_interface(struct usb_device *d,int ifno)
{
    struct msc_interface *s=d->binding[ifno].drvdata;
    if (!s) return;
    __atomic_store_n(&s->offline,1,__ATOMIC_RELEASE);
    /* Block submit takes medium gate BEFORE BOT gate. Do not reverse that
     * order here while another CPU owns a medium and awaits this transport. */
    for (int i=0;i<MSC_LUNS;i++) if (s->lun[i].disk) blk_dev_offline(s->lun[i].disk);
    { IO_DOMAIN_GUARD(&s->gate);s->usb=NULL; }
    d->binding[ifno].drvdata=NULL;
}
static const struct usb_match matches[]={{8,6,0x50},{0,0,0}};
static const struct usb_driver driver={.name="usb-storage",.match=matches,.probe=probe,.remove=remove_interface};
void usb_msc_register(void) { usb_register_driver(&driver); }
