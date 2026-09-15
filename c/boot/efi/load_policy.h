/* UEFI kernel-destination validation shared by the loader and its host gate.
 *
 * AllocatePages(AllocateAddress, EfiLoaderData, ...) is necessary but it is
 * not the final authority: before ExitBootServices the memory map must still
 * describe every target byte as EfiLoaderData. A successful status paired
 * with a hole, an overflowing descriptor, or a reserved/ACPI descriptor is a
 * refusal. The second pass that proves coverage is deliberately separate from
 * the first pass that rejects wrong types; otherwise an overlapping
 * LoaderData descriptor could hide a firmware-reserved descriptor beneath it.
 */
#ifndef LOGIT_EFI_LOAD_POLICY_H
#define LOGIT_EFI_LOAD_POLICY_H

#include "efi.h"

static int efi_descriptor_extent(const EFI_MEMORY_DESCRIPTOR *d,
                                 UINT64 *lo, UINT64 *hi)
{
    if (d->NumberOfPages > (~(UINT64)0 - d->PhysicalStart) / 4096)
        return 0;
    *lo = d->PhysicalStart;
    *hi = d->PhysicalStart + d->NumberOfPages * 4096;
    return *hi > *lo;
}

static int efi_load_range_is_loader_data(const void *memory_map,
                                         UINTN memory_map_size,
                                         UINTN descriptor_size,
                                         UINT64 target_lo,
                                         UINT64 target_hi)
{
    if (!memory_map || target_lo >= target_hi ||
        (target_lo & 4095) || (target_hi & 4095) ||
        descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        memory_map_size < descriptor_size ||
        memory_map_size % descriptor_size)
        return 0;

    UINTN count = memory_map_size / descriptor_size;
    const UINT8 *bytes = (const UINT8 *)memory_map;

    /* Reject every non-LoaderData descriptor that intersects the target.
     * This includes Conventional and BootServices memory: after our explicit
     * allocation, either type means the claimed ownership did not stick. */
    for (UINTN i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(bytes + i * descriptor_size);
        UINT64 lo, hi;
        if (!efi_descriptor_extent(d, &lo, &hi)) return 0;
        if (hi <= target_lo || lo >= target_hi) continue;
        if (d->Type != EfiLoaderData) return 0;
    }

    /* Prove there is no undescribed byte. The firmware normally returns a
     * sorted map, but this walk also accepts a valid unsorted map and makes no
     * ordering assumption. */
    UINT64 covered = target_lo;
    while (covered < target_hi) {
        UINT64 next = covered;
        for (UINTN i = 0; i < count; i++) {
            const EFI_MEMORY_DESCRIPTOR *d =
                (const EFI_MEMORY_DESCRIPTOR *)(bytes + i * descriptor_size);
            UINT64 lo, hi;
            if (!efi_descriptor_extent(d, &lo, &hi)) return 0;
            /* The first pass already proved every intersecting descriptor has
             * the right type; repeating the test here would let the mutation
             * gate break one check while accidentally retaining a duplicate. */
            if (lo <= covered && hi > next)
                next = hi;
        }
        if (next == covered) return 0;
        covered = next > target_hi ? target_hi : next;
    }
    return 1;
}

#endif
