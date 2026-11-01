/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_GPU_OPTOUT_MARKER_H
#define LOGIT_GPU_OPTOUT_MARKER_H

#include "efi.h"

/* The owner's switch for the GPU self-test (stage B3 of
 * docs/superpowers/plans/2026-11-01-rx580-stage-b.md): an empty file
 * \LOGIT\NOGPU on the boot stick's EFI System Partition.
 *
 * The kernel cannot read the ESP, so the UEFI loader does, before
 * ExitBootServices, the same way it reads the AMD microcode and the video
 * BIOS: when the file exists it hands the kernel a boot module named "nogpu"
 * (LOGIT_BOOT_TAG_MODULE, include/abi/logit_boot.h). Only the file's presence
 * matters; its content is ignored, so an empty file made from Windows works.
 * The module's own bytes are a fixed marker text because a boot module cannot
 * be empty (c/kernel/init/bootmod.c refuses a zero-length range).
 *
 * The consumer is polaris_selftest_opted_out() in
 * c/drivers/gpu/amd/polaris/selftest/opt_out.c. Model evidence only; not
 * verified on X79-VG4 hardware. */

/* The module name, equal to POLARIS_SELFTEST_OPT_OUT_MODULE in the kernel; the
 * host gate pins both to the same literal. */
#define GPU_OPTOUT_MODULE_NAME "nogpu"
#define GPU_OPTOUT_MODULE_NAME_BYTES 6u
#define GPU_OPTOUT_SLOT_NAME_BYTES 64u
/* "NOGPU\n" */
#define GPU_OPTOUT_MARKER_BYTES 6u

enum gpu_optout_outcome {
    GPU_OPTOUT_ABSENT = 0,      /* no \LOGIT\NOGPU: the normal case */
    GPU_OPTOUT_FOUND,           /* module produced in SLOT */
    GPU_OPTOUT_NO_MEMORY,       /* the file exists but AllocatePages refused */
    GPU_OPTOUT_NOT_LOADER_DATA, /* the file exists but the pages are not EfiLoaderData */
    GPU_OPTOUT_BAD_ARGUMENT
};

/* One finished module, the same fields loader.c keeps for every other. */
struct gpu_optout_slot {
    UINT64 lo;
    UINT64 hi;
    UINT64 page_hi;
    char name[GPU_OPTOUT_SLOT_NAME_BYTES];
};

/* Called with the page range of a fresh allocation; nonzero when the memory map
 * really lists it as EfiLoaderData (loader.c: allocated_range_is_loader_data). */
typedef int (*gpu_optout_verify_fn)(UINT64 lo, UINT64 hi);

/* Look for \LOGIT\NOGPU under ROOT (the opened volume) and, if it is there, copy
 * the marker text into fresh EfiLoaderData pages and describe them in SLOT. A
 * failure to build the module is reported, never fatal: the caller prints it,
 * and the boot goes on with the self-test enabled only if the module is
 * missing, which is why the caller must run this BEFORE the firmware files fill
 * the module registry. */
enum gpu_optout_outcome gpu_optout_marker_collect(EFI_BOOT_SERVICES *services,
                                                  EFI_FILE_PROTOCOL *root,
                                                  struct gpu_optout_slot *slot,
                                                  gpu_optout_verify_fn verify);

const char *gpu_optout_outcome_name(enum gpu_optout_outcome outcome);

#endif
