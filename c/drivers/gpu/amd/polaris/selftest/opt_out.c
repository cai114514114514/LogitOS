/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "amd/polaris/selftest/selftest.h"
#include "bootmod.h"

#ifdef POLARIS_SELFTEST_COMPILED

/* Why a boot module: the owner switches the GPU self-test off by creating an
 * empty file \LOGIT\NOGPU on the boot stick. The kernel cannot read the ESP,
 * but the UEFI loader can, before ExitBootServices, and it already hands the
 * kernel everything else it read from the stick as boot modules
 * (c/boot/efi/gpu_optout_marker.c, c/kernel/init/bootmod.h). A module named
 * "nogpu" is therefore the marker's presence, the same road the AMD microcode
 * and the VBIOS take. */
int polaris_selftest_opted_out(void)
{
#ifdef POLARIS_OPTOUT_NEGCTL_NEVER
    /* Watched mutation: the marker is never seen. */
    return 0;
#else
    return bootmod_find(POLARIS_SELFTEST_OPT_OUT_MODULE, 0) != 0;
#endif
}

#endif /* POLARIS_SELFTEST_COMPILED */
