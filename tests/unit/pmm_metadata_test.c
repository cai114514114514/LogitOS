/* Use the existing PMM hardware leaves, but exercise a different production
 * entry condition: fragmented UEFI low-memory descriptors and live boot info.
 * No metadata-placement helper is duplicated in this fixture. */
#define main existing_physmap_gate
#include "physmap_test.c"
#undef main

static void boot_layout(int hole, int split, uint64_t info_at)
{
    mm_host_kend = 0x200000;
    mm_host_cr3 = ROOT;
    memset(mm_p2v(ROOT), 0, 4096);
    unsigned char *info = mm_p2v(info_at);
    memset(info, 0, 4096);
    struct mtag { uint32_t type, size, stride, version; } *mt = (void *)(info + 8);
    struct entry *e = (void *)(mt + 1);
    int n = 0;
    if (split) {
        /* No single available descriptor can fit the ~3.4MiB metadata, though
         * the union is contiguous. Reverse order defeats adjacency assumptions. */
        for (int i = 31; i >= 0; i--)
            e[n++] = (struct entry){(uint64_t)i * 0x200000, 0x200000, 1, 0};
    } else if (hole) {
        e[n++] = (struct entry){0, 0x200000, 1, 0};
        e[n++] = (struct entry){0x200000, 0x600000, 2, 0};
        e[n++] = (struct entry){0x800000, LOW_END - 0x800000, 1, 0};
        memset(mm_p2v(0x200000), 0x5a, 4096);
    } else e[n++] = (struct entry){0, LOW_END, 1, 0};
    e[n++] = (struct entry){HIGH_B, HIGH_B_SIZE, 1, 0};
    mt->type = 6; mt->size = sizeof(*mt) + n * sizeof(*e);
    mt->stride = sizeof(*e); mt->version = 0;
    uint32_t *end = (uint32_t *)((unsigned char *)mt + mt->size);
    end[0] = 0; end[1] = 8;
    ((uint32_t *)info)[0] = 8 + mt->size + 8;
    unsigned char preserved[2048];
    unsigned length = ((uint32_t *)info)[0];
    memcpy(preserved, info, length);
    pmm_init(info_at);
    check(pmm_physmap_ready(), split ? "metadata spans adjacent available descriptors" :
          hole ? "metadata relocates beyond firmware hole" : "metadata avoids live boot information");
    if (!pmm_physmap_ready()) return;
    check(!memcmp(preserved, info, length), "metadata never overwrites boot information");
    if (hole) {
        check(*(unsigned char *)mm_p2v(0x200000) == 0x5a, "reserved firmware hole canary unchanged");
        check(!pmm_is_ram(0x200000, 4096), "firmware hole is not published as RAM");
    }
    if (info_at == 0x300000)
        check(pmm_refcount(0x280000) == 0, "relocated metadata does not reserve intervening usable RAM");
    uint64_t far = pmm_alloc_any();
    check(far >= HIGH_B, "relocated metadata still manages high physical pages");
    if (far) pmm_free(far);
    check(pmm_audit() == 0, "relocated PMM accounting remains consistent");
}

int main(void)
{
    void *arena = mmap(NULL, ARENA_END, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) return 2;
    mm_host_base = (uint64_t)(uintptr_t)arena;
    boot_layout(0, 1, INFO);
    boot_layout(1, 0, INFO);
    boot_layout(0, 0, 0x300000);
    munmap(arena, ARENA_END);
    printf("PMM_METADATA: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
