/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usb_bot.h"
#include <stddef.h>
#include <limits.h>

static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void putle32(uint8_t *p, uint32_t v)
{ for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
uint32_t usb_scsi_be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
uint64_t usb_scsi_be64(const uint8_t *p)
{ return (uint64_t)usb_scsi_be32(p)<<32 | usb_scsi_be32(p+4); }
static void putbe32(uint8_t *p, uint32_t v)
{ for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(24-8*i)); }
static void putbe64(uint8_t *p, uint64_t v)
{ putbe32(p,(uint32_t)(v>>32));putbe32(p+4,(uint32_t)v); }

int usb_bot_reset(struct usb_bot *t)
{
    /* BOT 5.3.4 prescribes this order. Attempt both clear-halt operations even
     * when the class request fails, but never authorize new CBWs on that basis. */
    t->resets++;
    int reset=t->control(t->ctx,0x21,0xff,0,t->interface,NULL,0);
    int in=t->clear_halt(t->ctx,t->in);
    int out=t->clear_halt(t->ctx,t->out);
    t->dead=reset!=0 || in!=0 || out!=0;
    return t->dead ? -1 : 0;
}

int usb_bot_max_lun(struct usb_bot *t)
{
    uint8_t max=0;
    int n=t->control(t->ctx,0xa1,0xfe,0,t->interface,&max,1);
    /* BOT 3.2 permits a STALL on single-LUN devices. The common USB API only
     * exposes -1, so this fallback is provisional: INQUIRY/capacity must still
     * succeed before any medium is published. EP0 recovery is the HCD's job. */
    if (n<0) return 0;
    return n==1 && max<=15 ? max : -1;
}

int usb_bot_exec(struct usb_bot *t, uint8_t lun, const uint8_t *cdb,
                 uint8_t cdb_len, void *data, uint32_t len, int in, uint32_t *done)
{
    if (done) *done=0;
    if (!t || !t->bulk || !t->control || !t->clear_halt || t->dead ||
        !cdb || !cdb_len || cdb_len>16 || lun>15 || len>INT_MAX || (len&&!data)) return -1;
    uint8_t cbw[31]={0}, csw[13]={0};
    uint32_t tag=++t->tag;
    if (!tag) tag=++t->tag;
    putle32(cbw,0x43425355);putle32(cbw+4,tag);putle32(cbw+8,len);
    cbw[12]=in?0x80:0;cbw[13]=lun;cbw[14]=cdb_len;
    for (unsigned i=0;i<cdb_len;i++) cbw[15+i]=cdb[i];
    if (t->bulk(t->ctx,t->out,cbw,sizeof cbw)!=(int)sizeof cbw) goto broken;

    int actual=0, data_error=0;
    if (len) {
        uint8_t ep=in?t->in:t->out;
        actual=t->bulk(t->ctx,ep,data,len);
        if (actual<0) {
            /* A data-stage STALL often represents a SCSI command failure.
             * Clear it and retrieve CSW, rather than losing REQUEST SENSE.
             * We do not know partial bytes on -1 and cannot claim success. */
            data_error=1;actual=0;
            if (t->clear_halt(t->ctx,ep)) goto broken;
        }
        if ((uint32_t)actual>len) goto broken;
    }
    int n=t->bulk(t->ctx,t->in,csw,sizeof csw);
    if (n<0) {
        /* The status phase permits exactly one clear-halt/read retry. A
         * second failure or malformed short CSW requires reset recovery. */
        if (t->clear_halt(t->ctx,t->in)) goto broken;
        n=t->bulk(t->ctx,t->in,csw,sizeof csw);
    }
    if (n!=(int)sizeof csw || le32(csw)!=0x53425355 || le32(csw+4)!=tag) goto broken;
    uint32_t residue=le32(csw+8);
    if (csw[12]>1 || residue>len) goto broken; /* status 2 = phase error */
    uint32_t relevant=len-residue;
    if (csw[12]==0 && (data_error || relevant>(uint32_t)actual)) goto broken;
    if (done) *done=relevant;
    return csw[12];
broken:
    (void)usb_bot_reset(t);
    return -1;
}

int usb_scsi_capacity10(const uint8_t *p, uint32_t n, uint64_t *sectors)
{
    if (!p || n!=8 || !sectors || usb_scsi_be32(p+4)!=512) return -1;
    uint32_t last=usb_scsi_be32(p);
    if (last==UINT32_MAX) return 1; /* READ CAPACITY(16), never wrap to zero */
    *sectors=(uint64_t)last+1;
    return 0;
}
int usb_scsi_capacity16(const uint8_t *p, uint32_t n, uint64_t *sectors)
{
    if (!p || n!=32 || !sectors || usb_scsi_be32(p+8)!=512 || (p[12]&1)) return -1;
    uint64_t last=usb_scsi_be64(p);
    if (last==UINT64_MAX) return -1;
    *sectors=last+1;
    return 0;
}
int usb_scsi_rw_cdb(uint8_t out[16], int write, uint64_t lba, uint32_t count)
{
    if (!out || !count || lba>UINT64_MAX-(count-1)) return -1;
    for (int i=0;i<16;i++) out[i]=0;
    if (count<=65535 && lba<=UINT32_MAX && count-1<=UINT32_MAX-lba) {
        out[0]=write?0x2a:0x28;putbe32(out+2,(uint32_t)lba);
        out[7]=(uint8_t)(count>>8);out[8]=(uint8_t)count;return 10;
    }
    out[0]=write?0x8a:0x88;putbe64(out+2,lba);putbe32(out+10,count);return 16;
}
int usb_scsi_sense(const uint8_t *p, uint32_t n, uint8_t *key, uint8_t *asc, uint8_t *ascq)
{
    if (!p || n<4 || !key || !asc || !ascq) return -1;
    uint8_t response=p[0]&0x7f;
    if (response==0x70 || response==0x71) {
        if (n<14 || p[7]<6) return -1;
        *key=p[2]&15;*asc=p[12];*ascq=p[13];return 0;
    }
    if (response==0x72 || response==0x73) {
        if (n<8) return -1;
        *key=p[1]&15;*asc=p[2];*ascq=p[3];return 0;
    }
    return -1;
}
