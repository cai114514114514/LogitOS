/* Local Web Storage snapshots, version 1. All integers are little endian;
 * a CRC32 covers the header (except its CRC field) AND all payload bytes.
 * An update writes the other slot, reads it back byte-for-byte, then changes
 * the in-memory map. A torn/bad newer slot never destroys the previous slot.
 *
 * Do not replace this with temp+rename: LogitFS rename cannot replace a file.
 * Its whole-file writes already use a transaction and B1/B2/B3 barriers
 * (logitfs_write_locked/log_commit). bstore's old comment says success=0;
 * the shipping write_file actually returns len. Accept both contracts, but
 * never a short positive write, and verify what the backend actually stored.
 *
 * This is one browser-process writer, not interprocess synchronization.
 * read_file conflates missing files and read errors as -1, so two unreadable
 * slots cannot be distinguished from first launch by this interface. A
 * recognizable corrupt record DOES block local storage rather than replacing
 * it with an empty store. A failed write/readback is an uncertain disk commit:
 * preserve the old in-memory map and slot, report failure, and block further
 * writes until reopen. A filesystem can report an error after data reached
 * disk; we do not promise rollback of that external outcome. */
#include "tabs.h"
#include <stdint.h>
#define STORAGE_ORIGIN_LIMIT 2048u
#define STORAGE_SNAPSHOT_MAX (32u + STORAGE_AREA_LIMIT * (8u + STORAGE_ORIGIN_LIMIT + STORAGE_BYTE_LIMIT + 8u * STORAGE_ITEM_LIMIT))
static const char *storage_paths[2]={BROWSER_DIR "/storage.0",BROWSER_DIR "/storage.1"};
static const struct bstore_ops *storage_store;
static uint64_t storage_generation;
static int storage_active_slot=-1;
static int storage_disk_status;
static uint32_t storage_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t storage_u64(const unsigned char *p)
{ return storage_u32(p) | (uint64_t)storage_u32(p+4)<<32; }
static void storage_put32(unsigned char *p,uint32_t v)
{ for(int i=0;i<4;i++)p[i]=(unsigned char)(v>>(i*8)); }
static void storage_put64(unsigned char *p,uint64_t v)
{ storage_put32(p,(uint32_t)v);storage_put32(p+4,(uint32_t)(v>>32)); }
static uint32_t storage_crc(const unsigned char *p,size_t n)
{
    uint32_t crc=~0u;
    for(size_t i=0;i<n;i++) {
        unsigned byte=(i>=28 && i<32)?0:p[i];crc^=byte;
        for(int j=0;j<8;j++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static void storage_free_areas(struct storage_area *areas)
{ if(areas){for(int i=0;i<STORAGE_AREA_LIMIT;i++)storage_clear_area(&areas[i]);free(areas);} }
static int storage_decode(const unsigned char *p,size_t n,struct storage_area **out,uint64_t *gen)
{
    if(n<32 || memcmp(p,"LGTWS01",8) || storage_u32(p+8)!=1 || storage_u32(p+12)!=n ||
       storage_u32(p+24)>STORAGE_AREA_LIMIT || storage_crc(p,n)!=storage_u32(p+28) || !storage_u64(p+16))return STORAGE_CORRUPT;
    struct storage_area *areas=calloc(STORAGE_AREA_LIMIT,sizeof *areas);if(!areas)return STORAGE_NOMEM;
    size_t pos=32;unsigned count=storage_u32(p+24);int rc=STORAGE_CORRUPT;
    for(unsigned i=0;i<count;i++) {
        if(n-pos<8)goto fail;
        unsigned on=storage_u32(p+pos),items=storage_u32(p+pos+4);pos+=8;
        if(!on || on>STORAGE_ORIGIN_LIMIT || on>n-pos || items>STORAGE_ITEM_LIMIT || memchr(p+pos,0,on))goto fail;
        struct storage_area *a=&areas[i];a->kind=STORAGE_LOCAL;
        a->origin=storage_copy((const char *)p+pos,on);if(!a->origin){rc=STORAGE_NOMEM;goto fail;}pos+=on;
        for(unsigned j=0;j<i;j++)if(!strcmp(a->origin,areas[j].origin))goto fail;
        for(unsigned j=0;j<items;j++) {
            if(n-pos<8)goto fail;
            unsigned kn=storage_u32(p+pos),vn=storage_u32(p+pos+4);pos+=8;
            if(kn>STORAGE_BYTE_LIMIT || vn>STORAGE_BYTE_LIMIT-kn || a->bytes>STORAGE_BYTE_LIMIT-kn-vn || kn>n-pos || vn>n-pos-kn)goto fail;
            if(storage_index(a,(const char *)p+pos,kn)>=0)goto fail;
            struct storage_item *item=&a->items[a->count++];
            item->key=storage_copy((const char *)p+pos,kn);pos+=kn;
            item->value=storage_copy((const char *)p+pos,vn);pos+=vn;
            if(!item->key || !item->value){rc=STORAGE_NOMEM;goto fail;}
            item->key_len=kn;item->value_len=vn;a->bytes+=kn+vn;
        }
    }
    if(pos!=n)goto fail;
    *gen=storage_u64(p+16);*out=areas;return STORAGE_OK;
fail:storage_free_areas(areas);return rc;
}
static int storage_read_slot(int slot,struct storage_area **out,uint64_t *gen)
{
    /* Correction after the first guest reopen failed (2026-09-09): the old
     * code read a 32-byte header, then allocated its declared size. LogitFS
     * inode_read is ALL OR NOTHING: max < file size returns -1, so every real
     * snapshot looked absent. Host fread-style prefix reads concealed it.
     * bstore has no stat/pread, therefore read once with the bounded maximum;
     * this temporary buffer is freed before the next slot is inspected.
     * The host adapters now enforce the same whole-file contract, and the
     * prefix-read negative control must fail cross-process restoration. */
#ifdef STORAGE_PERSIST_PREFIX_READ
    size_t capacity=32;
#else
    size_t capacity=STORAGE_SNAPSHOT_MAX+1u;
#endif
    unsigned char *p=malloc(capacity);if(!p)return STORAGE_NOMEM;
    int got=storage_store->read(storage_paths[slot],p,(int)capacity);
    int rc;
    if(got<0)rc=STORAGE_MISSING;
    else if(got<32 || (size_t)got>STORAGE_SNAPSHOT_MAX)rc=STORAGE_CORRUPT;
    else rc=storage_decode(p,(size_t)got,out,gen);
    free(p);return rc;
}
int storage_backend_set_store(const struct bstore_ops *ops)
{
    /* Configure only before creating realms. Reconfigure is a simulated
     * browser reopen for tests: both live maps are released; only local areas
     * are reconstructed. Session data must never appear in these files. */
    for(int i=0;i<STORAGE_AREA_LIMIT;i++)storage_clear_area(&storage_areas[i]);
    storage_store=ops;storage_generation=0;storage_active_slot=-1;storage_disk_status=STORAGE_OK;
    if(!ops)return STORAGE_OK;
    if(!ops->read || !ops->write){storage_disk_status=STORAGE_IO;return storage_disk_status;}
    struct storage_area *a[2]={NULL,NULL};uint64_t gen[2]={0,0};
    int status[2]={storage_read_slot(0,&a[0],&gen[0]),storage_read_slot(1,&a[1],&gen[1])};
    int pick=status[0]==STORAGE_OK?0:-1;
    if(status[1]==STORAGE_OK && (pick<0 || gen[1]>gen[0]))pick=1;
    if(status[0]==STORAGE_NOMEM || status[1]==STORAGE_NOMEM)storage_disk_status=STORAGE_NOMEM;
    else if(pick>=0) {
        memcpy(storage_areas,a[pick],sizeof storage_areas);free(a[pick]);a[pick]=NULL;
        storage_generation=gen[pick];storage_active_slot=pick;
    }
    else if(status[0]!=STORAGE_MISSING || status[1]!=STORAGE_MISSING)storage_disk_status=STORAGE_CORRUPT;
    storage_free_areas(a[0]);storage_free_areas(a[1]);return storage_disk_status;
}
int storage_backend_status(const struct storage_key *key)
{ return key && key->kind==STORAGE_LOCAL?storage_disk_status:STORAGE_OK; }
static int storage_persist_candidate(struct storage_area *replace,const struct storage_area *candidate)
{
    if(!storage_store)return STORAGE_OK;
    if(storage_disk_status!=STORAGE_OK)return storage_disk_status;
    if(storage_generation==UINT64_MAX)return STORAGE_IO;
    size_t size=32;unsigned count=0;
    for(int i=0;i<STORAGE_AREA_LIMIT;i++) {
        const struct storage_area *a=&storage_areas[i];if(a==replace)a=candidate;
        if(!a || !a->origin || a->kind!=STORAGE_LOCAL)continue;
        size_t on=strlen(a->origin);if(on>STORAGE_ORIGIN_LIMIT)return STORAGE_QUOTA;
        size+=8+on+a->bytes+8u*a->count;count++;
    }
    if(size>STORAGE_SNAPSHOT_MAX)return STORAGE_QUOTA;
    unsigned char *p=malloc(size),*readback=malloc(size+1);
    if(!p || !readback){free(p);free(readback);return STORAGE_NOMEM;}
    memset(p,0,32);memcpy(p,"LGTWS01",8);storage_put32(p+8,1);storage_put32(p+12,(uint32_t)size);
    storage_put64(p+16,storage_generation+1);storage_put32(p+24,count);size_t pos=32;
    for(int i=0;i<STORAGE_AREA_LIMIT;i++) {
        const struct storage_area *a=&storage_areas[i];if(a==replace)a=candidate;
        if(!a || !a->origin || a->kind!=STORAGE_LOCAL)continue;
        size_t on=strlen(a->origin);storage_put32(p+pos,(uint32_t)on);storage_put32(p+pos+4,a->count);pos+=8;
        memcpy(p+pos,a->origin,on);pos+=on;
        for(unsigned j=0;j<a->count;j++) {
            const struct storage_item *item=&a->items[j];
            storage_put32(p+pos,(uint32_t)item->key_len);storage_put32(p+pos+4,(uint32_t)item->value_len);pos+=8;
            memcpy(p+pos,item->key,item->key_len);pos+=item->key_len;memcpy(p+pos,item->value,item->value_len);pos+=item->value_len;
        }
    }
    storage_put32(p+28,storage_crc(p,size));int slot=storage_active_slot==0?1:0;
    if(storage_store->mkdir)storage_store->mkdir(BROWSER_DIR);
#ifdef STORAGE_PERSIST_NO_WRITE
    int rc=STORAGE_OK; /* Negative control: the former memory-only success. */
#else
    int wrote=storage_store->write(storage_paths[slot],p,(int)size);
    int rc=(wrote==0 || wrote==(int)size)?STORAGE_OK:STORAGE_IO;
    if(rc==STORAGE_OK) {
        int got=storage_store->read(storage_paths[slot],readback,(int)size+1);
        if(got!=(int)size || memcmp(p,readback,size))rc=STORAGE_IO;
    }
#endif
    free(p);free(readback);
    if(rc==STORAGE_OK){storage_generation++;storage_active_slot=slot;}
    else storage_disk_status=rc;
    return rc;
}
