/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "amd/polaris/smu/security_key.h"

/* An absent PCI function answers every memory read with all ones, so the
 * value can never be mistaken for a register that really holds it: both
 * registers read here have reserved bits that are zero on a live part. */
#define REGISTER_READ_ABSENT UINT32_MAX

/* Why a function of its own: which SMC file the board gets follows from this
 * one bit and from nothing else, so the watched mutation reads the neighbouring
 * bit (SMU_MODE, the protected-mode flag) instead and the gate must see the
 * wrong file chosen. SMU_SEL is bit 17 (smu_7_1_3_sh_mask.h
 * SMU_FIRMWARE__SMU_SEL_MASK 0x20000), polaris10_smumgr.c security_hard_key. */
static unsigned security_key_from_register(uint32_t firmware_register)
{
#ifdef POLARIS_SECURITY_KEY_NEGCTL_READ_MODE_BIT
    return !!(firmware_register & POLARIS_SMU_FIRMWARE_PROTECTED_MODE_MASK);
#else
    return (firmware_register & POLARIS_SMU_FIRMWARE_SECURITY_KEY_MASK) >>
           POLARIS_SMU_FIRMWARE_SECURITY_KEY_SHIFT;
#endif
}

/* Why this exists: the self-test must know which SMC microcode file the board
 * wants before it loads anything, and the only authority is the SMU_FIRMWARE
 * strap. The sequence is Linux vi_smc_rreg() with one extra read in front:
 * the access-control read refuses a port left in auto-increment mode, because
 * the index would then move after our data read and the next SMC access of
 * another component would land on the wrong address. With that refused, the
 * data read cannot move the index, so the single index write below is the only
 * register write of the sequence, and nothing is written once a step has
 * failed. Two reads (access control, then data) and one write in all. */
int polaris_security_key_read(const struct polaris_security_key_ops *ops,
                              struct polaris_security_key *out)
{
    if (!ops || !ops->read_register || !ops->write_register || !out) {
        return POLARIS_SECURITY_KEY_BAD_ARGUMENT;
    }

    uint32_t access_control = 0;
    if (ops->read_register(ops->context, POLARIS_SMC_ACCESS_CONTROL_REGISTER,
                           &access_control) ||
        access_control == REGISTER_READ_ABSENT) {
        return POLARIS_SECURITY_KEY_ACCESS_CONTROL_UNREADABLE;
    }
    if (access_control & POLARIS_SMC_ACCESS_AUTO_INCREMENT_PORT_11) {
        return POLARIS_SECURITY_KEY_AUTO_INCREMENT_ACTIVE;
    }

    /* vi_smc_rreg(): WREG32(mmSMC_IND_INDEX_11, reg); RREG32(mmSMC_IND_DATA_11).
     * PCIe never lets the data read overtake the posted index write, so no
     * read-back of the index is needed (Linux does not do one either). */
    if (ops->write_register(ops->context, POLARIS_SMC_INDEX_REGISTER,
                            POLARIS_SMU_FIRMWARE_ADDRESS)) {
        return POLARIS_SECURITY_KEY_INDEX_WRITE_FAILED;
    }
    uint32_t firmware_register = 0;
    if (ops->read_register(ops->context, POLARIS_SMC_DATA_REGISTER,
                           &firmware_register)) {
        return POLARIS_SECURITY_KEY_DATA_UNREADABLE;
    }
    if (firmware_register == REGISTER_READ_ABSENT) {
        return POLARIS_SECURITY_KEY_DEVICE_ABSENT;
    }

    out->firmware_register = firmware_register;
    out->security_key = security_key_from_register(firmware_register);
    out->protected_mode = !!(firmware_register & POLARIS_SMU_FIRMWARE_PROTECTED_MODE_MASK);
    return POLARIS_SECURITY_KEY_OK;
}

/* Why words: the serial line and the HUD are the only evidence a machine with
 * no debugger gives, and "-5" tells a photograph nothing. */
const char *polaris_security_key_result_name(int result)
{
    switch (result) {
    case POLARIS_SECURITY_KEY_OK:
        return "ok";
    case POLARIS_SECURITY_KEY_BAD_ARGUMENT:
        return "bad-argument";
    case POLARIS_SECURITY_KEY_ACCESS_CONTROL_UNREADABLE:
        return "smc-access-control-unreadable";
    case POLARIS_SECURITY_KEY_AUTO_INCREMENT_ACTIVE:
        return "smc-port-auto-increment";
    case POLARIS_SECURITY_KEY_INDEX_WRITE_FAILED:
        return "smc-index-write-failed";
    case POLARIS_SECURITY_KEY_DATA_UNREADABLE:
        return "smu-firmware-unreadable";
    case POLARIS_SECURITY_KEY_DEVICE_ABSENT:
        return "smu-firmware-reads-all-ones";
    default:
        return "unknown";
    }
}
