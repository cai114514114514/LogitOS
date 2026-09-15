#include "amd/polaris/smu/loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
struct fake {
    uint32_t ram[0x10000], index, clock, reset, pc, events, mode, status;
    uint32_t response, response_high, argument, msg[8], arg[8], count, uploads, writes, io;
    unsigned authenticated, cleared, syncs, partial, noack, stale, nopass;
    unsigned noevents, noflags, fail_sync, map_short, bad_offset, time_step, fail_io;
    uint64_t now;
    uint8_t toc[4096], scratch[819200], image[9][16];
};
static int identity(void *p, uint32_t *v) { (void)p; *v = 0x67df1002; return 0; }
static uint32_t *ind(struct fake *f)
{
    switch (f->index) {
    case 0x80000000: return &f->reset;
    case 0x80000004: return &f->clock;
    case 0x80000370: return &f->pc;
    case 0xc0000004: return &f->events;
    case 0xe0003088: return &f->status;
    case 0xe00030a4: return &f->mode;
    default: if (f->index < 0x40000 && !(f->index & 3)) return &f->ram[f->index / 4];
    }
    return 0;
}
static int rd(void *p, uint32_t offset, uint32_t *v)
{
    struct fake *f = p; ++f->io;
    if (f->fail_io && f->io == f->fail_io) return -1;
    switch (offset) {
    case 0x248: *v = 0; return 0;
    case 0x254: *v = f->response | f->response_high; return 0;
    case 0x6b4: { uint32_t *a = ind(f); if (a) { *v = *a; return 0; } break; }
    }
    f->bad_offset = 1; return -1;
}
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int wr(void *p, uint32_t offset, uint32_t value)
{
    struct fake *f = p; ++f->io;
    if (f->fail_io && f->io == f->fail_io) return -1;
    ++f->writes;
    switch (offset) {
    case 0x6b0: f->index = value; return 0;
    case 0x6b4: {
        uint32_t *a = ind(f); if (!a) break;
        if (f->index == 0x80000000 && (*a & 1) && !(value & 1)) {
            if (f->uploads != 4) return -1;
            if (f->mode & 0x10000) {
                if (!f->noevents) f->events |= 0x10000;
                if (f->authenticated && !f->noflags) f->ram[0x3f000/4] = 1;
            } else if (!f->noflags) f->ram[0x3f000/4] = 1;
            f->pc = 0x20100;
        }
        if (f->index >= 0x20000 && f->index < 0x20010) {
            if (!(f->reset & 1)) return -1;
            ++f->uploads;
        }
        if (f->index == 0x3006c && value == 0) f->cleared = 1;
        *a = value; return 0;
    }
    case 0x254: if (!f->stale) f->response = value; return 0;
    case 0x290: f->argument = value; return 0;
    case 0x250:
        if (f->response || f->count >= 8) return -1;
        f->msg[f->count] = value; f->arg[f->count++] = f->argument;
        if (value == 0x100) {
            if (!(f->mode & 0x10000) || f->argument != 0x20000) return -1;
            f->status = f->nopass ? 1 : 3; f->authenticated = !f->nopass;
        } else if (value == 0x254) {
            /* The fake consumes the actual uploaded directory, not a Boolean
             * supplied by the driver. Literal wire fields independently prove
             * that addresses/flags came through a complete mapped TOC. */
            if (!f->cleared || f->syncs != 11 || f->argument != 0x47e ||
                le32(f->toc) != 1 || le32(f->toc + 4) != 9) return -1;
            uint32_t mask = 0;
            for (unsigned i = 0; i < 9; ++i) {
                uint8_t *e = f->toc + 8 + 28 * i;
                if (le32(e + 4) != 8 || le32(e + 8) != 0x300000 + i * 4096 ||
                    le32(e + 20) != 16) return -1;
                mask |= 1u << (e[0] | (unsigned)e[1] << 8);
            }
            if (mask != 0x5fe) return -1;
            f->ram[0x3006c/4] = f->partial == 2 ? UINT32_MAX : f->partial ? 6 : 0x47e;
        }
        if (!f->noack) f->response = 1;
        return 0;
    }
    f->bad_offset = 1; return -1;
}
static int map(void *p, uint64_t gpu, uint64_t n, volatile uint8_t **cpu)
{
    struct fake *f = p;
    if (gpu == UINT64_C(0x800100000) && n == 4096) *cpu = f->toc;
    else if (gpu == UINT64_C(0x800200000) && n == 819200 && !f->map_short) *cpu = f->scratch;
    else if (gpu >= UINT64_C(0x800300000) && gpu < UINT64_C(0x800309000) &&
             !(gpu & 4095) && n == 16) *cpu = f->image[(gpu - UINT64_C(0x800300000)) / 4096];
    else return -1;
    return 0;
}
static int sync_dev(void *p, uint64_t gpu, uint64_t n)
{
    struct fake *f = p; volatile uint8_t *cpu;
    if (f->fail_sync || map(p, gpu, n, &cpu)) return -1;
    ++f->syncs; return 0;
}
static uint64_t now(void *p) { struct fake *f = p; f->now += f->time_step; return f->now; }
static struct polaris_smu_loader_ops ops(struct fake *f)
{
    struct polaris_smu_loader_ops o = {f,identity,rd,wr,map,sync_dev,now}; return o;
}
static void init(struct fake *f)
{
    memset(f, 0, sizeof(*f)); f->mode = 0x20000;
    f->events = 0x80; f->pc = 0x20100; f->response = 1;
    f->ram[0x3f000/4] = 1; f->ram[0x20030/4] = 0x30000;
    memset(f->scratch, 0xa6, sizeof(f->scratch));
}
int main(void)
{
    struct fake *f = malloc(sizeof(*f));
    if (!f) return 1;
    const uint8_t firmware[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    struct polaris_fw_view fw = {.ucode=firmware,.bytes=16,.ucode_start_addr=0x20000};
    struct polaris_smu_image images[9];
    const uint32_t ids[] = {1,2,3,4,5,6,10,7,8};
    for (unsigned i = 0; i < 9; ++i) images[i] = (struct polaris_smu_image){ids[i],1,UINT64_C(0x800300000)+i*4096,16,ids[i]==6||ids[i]==10};
    struct polaris_smu_load_request r = {&fw,1,UINT64_C(0x800100000),UINT64_C(0x800200000),images,9};
    struct polaris_smu_loader c = {0};
    init(f); struct polaris_smu_loader_ops o = ops(f);
    CHECK(polaris_smu_load(&c,&o,&r) == 0);
    CHECK(c.loaded && !c.quarantined && !c.started && c.messages == 5);
    CHECK(c.load_status == 0x47e && f->uploads == 0 && f->bad_offset == 0);
    const uint32_t msg[] = {0x252,0x253,0x250,0x251,0x254};
    const uint32_t arg[] = {8,0x200000,8,0x100000,0x47e};
    CHECK(memcmp(f->msg,msg,sizeof(msg)) == 0 && memcmp(f->arg,arg,sizeof(arg)) == 0);
    CHECK(f->scratch[0] == 0 && f->scratch[819199] == 0 && f->toc[4095] == 0);
    unsigned oldio = f->io;
    CHECK(polaris_smu_load(&c,&o,&r) == -1 && f->io == oldio);
    /* Reserved high bits neither complete a pending command nor invalidate
     * a fresh low-16 ACK. Raw all-ones is still a transport disappearance. */
    init(f); memset(&c,0,sizeof(c)); f->response_high=0xbeef0000;
    CHECK(polaris_smu_load(&c,&o,&r) == 0);
    CHECK(c.loaded && !c.quarantined && c.messages == 5);
    init(f); memset(&c,0,sizeof(c)); f->response_high=0xabcd0000; f->noack=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT && c.quarantined && f->count == 1);
    init(f); memset(&c,0,sizeof(c)); f->response_high=0xabcd0000; f->stale=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_REJECTED && f->count == 0);
    init(f); memset(&c,0,sizeof(c)); f->response_high=0xabcd0000; f->response=0xfe;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_REJECTED && f->count == 0);
    init(f); memset(&c,0,sizeof(c)); f->response=UINT32_MAX;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_IO && f->count == 0);
    for (unsigned protected_mode = 0; protected_mode < 2; ++protected_mode) {
        init(f); memset(&c,0,sizeof(c)); f->clock=1; f->pc=0; f->ram[0x3f000/4]=0;
        f->mode |= protected_mode << 16;
        CHECK(polaris_smu_load(&c,&o,&r) == 0);
        CHECK(c.started && c.loaded && c.uploaded_dwords == 4 && c.protected_mode == protected_mode);
        CHECK(f->ram[0x20000/4] == 0x04030201 && f->ram[0x2000c/4] == 0x100f0e0d);
        CHECK(protected_mode ? f->authenticated && f->msg[0] == 0x100 : f->ram[0] == 0xe0008040);
    }
    init(f); memset(&c,0,sizeof(c)); f->partial=1;
    /* NEGCTL removes only SRAM completion wait: mailbox ACK alone must fail. */
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT);
    CHECK(c.quarantined && !c.loaded && c.load_status == 6);
    oldio = f->io;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_QUARANTINED && f->io == oldio);
    init(f); memset(&c,0,sizeof(c)); f->noack=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT && c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->stale=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_REJECTED && f->count == 0);
    init(f); memset(&c,0,sizeof(c)); f->mode=0;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_UNSUPPORTED && f->uploads == 0 && !c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->mode=0xffffffff;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_IO && !c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->map_short=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_INVALID && f->io == 0);
    init(f); memset(&c,0,sizeof(c)); f->fail_sync=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_IO && f->count == 0);
    init(f); memset(&c,0,sizeof(c)); f->ram[0x20030/4]=0x3ffff;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_UNSUPPORTED && f->count == 0);
    init(f); memset(&c,0,sizeof(c)); f->clock=1; f->pc=0; f->mode=0x30000; f->nopass=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_REJECTED && c.quarantined && !c.loaded);
    init(f); memset(&c,0,sizeof(c)); f->clock=1; f->pc=0; f->events=0;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT && !c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->clock=1; f->pc=0; f->noflags=1; f->ram[0x3f000/4]=0;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT && c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->ram[0x3f000/4]=0;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT && !c.quarantined && f->uploads == 0);
    init(f); memset(&c,0,sizeof(c)); f->pc=0x80000;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_UNSUPPORTED && !c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->mode|=0x10;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_UNSUPPORTED && !c.quarantined);
    init(f); memset(&c,0,sizeof(c)); f->response=0xfe;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_REJECTED && f->count == 0);
    init(f); memset(&c,0,sizeof(c)); f->partial=2;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_IO && c.quarantined && !c.loaded);
    init(f); memset(&c,0,sizeof(c)); f->mode=0; r.smc_security_key=0;
    CHECK(polaris_smu_load(&c,&o,&r) == 0 && !c.security_key);
    init(f); memset(&c,0,sizeof(c)); f->mode=0x10000; f->clock=1; f->pc=0;
    CHECK(polaris_smu_load(&c,&o,&r) == 0 && c.protected_mode && !c.security_key);
    r.smc_security_key=1;
    init(f); memset(&c,0,sizeof(c)); c.lock=1;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_BUSY && !f->io);
    init(f); memset(&c,0,sizeof(c)); r.image_count=6;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_INVALID && !f->io); r.image_count=9;
    init(f); memset(&c,0,sizeof(c)); r.scratch_gpu=r.toc_gpu;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_INVALID && !f->io); r.scratch_gpu=UINT64_C(0x800200000);
    init(f); memset(&c,0,sizeof(c)); f->time_step=100000;
    CHECK(polaris_smu_load(&c,&o,&r) == POLARIS_SMU_LOAD_TIMEOUT && !c.loaded);
    /* Every transport operation before a successful warm load is independently
     * failed. No injected read/write failure may produce loaded=1. */
    init(f); memset(&c,0,sizeof(c)); CHECK(polaris_smu_load(&c,&o,&r) == 0);
    unsigned totalio=f->io;
    for (unsigned i=1; i<=totalio; ++i) {
        init(f); memset(&c,0,sizeof(c)); f->fail_io=i;
        CHECK(polaris_smu_load(&c,&o,&r) != 0 && !c.loaded);
    }
    free(f);
    printf("POLARIS_SMU_LOADER: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
