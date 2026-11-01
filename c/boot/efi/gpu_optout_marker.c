/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_optout_marker.h"

/* The marker's path, widened by hand: the loader is freestanding and builds
 * without wide-character literals (loader.c fw_path does the same). */
#define GPU_OPTOUT_PATH_UNITS 14u

/* Why byte loops, here and in optout_clear_bytes: -ffreestanding does not stop LLVM turning a copy loop into a
 * memcpy() call this binary lacks; the volatile destination prevents it (the
 * longer explanation is in vbios_rom.c). */
static void optout_copy_bytes(void *destination, const void *source, UINT64 bytes)
{
    volatile UINT8 *to = (volatile UINT8 *)destination;
    const UINT8 *from = (const UINT8 *)source;
    while (bytes--) {
        *to++ = *from++;
    }
}

/* Same volatile loop as optout_copy_bytes, for the same reason: a plain clear
 * loop would be rewritten into a memset() call this binary lacks. */
static void optout_clear_bytes(void *destination, UINT64 bytes)
{
    volatile UINT8 *to = (volatile UINT8 *)destination;
    while (bytes--) {
        *to++ = 0;
    }
}

/* Spell "\LOGIT\NOGPU" as UCS-2 (UEFI 2.10 section 13.5, EFI_FILE_PROTOCOL.Open
 * takes a CHAR16 path relative to the volume root). */
static void optout_path(CHAR16 out[GPU_OPTOUT_PATH_UNITS])
{
    static const char path[] = "\\LOGIT\\NOGPU";
    UINTN length = 0;
    while (path[length]) {
        out[length] = (CHAR16)(UINT8)path[length];
        ++length;
    }
    out[length] = 0;
}

/* Why the page copy: the module has to live in EfiLoaderData to survive
 * ExitBootServices, like every other module; the tail of the page is cleared so
 * the module carries nothing the page held before. */
static enum gpu_optout_outcome optout_build_module(EFI_BOOT_SERVICES *services,
                                                   struct gpu_optout_slot *slot,
                                                   gpu_optout_verify_fn verify)
{
    static const UINT8 marker[GPU_OPTOUT_MARKER_BYTES] = {'N', 'O', 'G', 'P', 'U', '\n'};
    EFI_PHYSICAL_ADDRESS at = 0;
    if (services->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &at)) {
        return GPU_OPTOUT_NO_MEMORY;
    }
    UINT64 page_hi = at + 0x1000;
    if (!verify(at, page_hi)) {
        services->FreePages(at, 1);
        return GPU_OPTOUT_NOT_LOADER_DATA;
    }
    optout_clear_bytes((void *)(UINTN)at, 0x1000);
    optout_copy_bytes((void *)(UINTN)at, marker, GPU_OPTOUT_MARKER_BYTES);
    slot->lo = at;
    slot->hi = at + GPU_OPTOUT_MARKER_BYTES;
    slot->page_hi = page_hi;
    for (UINTN i = 0; i < GPU_OPTOUT_MODULE_NAME_BYTES; i++) {
        slot->name[i] = GPU_OPTOUT_MODULE_NAME[i];
    }
    return GPU_OPTOUT_FOUND;
}

/* Why a function of its own: existence is the whole question, so the watched
 * mutation can make the answer "no" in exactly one place. The file is opened
 * and closed, never read. */
static int optout_file_exists(EFI_FILE_PROTOCOL *root)
{
    CHAR16 path[GPU_OPTOUT_PATH_UNITS];
    EFI_FILE_PROTOCOL *file = 0;
    optout_path(path);
    if (root->Open(root, &file, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        return 0;
    }
    file->Close(file);
#ifdef GPU_OPTOUT_MARKER_NEGCTL_NEVER_LOOKS
    /* Watched mutation: the marker is found and then ignored. */
    return 0;
#else
    return 1;
#endif
}

/* Why this order: the cheap, read-only existence test first, then the
 * allocation. Nothing is allocated for a stick without the marker. */
enum gpu_optout_outcome gpu_optout_marker_collect(EFI_BOOT_SERVICES *services,
                                                  EFI_FILE_PROTOCOL *root,
                                                  struct gpu_optout_slot *slot,
                                                  gpu_optout_verify_fn verify)
{
    if (!services || !root || !slot || !verify) {
        return GPU_OPTOUT_BAD_ARGUMENT;
    }
    if (!optout_file_exists(root)) {
        return GPU_OPTOUT_ABSENT;
    }
    return optout_build_module(services, slot, verify);
}

/* Why words: the loader prints the outcome on the serial line. */
const char *gpu_optout_outcome_name(enum gpu_optout_outcome outcome)
{
    switch (outcome) {
    case GPU_OPTOUT_ABSENT:
        return "no opt-out marker";
    case GPU_OPTOUT_FOUND:
        return "opt-out marker found";
    case GPU_OPTOUT_NO_MEMORY:
        return "opt-out marker found but no pages for its module";
    case GPU_OPTOUT_NOT_LOADER_DATA:
        return "opt-out marker found but pages are not EfiLoaderData";
    default:
        return "bad argument";
    }
}
