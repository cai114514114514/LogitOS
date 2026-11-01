/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_POLARIS_SMU_SECURITY_KEY_H
#define LOGIT_POLARIS_SMU_SECURITY_KEY_H

#include <stdint.h>

/* Which SMC microcode file a Polaris10 board needs depends on a fuse the
 * hardware exposes in the SMU_FIRMWARE register, behind the SMC indirect
 * register port. This module reads that register exactly as Linux v6.12 does
 * and says nothing about files: the key-to-file choice is
 * polaris_resource_file_path() in resource/files.c.
 *
 * Linux v6.12 references:
 *   drivers/gpu/drm/amd/pm/powerplay/smumgr/polaris10_smumgr.c
 *     polaris10_start_smu(): reads SMU_FIRMWARE.SMU_MODE into protected_mode
 *     and SMU_FIRMWARE.SMU_SEL into smu7_data.security_hard_key with
 *     PHM_READ_VFPF_INDIRECT_FIELD(..., CGS_IND_REG__SMC, SMU_FIRMWARE, ...).
 *   drivers/gpu/drm/amd/pm/powerplay/smumgr/smu7_smumgr.c
 *     smu7_upload_smu_firmware_image(): security_hard_key == 1 selects
 *     UCODE_ID_SMU (polaris10_smc.bin, _k_ or _k2_ variants), anything else
 *     UCODE_ID_SMU_SK (polaris10_smc_sk.bin).
 *   drivers/gpu/drm/amd/amdgpu/amdgpu_cgs.c
 *     amdgpu_cgs_read_ind_register(CGS_IND_REG__SMC) -> RREG32_SMC().
 *   drivers/gpu/drm/amd/amdgpu/vi.c
 *     vi_smc_rreg(): WREG32(mmSMC_IND_INDEX_11, reg), then
 *     RREG32(mmSMC_IND_DATA_11).
 *   drivers/gpu/drm/amd/include/asic_reg/smu/smu_7_1_3_d.h
 *     mmSMC_IND_INDEX_11 0x1AC, mmSMC_IND_DATA_11 0x1AD,
 *     mmSMC_IND_ACCESS_CNTL 0x92, ixSMU_FIRMWARE 0xe00030a4.
 *   drivers/gpu/drm/amd/include/asic_reg/smu/smu_7_1_3_sh_mask.h
 *     SMU_FIRMWARE__SMU_MODE_MASK 0x10000, SMU_FIRMWARE__SMU_SEL_MASK 0x20000,
 *     SMC_IND_ACCESS_CNTL__AUTO_INCREMENT_IND_11_MASK 0x800.
 * Linux dword register indices are multiplied by four here: callers pass BAR5
 * byte offsets.
 *
 * Deviation from Linux, stated so nobody has to find it: Linux reads the key
 * only when the SMC RAM is not already running (polaris10_start_smu()). Here
 * the key is needed on a card the firmware already POSTed with the SMC running,
 * only to pick the microcode file, so it is read unconditionally. The register
 * is a hardware strap, not run-time state; the loader (smu/loader.c) later
 * re-reads the same register and refuses a mismatch.
 *
 * Writing the index register is the first MMIO write of the whole GPU
 * self-test (selftest/selftest.c), so the caller must hold the ownership
 * lease before calling. Model evidence only; not verified on X79-VG4
 * hardware. */

/* BAR5 byte offsets (Linux dword index * 4). */
#define POLARIS_SMC_INDEX_REGISTER 0x6b0u          /* mmSMC_IND_INDEX_11 */
#define POLARIS_SMC_DATA_REGISTER 0x6b4u           /* mmSMC_IND_DATA_11 */
#define POLARIS_SMC_ACCESS_CONTROL_REGISTER 0x248u /* mmSMC_IND_ACCESS_CNTL */

/* Address inside the SMC register space, written to the index register. */
#define POLARIS_SMU_FIRMWARE_ADDRESS 0xe00030a4u   /* ixSMU_FIRMWARE */

/* SMC_IND_ACCESS_CNTL bit that makes port 11 advance its index after every
 * data access; a previous owner leaving it set would turn our single read
 * into a moving target. */
#define POLARIS_SMC_ACCESS_AUTO_INCREMENT_PORT_11 0x800u

#define POLARIS_SMU_FIRMWARE_PROTECTED_MODE_MASK 0x00010000u /* SMU_MODE */
#define POLARIS_SMU_FIRMWARE_SECURITY_KEY_MASK 0x00020000u   /* SMU_SEL */
#define POLARIS_SMU_FIRMWARE_SECURITY_KEY_SHIFT 17u

/* How the key is read: two callbacks over BAR5 byte offsets. Both return 0 on
 * success and nonzero when the access did not happen. */
struct polaris_security_key_ops {
    void *context;
    int (*read_register)(void *context, uint32_t byte_offset, uint32_t *value);
    int (*write_register)(void *context, uint32_t byte_offset, uint32_t value);
};

struct polaris_security_key {
    uint32_t firmware_register; /* SMU_FIRMWARE exactly as read */
    unsigned security_key;      /* SMU_SEL: 0 selects the _sk file, 1 the other */
    unsigned protected_mode;    /* SMU_MODE: the SMC boots protected firmware */
};

enum polaris_security_key_result {
    POLARIS_SECURITY_KEY_OK = 0,
    POLARIS_SECURITY_KEY_BAD_ARGUMENT = -1,
    POLARIS_SECURITY_KEY_ACCESS_CONTROL_UNREADABLE = -2,
    POLARIS_SECURITY_KEY_AUTO_INCREMENT_ACTIVE = -3,
    POLARIS_SECURITY_KEY_INDEX_WRITE_FAILED = -4,
    POLARIS_SECURITY_KEY_DATA_UNREADABLE = -5,
    POLARIS_SECURITY_KEY_DEVICE_ABSENT = -6
};

/* Read SMU_FIRMWARE through SMC port 11. At most one register write (the
 * index) and two reads (SMC_IND_ACCESS_CNTL, then SMC_IND_DATA_11); no loop, so
 * there is nothing to time out. On any failure the output is left untouched and
 * no register is written after the failing step. */
int polaris_security_key_read(const struct polaris_security_key_ops *ops,
                              struct polaris_security_key *out);

/* Text for the boot log and the HUD; never NULL. */
const char *polaris_security_key_result_name(int result);

#endif
