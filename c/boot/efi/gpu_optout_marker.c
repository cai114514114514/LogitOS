/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_optout_marker.h"

/* The widened path buffer: the longest spelling, counted by the compiler, with
 * its terminator (a string literal's sizeof includes the NUL). */
#define GPU_OPTOUT_PATH_UNITS sizeof(GPU_OPTOUT_PATH_TEXT)

/* One page: the module lives in whole pages (see optout_build_module). */
#define GPU_OPTOUT_PAGE_BYTES 0x1000u

/* The spellings of the owner's file, in the order they are tried. FAT lookup is
 * case-insensitive, so no case variants are listed. */
static const char *const optout_marker_paths[] = {
    GPU_OPTOUT_PATH_PLAIN,
    GPU_OPTOUT_PATH_TEXT
};

#define GPU_OPTOUT_PATH_COUNT (sizeof optout_marker_paths / sizeof optout_marker_paths[0])

_Static_assert(sizeof(GPU_OPTOUT_PATH_PLAIN) <= GPU_OPTOUT_PATH_UNITS,
               "the widened path buffer must hold every spelling");

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

/* Spell an ASCII path as UCS-2 (UEFI 2.10 section 13.5, EFI_FILE_PROTOCOL.Open
 * takes a CHAR16 path relative to the volume root). The loader is freestanding
 * and builds without wide-character literals (loader.c fw_path does the same).
 * Nonzero when the path fits, terminator included. */
static int optout_widen_path(CHAR16 out[GPU_OPTOUT_PATH_UNITS], const char *path)
{
    UINTN length = 0;
    while (path[length]) {
        if (length + 1 >= GPU_OPTOUT_PATH_UNITS) {
            return 0;
        }
        out[length] = (CHAR16)(UINT8)path[length];
        ++length;
    }
    out[length] = 0;
    return 1;
}

/* Why the page copy: the module has to live in EfiLoaderData to survive
 * ExitBootServices, like every other module; the tail of the page is cleared so
 * the module carries nothing the page held before. */
static enum gpu_optout_outcome optout_build_module(EFI_BOOT_SERVICES *services,
                                                   struct gpu_optout_slot *slot,
                                                   gpu_optout_verify_fn verify)
{
    static const char marker[] = GPU_OPTOUT_MARKER_TEXT;
    EFI_PHYSICAL_ADDRESS at = 0;
    if (services->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &at)) {
        return GPU_OPTOUT_NO_MEMORY;
    }
    UINT64 page_hi = at + GPU_OPTOUT_PAGE_BYTES;
    if (!verify(at, page_hi)) {
        services->FreePages(at, 1);
        return GPU_OPTOUT_NOT_LOADER_DATA;
    }
    optout_clear_bytes((void *)(UINTN)at, GPU_OPTOUT_PAGE_BYTES);
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
 * and closed, never read. The result is the EFI status of the open when the
 * file is not there or cannot be told about; EFI_SUCCESS means it exists. */
static EFI_STATUS optout_file_exists(EFI_FILE_PROTOCOL *root, const char *ascii_path)
{
    CHAR16 path[GPU_OPTOUT_PATH_UNITS];
    EFI_FILE_PROTOCOL *file = 0;
    if (!optout_widen_path(path, ascii_path)) {
        return EFI_INVALID_PARAMETER;
    }
    EFI_STATUS status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        return status;
    }
    file->Close(file);
#ifdef GPU_OPTOUT_MARKER_NEGCTL_NEVER_LOOKS
    /* Watched mutation: the marker is found and then ignored. */
    return EFI_NOT_FOUND;
#else
    return EFI_SUCCESS;
#endif
}

/* Why this order and why only EFI_NOT_FOUND means "absent": a stick without the
 * marker answers "not found"; any other answer (no media, a device error, a
 * corrupt volume) says nothing about whether the owner created the file, and
 * treating it as "absent" would run the self-test the owner may have switched
 * off. A file found under either spelling wins over an error on the other. */
static enum gpu_optout_outcome optout_look_for_marker(EFI_FILE_PROTOCOL *root,
                                                      struct gpu_optout_slot *slot)
{
    EFI_STATUS unreadable_status = EFI_SUCCESS;
    for (UINTN i = 0; i < GPU_OPTOUT_PATH_COUNT; ++i) {
        EFI_STATUS status = optout_file_exists(root, optout_marker_paths[i]);
        if (status == EFI_SUCCESS) {
            slot->found_path = optout_marker_paths[i];
            return GPU_OPTOUT_FOUND;
        }
        if (status != EFI_NOT_FOUND && unreadable_status == EFI_SUCCESS) {
            unreadable_status = status;
        }
    }
    if (unreadable_status != EFI_SUCCESS) {
        slot->unreadable_status = unreadable_status;
        return GPU_OPTOUT_UNREADABLE;
    }
    return GPU_OPTOUT_ABSENT;
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
    slot->found_path = 0;
    slot->unreadable_status = 0;
    enum gpu_optout_outcome outcome = optout_look_for_marker(root, slot);
    if (outcome != GPU_OPTOUT_FOUND) {
        return outcome;
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
    case GPU_OPTOUT_UNREADABLE:
        return "opt-out marker lookup failed, so its presence is unknown";
    default:
        return "bad argument";
    }
}

/* Why a list of the safe outcomes and not of the unsafe ones: a new outcome
 * added later must default to "assume the owner opted out", not to "carry
 * on". Only a clean "no marker" and a finished module need nothing more. */
int gpu_optout_outcome_assumes_opt_out(enum gpu_optout_outcome outcome)
{
#ifdef GPU_OPTOUT_MARKER_NEGCTL_FAIL_OPEN
    /* Watched mutation: a marker the loader cannot pass on is carried on past. */
    (void)outcome;
    return 0;
#else
    return outcome != GPU_OPTOUT_ABSENT && outcome != GPU_OPTOUT_FOUND;
#endif
}
