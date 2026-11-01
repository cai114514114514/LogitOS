/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_GPU_OPTOUT_MARKER_H
#define LOGIT_GPU_OPTOUT_MARKER_H

#include "efi.h"

/* The owner's switch for the GPU self-test (stage B3 of
 * docs/superpowers/plans/2026-11-01-rx580-stage-b.md): an empty file
 * \LOGIT\NOGPU, or \LOGIT\NOGPU.TXT, on the boot stick's EFI System Partition.
 * FAT file names are case-insensitive, and Windows hides a ".txt" extension
 * (a "NOGPU" made in Notepad is NOGPU.TXT), so both spellings count.
 *
 * The kernel cannot read the ESP, so the UEFI loader does, before
 * ExitBootServices, the same way it reads the AMD microcode and the video
 * BIOS: when the file exists it hands the kernel a boot module named "nogpu"
 * (LOGIT_BOOT_TAG_MODULE, include/abi/logit_boot.h). Only the file's presence
 * matters; its content is ignored, so an empty file made from Windows works.
 * The module's own bytes are a fixed marker text because a boot module cannot
 * be empty (c/kernel/init/bootmod.c refuses a zero-length range).
 *
 * The switch fails closed: when the loader cannot tell whether the file exists
 * (the volume cannot be opened, a lookup fails with anything but "not found")
 * or cannot build the module (no pages), it says so on the serial line and
 * hands the kernel LOGIT_BOOT_TAG_GPU_OPTOUT instead, which the kernel treats
 * exactly like the module.
 *
 * The consumer is polaris_selftest_opted_out() in
 * c/drivers/gpu/amd/polaris/selftest/opt_out.c. Model evidence only; not
 * verified on X79-VG4 hardware. */

/* The module name, equal to POLARIS_SELFTEST_OPT_OUT_MODULE in the kernel; the
 * host gate pins both to the same literal. */
#define GPU_OPTOUT_MODULE_NAME "nogpu"
/* The name with its terminator, counted by the compiler. */
#define GPU_OPTOUT_MODULE_NAME_BYTES sizeof(GPU_OPTOUT_MODULE_NAME)
#define GPU_OPTOUT_SLOT_NAME_BYTES 64u
/* The module's content: a boot module cannot be empty. */
#define GPU_OPTOUT_MARKER_TEXT "NOGPU\n"
#define GPU_OPTOUT_MARKER_BYTES (sizeof(GPU_OPTOUT_MARKER_TEXT) - 1u)
/* The two spellings of the owner's file, UEFI 2.10 section 13.5 paths relative
 * to the volume root. The longer one sizes the widened path buffer. */
#define GPU_OPTOUT_PATH_PLAIN "\\LOGIT\\NOGPU"
#define GPU_OPTOUT_PATH_TEXT "\\LOGIT\\NOGPU.TXT"

enum gpu_optout_outcome {
    GPU_OPTOUT_ABSENT = 0,      /* no marker file: the normal case */
    GPU_OPTOUT_FOUND,           /* module produced in SLOT */
    GPU_OPTOUT_NO_MEMORY,       /* the file exists but AllocatePages refused */
    GPU_OPTOUT_NOT_LOADER_DATA, /* the file exists but the pages are not EfiLoaderData */
    GPU_OPTOUT_UNREADABLE,      /* a lookup failed with something other than "not found" */
    GPU_OPTOUT_BAD_ARGUMENT
};

/* One finished module, the same fields loader.c keeps for every other. */
struct gpu_optout_slot {
    UINT64 lo;
    UINT64 hi;
    UINT64 page_hi;
    char name[GPU_OPTOUT_SLOT_NAME_BYTES];
    /* Which spelling was found (one of the GPU_OPTOUT_PATH_* strings), or NULL;
     * set whenever a file was found, whether or not the module could be built. */
    const char *found_path;
    /* The EFI status of the lookup that made the outcome UNREADABLE. */
    UINT64 unreadable_status;
};

/* Called with the page range of a fresh allocation; nonzero when the memory map
 * really lists it as EfiLoaderData (loader.c: allocated_range_is_loader_data). */
typedef int (*gpu_optout_verify_fn)(UINT64 lo, UINT64 hi);

/* Look for \LOGIT\NOGPU and \LOGIT\NOGPU.TXT under ROOT (the opened volume)
 * and, if either is there, copy the marker text into fresh EfiLoaderData pages
 * and describe them in SLOT. A failure to build the module is reported, never
 * fatal: the caller prints it, and (gpu_optout_outcome_assumes_opt_out) tells
 * the kernel to treat the self-test as off anyway. The caller must run this
 * BEFORE the firmware files fill the module registry. */
enum gpu_optout_outcome gpu_optout_marker_collect(EFI_BOOT_SERVICES *services,
                                                  EFI_FILE_PROTOCOL *root,
                                                  struct gpu_optout_slot *slot,
                                                  gpu_optout_verify_fn verify);

const char *gpu_optout_outcome_name(enum gpu_optout_outcome outcome);

/* Nonzero for every outcome that leaves the owner's wish unknown or
 * unrepresentable (no memory, wrong memory type, unreadable volume, bad
 * argument): the self-test must then be treated as switched off. */
int gpu_optout_outcome_assumes_opt_out(enum gpu_optout_outcome outcome);

#endif
