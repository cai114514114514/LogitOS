/* Host fixture for the exact UEFI destination validator used by loader.c. */
#include <stdio.h>
#include <stdlib.h>

#include "load_policy.h"

#define BASE 0x02000000ULL
#define STRIDE 48

static int checks;

#define CHECK(expr, label) do {                                             \
    checks++;                                                               \
    if (!(expr)) {                                                          \
        fprintf(stderr, "FAIL: %s (line %d)\n", (label), __LINE__);         \
        return 1;                                                           \
    }                                                                       \
} while (0)

union map_storage {
    UINT64 align;
    UINT8 bytes[4 * STRIDE];
};

static EFI_MEMORY_DESCRIPTOR *desc(union map_storage *m, UINTN i)
{
    return (EFI_MEMORY_DESCRIPTOR *)(void *)(m->bytes + i * STRIDE);
}

static void put(union map_storage *m, UINTN i, UINT32 type,
                UINT64 start, UINT64 pages)
{
    EFI_MEMORY_DESCRIPTOR *d = desc(m, i);
    *d = (EFI_MEMORY_DESCRIPTOR){0};
    d->Type = type;
    d->PhysicalStart = start;
    d->NumberOfPages = pages;
}

static int owns(union map_storage *m, UINTN n, UINT64 lo, UINT64 hi)
{
    return efi_load_range_is_loader_data(m->bytes, n * STRIDE, STRIDE, lo, hi);
}

int main(void)
{
    union map_storage m = {0};

    put(&m, 0, EfiLoaderData, BASE, 3);
    CHECK(owns(&m, 1, BASE, BASE + 0x3000),
          "one exact LoaderData allocation accepted");

    put(&m, 0, EfiLoaderData, BASE + 0x2000, 1);
    put(&m, 1, EfiLoaderData, BASE, 2);
    CHECK(owns(&m, 2, BASE, BASE + 0x3000),
          "unsorted adjacent LoaderData descriptors cover the span");

    put(&m, 0, EfiReservedMemoryType, BASE - 0x3000, 1);
    put(&m, 1, EfiLoaderData, BASE - 0x1000, 5);
    CHECK(owns(&m, 2, BASE, BASE + 0x3000),
          "unrelated reserved memory outside the target is harmless");

    put(&m, 0, EfiACPIMemoryNVS, BASE, 3);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "ACPI NVS overlap rejected");

    put(&m, 0, EfiReservedMemoryType, BASE, 3);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "firmware Reserved overlap rejected");

    put(&m, 0, EfiACPIReclaimMemory, BASE, 3);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "ACPI reclaim overlap rejected");

    put(&m, 0, EfiBootServicesData, BASE, 3);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "BootServicesData is not accepted after AllocateAddress");

    put(&m, 0, EfiConventionalMemory, BASE, 3);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "Conventional memory is not accepted after AllocateAddress");

    put(&m, 0, EfiLoaderData, BASE, 3);
    put(&m, 1, EfiACPIMemoryNVS, BASE + 0x1000, 1);
    CHECK(!owns(&m, 2, BASE, BASE + 0x3000),
          "a covering LoaderData descriptor cannot hide overlapping NVS");

    put(&m, 0, EfiLoaderData, BASE, 1);
    put(&m, 1, EfiLoaderData, BASE + 0x2000, 1);
    CHECK(!owns(&m, 2, BASE, BASE + 0x3000),
          "a one-page hole is rejected");

    put(&m, 0, EfiLoaderData, BASE + 0x1000, 2);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "missing target prefix is rejected");

    put(&m, 0, EfiLoaderData, BASE, 2);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "missing target suffix is rejected");

    put(&m, 0, EfiLoaderData, ~(UINT64)0 - 0xFFF, 2);
    CHECK(!owns(&m, 1, BASE, BASE + 0x3000),
          "overflowing descriptor extent is rejected");

    put(&m, 0, EfiLoaderData, BASE, 3);
    CHECK(!efi_load_range_is_loader_data(m.bytes, STRIDE - 1, STRIDE,
                                         BASE, BASE + 0x3000),
          "partial descriptor is rejected");
    CHECK(!efi_load_range_is_loader_data(m.bytes, STRIDE, 32,
                                         BASE, BASE + 0x3000),
          "undersized descriptor stride is rejected");
    CHECK(!efi_load_range_is_loader_data(m.bytes, STRIDE, STRIDE,
                                         BASE + 1, BASE + 0x3000),
          "unaligned target is rejected");
    CHECK(!efi_load_range_is_loader_data(NULL, STRIDE, STRIDE,
                                         BASE, BASE + 0x3000),
          "null memory map is rejected");

    printf("EFI_LOAD_POLICY: %d/%d checks passed\n", checks, checks);
    return 0;
}
