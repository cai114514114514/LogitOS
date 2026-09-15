#include "amd/polaris/firmware/bundle.h"
#include "amd/polaris/firmware.h"

/* Fixed wire and consumer references:
 * linux v6.12 drivers/gpu/drm/amd/amdgpu/{amdgpu_ucode.h,amdgpu_cgs.c}
 * and pm/powerplay/smumgr/smu7_smumgr.c. In particular get_firmware_info()
 * truncates MEC's TOC image at jt_offset and VI maps both JT IDs to MEC1.
 * Normal PF retains payload digests; only the VF path strips 20 bytes. */
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static int span(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{ return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an; }
static uint64_t page_up(uint64_t n) { return (n + 4095u) & ~UINT64_C(4095); }

int polaris_fw_parse(enum polaris_fw_kind kind, const void *data, size_t bytes,
                     struct polaris_fw_view *out)
{
    if (!span(data, bytes) || !span(out, sizeof *out) ||
        overlaps(data, bytes, out, sizeof *out) || bytes < 32 ||
        kind < POLARIS_FW_SDMA0 || kind > POLARIS_FW_SMC) return -1;
    const uint8_t *p = data;
    struct polaris_fw_view v = {0};
    if (kind == POLARIS_FW_SDMA0 || kind == POLARIS_FW_SDMA1) {
        struct polaris_sdma_firmware fw;
        if (polaris_sdma_firmware_parse(data, bytes, &fw) ||
            fw.ip_major != 3 || fw.ip_minor != 1) return -1;
        v.ucode = fw.ucode; v.bytes = fw.ucode_bytes; v.version = fw.ucode_version;
        v.feature_version = fw.feature_version;
        v.jt_offset_dwords = fw.jump_offset_dwords; v.jt_size_dwords = fw.jump_size_dwords;
        *out = v; return 0;
    }
    uint32_t header = kind == POLARIS_FW_RLC ? 104u : kind == POLARIS_FW_SMC ? 36u : 44u;
    uint16_t major = kind == POLARIS_FW_RLC ? 2u : 1u;
    uint16_t ip = kind == POLARIS_FW_SMC ? 7u : 8u;
    uint16_t minor = kind == POLARIS_FW_SMC ? 2u : 0u;
    if (bytes < header || bytes != u32(p) || u32(p+4) != header ||
        u16(p+8) != major || u16(p+10) != 0 || u16(p+12) != ip || u16(p+14) != minor)
        return -1;
    uint32_t size = u32(p+20), offset = u32(p+24);
    if (!size || ((size | offset) & 3u) || offset < header ||
        offset > bytes || size > bytes - offset) return -1;
    v.ucode = p + offset; v.bytes = size; v.version = u32(p+16);
    if (kind == POLARIS_FW_SMC) {
        v.ucode_start_addr = u32(p+32);
        /* SMU7's boot SRAM occupies [0x20000,0x40000). Accepting a general
         * common header here would turn its start address into arbitrary MMIO. */
        if (v.ucode_start_addr != 0x20000u || size > 0x20000u) return -1;
    } else {
        v.feature_version = u32(p+32);
        v.jt_offset_dwords = u32(p+36); v.jt_size_dwords = u32(p+40);
        if (v.jt_offset_dwords > size/4 || v.jt_size_dwords > size/4-v.jt_offset_dwords ||
            (!v.jt_size_dwords && v.jt_offset_dwords) ||
            (kind == POLARIS_FW_MEC && (!v.jt_offset_dwords || !v.jt_size_dwords))) return -1;
    }
    if (kind == POLARIS_FW_RLC) {
        /* These are file-relative byte arrays OUTSIDE the main RLC payload.
         * Checking only ucode_size misses truncated register restore data. */
        for (unsigned i=0; i<4; ++i) {
            uint32_t n=u32(p+72+i*8), off=u32(p+76+i*8);
            if (!n) { if (off) return -1; continue; }
            if (((n|off)&3u) || off<header || off>bytes || n>bytes-off ||
                overlaps(p+off,n,v.ucode,size)) return -1;
            for (unsigned j=0; j<i; ++j)
                if (v.aux_bytes[j] && overlaps(p+off,n,v.aux[j],v.aux_bytes[j])) return -1;
            v.aux[i]=p+off; v.aux_bytes[i]=n;
        }
    }
    *out = v;
    return 0;
}

static int inputs(const struct polaris_fw_sources *s, struct polaris_fw_bundle_info *out,
                  struct polaris_fw_view view[POLARIS_FW_FILE_COUNT])
{
    if (!span(s,sizeof *s) || !span(out,sizeof *out) || overlaps(s,sizeof *s,out,sizeof *out)) return -1;
    for (unsigned i=0;i<POLARIS_FW_FILE_COUNT;++i) {
        if (!span(s->file[i].data,s->file[i].bytes) ||
            overlaps(out,sizeof *out,s->file[i].data,s->file[i].bytes) ||
            polaris_fw_parse((enum polaris_fw_kind)i,s->file[i].data,s->file[i].bytes,&view[i])) return -1;
    }
    return 0;
}

static int layout(uint64_t base, const struct polaris_fw_view *v,
                  struct polaris_fw_bundle_info *out)
{
    static const unsigned kind[9] = {POLARIS_FW_RLC,POLARIS_FW_CE,POLARIS_FW_PFP,
        POLARIS_FW_ME,POLARIS_FW_MEC,POLARIS_FW_MEC,POLARIS_FW_MEC,POLARIS_FW_SDMA0,POLARIS_FW_SDMA1};
    static const unsigned id[9] = {10,3,4,5,6,7,8,1,2};
    struct polaris_fw_bundle_info result = {.proposed_gpu_base=base};
    uint64_t at=4096;
    if ((base&4095u) || base >= POLARIS_SMU_TOC_GPU_LIMIT) return -1;
    for (unsigned i=0;i<9;++i) {
        const struct polaris_fw_view *fw=&v[kind[i]];
        uint32_t bytes=fw->bytes;
        if (i==4) bytes=fw->jt_offset_dwords*4;
        if (i==5 || i==6) bytes=fw->jt_size_dwords*4;
        if (!bytes || at > POLARIS_SMU_TOC_GPU_LIMIT-base ||
            page_up(bytes) > POLARIS_SMU_TOC_GPU_LIMIT-base-at) return -1;
        result.image_offset[i]=(size_t)at;
        result.image[i]=(struct polaris_smu_image){.id=id[i],.version=fw->version,
            .gpu_addr=base+at,.bytes=bytes,.flags=(i==0 || i==4)};
        /* Retain the complete MEC payload allocation, matching upstream's BO
         * layout; the TOC intentionally exposes only the pre-JT code range. */
        at+=page_up(i==4 ? fw->bytes : bytes);
    }
    if (at > SIZE_MAX || at > POLARIS_SMU_TOC_GPU_LIMIT-base) return -1;
    uint8_t toc[POLARIS_SMU_TOC_BYTES];
    if (polaris_smu_toc_encode(toc,sizeof toc,base,result.image,9,&result.inventory) ||
        result.inventory.missing_mask) return -1;
    result.bytes_used=(size_t)at; *out=result; return 0;
}

int polaris_fw_bundle_plan(const struct polaris_fw_sources *s, uint64_t base,
                           struct polaris_fw_bundle_info *out)
{
    struct polaris_fw_view view[POLARIS_FW_FILE_COUNT];
    if (inputs(s,out,view)) return -1;
    return layout(base,view,out);
}

int polaris_fw_bundle_stage(uint8_t *dst,size_t capacity,uint64_t base,
                            const struct polaris_fw_sources *s,struct polaris_fw_bundle_info *out)
{
    struct polaris_fw_view view[POLARIS_FW_FILE_COUNT];
    if (!span(dst,capacity) || inputs(s,out,view) || overlaps(dst,capacity,s,sizeof *s) ||
        overlaps(dst,capacity,out,sizeof *out)) return -1;
    for (unsigned i=0;i<POLARIS_FW_FILE_COUNT;++i)
        if (overlaps(dst,capacity,s->file[i].data,s->file[i].bytes)) return -1;
    struct polaris_fw_bundle_info result;
    if (layout(base,view,&result) || result.bytes_used>capacity) return -1;
    uint8_t toc[POLARIS_SMU_TOC_BYTES];
    struct polaris_smu_toc_info inventory;
    if (polaris_smu_toc_encode(toc,sizeof toc,base,result.image,9,&inventory)) return -1;
    /* All validation precedes the first byte written; caller can retain its
     * previous complete package after a malformed last source or small buffer. */
    for (size_t n=0;n<result.bytes_used;++n) dst[n]=0;
    for (size_t n=0;n<sizeof toc;++n) dst[n]=toc[n];
    static const unsigned kind[9]={6,2,3,4,5,5,5,0,1};
    for (unsigned i=0;i<9;++i) {
        const struct polaris_fw_view *fw=&view[kind[i]];
        const uint8_t *p=fw->ucode;
        uint32_t n=fw->bytes;
        if (i==5 || i==6) { p+=fw->jt_offset_dwords*4; n=fw->jt_size_dwords*4; }
        for (uint32_t j=0;j<n;++j) dst[result.image_offset[i]+j]=p[j];
    }
#ifdef POLARIS_FW_BUNDLE_NEGCTL_JT
    dst[result.image_offset[6]]^=1u; /* Independent JT2 oracle must catch this. */
#endif
    *out=result; return 0;
}
