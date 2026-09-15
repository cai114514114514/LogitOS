/* Real page-cache readahead with a deterministic allocator that returns the
 * final low frame and first high frame. MM_HOSTTEST's arena is linear, so the
 * backend independently rejects a buffer spanning the guest alias boundary. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "pcache.h"
#include "pmm.h"
#include "mmhost.h"

uint64_t mm_host_base, mm_host_kend, mm_host_cr3;
static int checks, failures, next_frame, backend_calls, crossed;
static const uint64_t sequence[] = {0x01000000, PMM_LOW_LIMIT, PMM_LOW_LIMIT - FRAME_SIZE};
static void check(int ok, const char *why)
{ checks++; if (!ok) { printf("FAIL: %s\n", why); failures++; } }
void kprintf(const char *fmt, ...) { (void)fmt; }
uint64_t pmm_alloc_contig(size_t n)
{ check(n * FRAME_SIZE < 0x00e00000, "cache metadata remains clear of fixture payload"); return 0x200000; }
uint64_t pmm_alloc_any(void)
{ return next_frame < 3 ? sequence[next_frame++] : 0; }
void pmm_free(uint64_t phys) { (void)phys; }
int pmm_ref(uint64_t phys) { (void)phys; return 0; }
unsigned pmm_refcount(uint64_t phys) { (void)phys; return 1; }
uint64_t pmm_free_frames(void) { return 4096; }
uint64_t reclaim_low(void) { return 0; }
static int fs_stat(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size)
{ (void)path; *dev = 1; *ino = 5; *size = 8 * FRAME_SIZE; return 0; }
static unsigned char pattern(uint64_t off)
{ return (unsigned char)((off / FRAME_SIZE) * 31 + off % 251 + 1); }
static long fs_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    (void)path;
    uint64_t phys = mm_v2p(dst);
    backend_calls++;
    if (phys < PMM_LOW_LIMIT && len > PMM_LOW_LIMIT - phys) {
        crossed++;
        return -1;       /* exactly the guest CPU alias discontinuity */
    }
    for (uint64_t i = 0; i < len; i++) ((unsigned char *)dst)[i] = pattern(off + i);
    return (long)len;
}
int main(void)
{
    uint64_t extent = PMM_LOW_LIMIT + 2 * FRAME_SIZE;
    void *arena = mmap(NULL, extent, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) { perror("alias arena"); return 2; }
    mm_host_base = (uint64_t)(uintptr_t)arena;
    const struct pcache_ops ops = {fs_stat, fs_read, NULL};
    pcache_init(extent / FRAME_SIZE);
    pcache_set_ops(&ops);
    int fh = pcache_file_open("/alias.dat");
    check(fh >= 0, "fixture file opens");
    uint64_t first = pcache_get(fh, 0);
    check(first == sequence[0], "first page seeds sequential read detection");
    uint64_t second = pcache_get(fh, 1);
    uint64_t third = pcache_get(fh, 2);
    check(crossed == 0, "backend buffer never crosses low/direct-map alias boundary");
    check(backend_calls == 3, "two physically adjacent alias zones require two backend reads");
    check(second == PMM_LOW_LIMIT - FRAME_SIZE, "sorted low page holds the second file page");
    check(third == PMM_LOW_LIMIT, "high page holds the third file page");
    if (second && third) {
        for (unsigned i = 0; i < FRAME_SIZE; i++) {
            check(((unsigned char *)mm_p2v(second))[i] == pattern(FRAME_SIZE + i), "low payload bytes match file offset");
            check(((unsigned char *)mm_p2v(third))[i] == pattern(2 * FRAME_SIZE + i), "high payload bytes match file offset");
        }
    }
    pcache_file_put(fh);
    munmap(arena, extent);
    printf("physmap consumers: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
