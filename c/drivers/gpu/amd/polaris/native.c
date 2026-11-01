#include "amd/polaris/native.h"
#include "amd/polaris/pci_id.h"

/* Linux v6.12 gmc_v8_0.c + vi.c, oss_3_0_d.h and bif_5_0_d.h.
 * Offsets below are BYTES; upstream mm registers are DWORD indices. HDP base
 * is in 256-byte units, while MC_FB_LOCATION halves are in 16-MiB units.
 * Comparing GPU MC addresses directly to the CPU's PCI BAR silently points
 * DMA at a different address space even when both numerical values look sane. */
enum {
    MC_FB=0x2024, MC_SYSTEM_LOW=0x2034, MC_SYSTEM_HIGH=0x2038, MC_L1=0x2064,
    MC_BLACKOUT=0x20ac, HDP_BASE=0x2c04, HDP_INFO=0x2c08,
    HDP_MISC=0x2f4c, HDP_DEBUG=0x2f30, HDP_FLUSH=0x5480,
    BIF_FB=0x5490, MEMSIZE=0x5428
};
/* Register accesses that share one full PCI identity check. The check costs
 * about a dozen configuration reads (port I/O on this platform, microseconds
 * each); at this size an SMC firmware upload of 32768 words (65536 accesses,
 * smu/loader.c) pays for 64 checks instead of 65536, and a function that
 * vanishes or moves is still noticed within roughly a millisecond of MMIO
 * traffic. Linux does no per-access check at all. */
#define POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK 1024u
static int span(const void *p,uint64_t n)
{ return p && n && n<=UINTPTR_MAX-(uintptr_t)p; }
static int overlap(const void *a,uint64_t an,const void *b,uint64_t bn)
{ return (uintptr_t)a<(uintptr_t)b+bn && (uintptr_t)b<(uintptr_t)a+an; }
static int range(struct polaris_memory_range r)
{ return r.bytes && r.base<POLARIS_MEMORY_LIMIT && r.bytes<=POLARIS_MEMORY_LIMIT-r.base; }
static int inside(uint64_t a,uint64_t n,uint64_t base,uint64_t bytes)
{ return n && a>=base && a-base<bytes && n<=bytes-(a-base); }
static void barrier(void)
{
    /* This is a hardware fence, not merely a compiler barrier. The x86
     * cross-build was checked to emit locked OR instructions (an earlier
     * comment named MFENCE, which is another legal compiler lowering).
     * UC mappings have no dirty WB lines to evict; it orders CPU
     * device writes before HDP visibility and reads after invalidation. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static int pci(struct polaris_native *n,uint16_t at,uint32_t *v)
{ return n->resource.read_pci32(n->resource.opaque,at,v); }
static int identity(struct polaris_native *n,uint32_t *value)
{
    uint32_t id,command,header,classrev,bar0,bar1=0,bar5,caps;
    if (n->faulted || pci(n,0,&id) || !polaris10_identity_known(id) || pci(n,4,&command) ||
        (command&6u)!=6u || pci(n,0xc,&header) || ((header>>16)&0x7f) ||
        pci(n,8,&classrev) || (classrev>>24)!=3 ||
        pci(n,0x10,&bar0) || pci(n,0x24,&bar5) ||
        (bar0&1u) || ((bar0&6u)!=0 && (bar0&6u)!=4) || (bar5&7u)) return -1;
    if ((bar0&6u)==4 && pci(n,0x14,&bar1)) return -1;
    uint64_t phys0=((uint64_t)bar1<<32)|(bar0&~UINT32_C(15));
    if (phys0!=n->resource.bar0_physical ||
        (bar5&~UINT32_C(15))!=n->resource.bar5_physical) return -1;
    /* Capability walking is bounded and rejects loops/truncation rather than
     * treating a malformed PM chain as evidence that the device is in D0. */
    if (command&(1u<<20)) {
        if (pci(n,0x34,&caps)) return -1;
        uint32_t at=caps&255u;uint64_t visited=0;
        for (unsigned count=0;at;++count) {
            if (count>=48 || at<0x40 || at>0xfc || (at&3u) || (visited&(UINT64_C(1)<<(at/4)))) return -1;
            visited|=UINT64_C(1)<<(at/4);
            if (pci(n,(uint16_t)at,&caps)) return -1;
            if ((caps&255u)==1u) {
                uint32_t pmcsr;
                if (at>0xf8 || pci(n,(uint16_t)(at+4),&pmcsr) || (pmcsr&3u)) return -1;
            }
            at=(caps>>8)&255u;
        }
    }
    *value=id;return 0;
}
static uint32_t raw_read(struct polaris_native *n,uint32_t at)
{
    barrier();uint32_t value=*(volatile uint32_t *)(n->resource.bar5+at);barrier();return value;
}
static void raw_write(struct polaris_native *n,uint32_t at,uint32_t value)
{ barrier();*(volatile uint32_t *)(n->resource.bar5+at)=value;barrier(); }
static int check_mapping(struct polaris_native *n,int first)
{
    uint32_t id;
    if (identity(n,&id)) return -1;
    uint32_t location=raw_read(n,MC_FB),hdp_base=raw_read(n,HDP_BASE),info=raw_read(n,HDP_INFO);
    uint32_t misc=raw_read(n,HDP_MISC),mem=raw_read(n,MEMSIZE);
    uint32_t low=raw_read(n,MC_SYSTEM_LOW),high=raw_read(n,MC_SYSTEM_HIGH),mode=raw_read(n,MC_L1);
    uint64_t start=(uint64_t)(location&65535u)<<24;
    uint64_t end=((uint64_t)(location>>16)+1)<<24;
    if (end<=start || !mem || mem>65536 || ((uint64_t)mem<<20)!=end-start ||
        ((uint64_t)hdp_base<<8)!=start || info!=0x40000100u ||
        (misc&0x00c0001eu) || (raw_read(n,BIF_FB)&3u)!=3u ||
        (raw_read(n,MC_BLACKOUT)&7u) || n->resource.bar0_bytes>end-start) return -1;
    /* Correction: matching MC_FB/HDP and SDMA VMID0 alone did NOT establish
     * physical system access. GMC can route the same addresses through page
     * translation or unmapped handling. Linux gmc_v8_0_gart_enable requires
     * SYSTEM_ACCESS_MODE=3, ADVANCED_DRIVER_MODEL=1, UNMAPPED_ACCESS=0.
     * LOW/HIGH are 28-bit page numbers with an INCLUSIVE high page. Preserve
     * this already configured window; never rewrite a firmware-owned mapping.
     * Rechecking the whole VRAM span also protects later arena allocations. */
    if ((low|high)&0xf0000000u || high<low || (mode&0x78u)!=0x58u) return -1;
    uint64_t system_start=(uint64_t)low<<12;
    uint64_t system_end=((uint64_t)high+1u)<<12;
    if (!inside(start,end-start,system_start,system_end-system_start) ||
        !inside(n->resource.arena.base,n->resource.arena.bytes,system_start,system_end-system_start) ||
        !inside(n->resource.scanout.base,n->resource.scanout.bytes,system_start,system_end-system_start)) return -1;
    if (!first && (location!=n->location || hdp_base!=n->hdp_base || info!=n->hdp_info ||
                    (misc&0x00c0001eu)!=(n->hdp_misc&0x00c0001eu) ||
                    low!=n->system_low || high!=n->system_high || (mode&0x78u)!=n->l1_mode)) return -1;
    if (first) {n->location=location;n->hdp_base=hdp_base;n->hdp_info=info;n->hdp_misc=misc;
                n->vram_base=start;n->vram_bytes=end-start;
                n->system_low=low;n->system_high=high;n->l1_mode=mode&0x78u;}
    return 0;
}
/* Why a full check here and a credit elsewhere: the explicit identity request
 * is the phase boundary (the SMU loader and the SDMA engine ask once when they
 * start), so it always re-reads configuration space and then covers the
 * accesses that follow. */
static int read_identity(void *opaque,uint32_t *out)
{
    struct polaris_native *native=opaque;
    uint32_t identity_value;
    if (!native || !native->bound || !out) {
        return -1;
    }
    if (identity(native,&identity_value)) {
        native->faulted=1;
        return -1;
    }
    native->identity_credit=POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK;
    *out=identity_value;
    return 0;
}
/* Why a per-batch check: see POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK. The
 * access that finds the credit used up pays for the full check and starts the
 * next batch with itself already counted. A fault is latched, so a later
 * access never runs on a credit earned before it. */
static int register_access_allowed(struct polaris_native *native)
{
    uint32_t identity_value;
    if (native->faulted) {
        return -1;
    }
    if (native->identity_credit) {
        --native->identity_credit;
        return 0;
    }
    if (identity(native,&identity_value)) {
        native->faulted=1;
        return -1;
    }
    native->identity_credit=POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK-1u;
    return 0;
}
static int read_reg(void *opaque,uint32_t at,uint32_t *out)
{
    struct polaris_native *native=opaque;
    if (!native || !native->bound || !out || (at&3u) || at>native->resource.bar5_bytes-4) {
        return -1;
    }
    if (register_access_allowed(native)) {
        return -1;
    }
    *out=raw_read(native,at);
    return 0;
}
static int write_reg(void *opaque,uint32_t at,uint32_t value)
{
    struct polaris_native *native=opaque;
    if (!native || !native->bound || (at&3u) || at>native->resource.bar5_bytes-4) {
        return -1;
    }
    if (register_access_allowed(native)) {
        return -1;
    }
    raw_write(native,at,value);
    return 0;
}
static int resolve(void *opaque,uint64_t gpu,uint64_t bytes,volatile uint8_t **out)
{
    struct polaris_native *n=opaque;
    if (!n || !n->bound || !out || n->faulted) return -1;
    if (check_mapping(n,0)) {n->faulted=1;return -1;}
    /* check_mapping verified identity in full, which covers the accesses that
     * follow (see register_access_allowed). */
    n->identity_credit=POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK;
    if (!inside(gpu,bytes,n->resource.arena.base,n->resource.arena.bytes) &&
        !inside(gpu,bytes,n->resource.scanout.base,n->resource.scanout.bytes)) return -1;
    if (!inside(gpu,bytes,n->vram_base,n->resource.bar0_bytes)) return -1;
    *out=n->resource.bar0+(size_t)(gpu-n->vram_base);return 0;
}
static int sync_mapping(void *opaque,enum polaris_sdma_sync direction,uint64_t gpu,uint64_t bytes)
{
    struct polaris_native *n=opaque;volatile uint8_t *cpu;
    if ((direction!=POLARIS_SDMA_TO_DEVICE && direction!=POLARIS_SDMA_TO_CPU) || resolve(n,gpu,bytes,&cpu)) return -1;
    (void)cpu;
    /* vi_flush_hdp's CPU path: write 1 and read the same register to drain
     * posted PCI writes. GPU->CPU additionally drops HDP's stale read cache. */
    raw_write(n,HDP_FLUSH,1);
    if (raw_read(n,HDP_FLUSH)==UINT32_MAX) {n->faulted=1;return -1;}
    if (direction==POLARIS_SDMA_TO_CPU) {
#ifndef POLARIS_NATIVE_NEGCTL_NO_INVALIDATE
        raw_write(n,HDP_DEBUG,1);
        if (raw_read(n,HDP_DEBUG)==UINT32_MAX) {n->faulted=1;return -1;}
#endif
    }
    barrier();return 0;
}
static uint64_t now_us(void *opaque)
{ struct polaris_native *n=opaque;return n->resource.now_us(n->resource.opaque); }
int polaris_native_bind(struct polaris_native *n,const struct polaris_native_resources *r,
                         struct polaris_platform *out)
{
    if (!span(n,sizeof *n) || !span(r,sizeof *r) || !span(out,sizeof *out) ||
        overlap(n,sizeof *n,r,sizeof *r) || overlap(n,sizeof *n,out,sizeof *out) ||
        overlap(r,sizeof *r,out,sizeof *out) || n->bound || n->faulted ||
        !r->read_pci32 || !r->now_us || !span((const void *)r->bar0,r->bar0_bytes) ||
        !span((const void *)r->bar5,r->bar5_bytes) || ((uintptr_t)r->bar0&3u) || ((uintptr_t)r->bar5&3u) ||
        !r->bar0_physical || !r->bar5_physical || r->bar5_bytes<0x5494 ||
        r->bar0_bytes>UINT64_MAX-r->bar0_physical ||
        r->bar5_physical>UINT32_MAX || r->bar5_bytes>(UINT64_C(1)<<32)-r->bar5_physical ||
        (r->bar0_physical<r->bar5_physical+r->bar5_bytes &&
         r->bar5_physical<r->bar0_physical+r->bar0_bytes) ||
        (r->bar0_physical&15u) || (r->bar5_physical&15u) ||
        !range(r->arena) || !range(r->scanout) ||
        ((r->arena.base|r->arena.bytes)&4095u) || ((r->scanout.base|r->scanout.bytes)&3u) ||
        (r->arena.base<r->scanout.base+r->scanout.bytes && r->scanout.base<r->arena.base+r->arena.bytes) ||
        overlap((const void *)r->bar0,r->bar0_bytes,(const void *)r->bar5,r->bar5_bytes)) return -1;
    const void *objects[3]={n,r,out};size_t sizes[3]={sizeof *n,sizeof *r,sizeof *out};
    for(unsigned i=0;i<3;++i)
        if(overlap(objects[i],sizes[i],(const void *)r->bar0,r->bar0_bytes) ||
           overlap(objects[i],sizes[i],(const void *)r->bar5,r->bar5_bytes)) return -1;
    struct polaris_native candidate={.resource=*r,.bound=1};
    if (check_mapping(&candidate,1) ||
        !inside(r->arena.base,r->arena.bytes,candidate.vram_base,r->bar0_bytes) ||
        !inside(r->scanout.base,r->scanout.bytes,candidate.vram_base,r->bar0_bytes)) return -1;
    struct polaris_platform platform={.opaque=n,.read_identity=read_identity,.read_reg=read_reg,
        .write_reg=write_reg,.resolve_mapping=resolve,.sync=sync_mapping,.now_us=now_us};
    *n=candidate;*out=platform;return 0;
}
