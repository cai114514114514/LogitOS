#include "loader.h"
#include "amd/polaris/pci_id.h"
#include "amd/polaris/memory/layout.h"

/* Register numbers are translated to BYTE offsets once here. SMC indirect
 * addresses remain byte addresses. Linux v6.12 primary protocol references:
 * drivers/gpu/drm/amd/pm/powerplay/smumgr/{polaris10,smu7}_smumgr.c;
 * drivers/gpu/drm/amd/include/asic_reg/smu/smu_7_1_3_{d,sh_mask}.h.
 * Do not reuse the test-only mailbox's logical enum as hardware offsets. */
#define INDEX 0x6b0u
#define DATA 0x6b4u
#define ACCESS 0x248u
#define MESSAGE 0x250u
#define RESPONSE 0x254u
#define ARGUMENT 0x290u
#define RESET 0x80000000u
#define CLOCK 0x80000004u
#define PC 0x80000370u
#define EVENTS 0xc0000004u
#define STATUS 0xe0003088u
#define FIRMWARE 0xe00030a4u
#define FLAGS 0x3f000u
#define SOFT_POINTER 0x20030u
#define LOAD_STATUS_OFFSET 0x6cu
/* Wait bounds, from Linux v6.12. Every SMU register wait there (smu7_send_msg_
 * to_smc's PHM_WAIT_FIELD_UNEQUAL, the SMU_STATUS and interrupt waits in
 * polaris10_start_smu_in_*_mode, smu7_check_fw_load_finish) runs for
 * hwmgr->usec_timeout, set in hwmgr_early_init() (pm/powerplay/hwmgr/hwmgr.c)
 * to AMD_MAX_USEC_TIMEOUT = 1000000 us = 1 s (include/amd_shared.h). The first
 * version of this loader allowed 100 ms per wait, ten times less than the
 * kernel driver gives the same firmware, which on a real card could turn a slow
 * but healthy load into a failure. Linux has no whole-sequence limit; this
 * driver adds one so a stuck card cannot freeze the boot for good: the ten or so
 * sequential waits of a load, each at most POLL_TIMEOUT_US, plus margin for the
 * slow config-space-checked register writes of the firmware upload. */
#define POLL_TIMEOUT_US 1000000u
#define LOAD_TIMEOUT_US 10000000u
/* Only guards a clock that stops; the time bounds above are the real limits. */
#define POLL_LIMIT 1000000u
#define RESPONSE_MASK 0xffffu
/* SMC_RESP_0.SMC_RESP value of a command that succeeded: PPSMC_Result_OK = 0x01
 * (Linux v6.12 drivers/gpu/drm/amd/pm/powerplay/inc/smu7_ppsmc.h; 0xFE is
 * PPSMC_Result_UnknownCmd). smu7_send_msg_to_smc in smu7_smumgr.c tests "ret != 1". */
#define RESPONSE_OK 1u
/* SMC RAM address where the firmware image is written: the ucode_start_addr of
 * the SMC firmware header, which polaris_smu_load requires to be this value
 * (Linux v6.12 smu7_upload_smu_firmware_image copies to hdr->ucode_start_addr). */
#define SMC_RAM_START 0x20000u
/* Bit fields of the SMC registers prepare_firmware reads, Linux v6.12
 * smu_7_1_3_sh_mask.h. SMU_FIRMWARE: SMU_IN_PROG (bit 0), SMU_RD_DONE (bits
 * 1-2), SMU_SRAM_RD_BLOCK_EN (bit 3), SMU_SRAM_WR_BLOCK_EN (bit 4), SMU_counter
 * (bits 8-11), SMU_MODE (bit 16), SMU_SEL (bit 17). A set IN_PROG or SRAM block
 * bit means the SMU is busy with a transfer, which this loader will not touch. */
#define SMU_FIRMWARE_KNOWN_BITS 0x30f1fu
#define SMU_FIRMWARE_BUSY_BITS 0x19u
#define SMU_FIRMWARE_MODE_SHIFT 16u
#define SMU_FIRMWARE_SEL_SHIFT 17u
/* ixSMC_SYSCON_CLOCK_CNTL_0.ck_disable (bit 0) and ixSMC_SYSCON_RESET_CNTL.rst_reg
 * (bit 0): set means the SMC clock is stopped / the SMC is held in reset. */
#define SMC_CLOCK_DISABLED_BIT 1u
#define SMC_IN_RESET_BIT 1u
/* ixFIRMWARE_FLAGS.INTERRUPTS_ENABLED (bit 0): the running firmware finished
 * its initialization. */
#define FIRMWARE_INTERRUPTS_ENABLED_BIT 1u
/* smu7_is_smc_ram_running (smu7_smumgr.c): the SMC program counter is at or
 * past 0x20100 once the firmware is running; SMC_RAM_END (256 KiB) bounds any
 * sane value. */
#define SMC_PC_RUNNING_MINIMUM 0x20100u
#define SMC_RAM_END 0x40000u
/* Bytes per firmware word: smu7_copy_bytes_to_smc writes the image 32 bits at
 * a time through SMC_IND_DATA_11. */
#define FIRMWARE_WORD_BYTES 4u

/* SMU7 mailbox messages; address halves must be sent high before low. */
enum smu_message {
    SMU_TEST = 0x100,
    SMU_SET_TOC_ADDRESS_HIGH = 0x250,
    SMU_SET_TOC_ADDRESS_LOW = 0x251,
    SMU_SET_SCRATCH_ADDRESS_HIGH = 0x252,
    SMU_SET_SCRATCH_ADDRESS_LOW = 0x253,
    SMU_LOAD_UCODES = 0x254
};

struct session {
    struct polaris_smu_loader *loader;
    const struct polaris_smu_loader_ops *ops;
    uint64_t started_us, last_us;
    unsigned write_attempted;
    /* The SMC is held in reset (see reset_window_begin). */
    unsigned reset_window_open;
    uint64_t reset_window_started_us;
};

/* Why the whole-load limit is not enforced while the SMC is in reset: with
 * reset asserted the SMC RAM holds a partly written image, and the only safe
 * way out is to finish the upload and release reset. Linux has no whole-load
 * limit at all (only the per-wait usec_timeout), so skipping it here costs
 * nothing it ever had; the window contains no wait, only a bounded number of
 * register accesses. */
static void reset_window_begin(struct session *session)
{
    session->reset_window_open = 1;
    session->reset_window_started_us = session->last_us;
}

/* Why the window's time is not charged to the 10 s limit: the waits that follow
 * (boot-sequence interrupt, SMU_TEST, firmware flags) each have their own
 * 1 s limit and must not be cut short because a slow, healthy upload used up
 * the whole-load allowance. The time is reported through upload_microseconds
 * instead. */
static void reset_window_end(struct session *session)
{
    uint64_t now = session->ops->now_us(session->ops->opaque);
    if (now > session->reset_window_started_us)
        session->started_us += now - session->reset_window_started_us;
    if (now > session->last_us)
        session->last_us = now;
    session->reset_window_open = 0;
}

/* Why every register access passes through here: it is the single place that
 * enforces the whole-load limit and notices a clock that runs backwards. */
static int clock_check(struct session *session, uint64_t *now)
{
    *now = session->ops->now_us(session->ops->opaque);
    if (session->reset_window_open) {
        /* No limit inside the window; the clock is still followed so the
         * first check after it starts from the latest time seen. */
        if (*now > session->last_us)
            session->last_us = *now;
        return 0;
    }
    if (*now < session->last_us || *now - session->started_us >= LOAD_TIMEOUT_US)
        return POLARIS_SMU_LOAD_TIMEOUT;
    session->last_us = *now;
    return 0;
}
static int read_register(struct session *session, uint32_t reg, uint32_t *value)
{
    uint64_t now;
    int result = clock_check(session, &now);
    if (result)
        return result;
    /* These reads are status/control/pointer registers, never arbitrary SRAM
     * payload. A disappeared PCI function reads all ones: that must not look
     * like all firmware-load bits becoming complete. */
    return session->ops->read_reg(session->ops->opaque, reg, value) || *value == UINT32_MAX
               ? POLARIS_SMU_LOAD_IO
               : 0;
}
static int write_register(struct session *session, uint32_t reg, uint32_t value)
{
    uint64_t now;
    int result = clock_check(session, &now);
    if (result)
        return result;
    if (reg != INDEX)
        session->write_attempted = 1;
    return session->ops->write_reg(session->ops->opaque, reg, value) ? POLARIS_SMU_LOAD_IO : 0;
}
static int read_smc(struct session *session, uint32_t address, uint32_t *value)
{
    int result = write_register(session, INDEX, address);
    if (result)
        return result;
    return read_register(session, DATA, value);
}
static int write_smc(struct session *session, uint32_t address, uint32_t value)
{
    int result = write_register(session, INDEX, address);
    if (result)
        return result;
    return write_register(session, DATA, value);
}
static int update_smc_bits(struct session *session, uint32_t address, uint32_t mask,
                           uint32_t bits_to_set)
{
    uint32_t previous_value;
    int result = read_smc(session, address, &previous_value);
    if (result)
        return result;
    return write_smc(session, address, (previous_value & ~mask) | bits_to_set);
}

static int wait_bits(struct session *session, uint32_t address, uint32_t mask, uint32_t wanted,
                     int indirect, int nonzero, uint32_t *observed)
{
    uint64_t start = session->last_us;
    uint64_t now;
    for (unsigned poll = 0; poll < POLL_LIMIT; ++poll) {
        uint32_t value;
        int result =
            indirect ? read_smc(session, address, &value) : read_register(session, address, &value);
        if (result)
            return result;
        ++session->loader->polls;
        if (observed)
            *observed = value;
        if (nonzero ? (value & mask) != 0 : (value & mask) == wanted)
            return 0;
        result = clock_check(session, &now);
        if (result || now - start >= POLL_TIMEOUT_US)
            return POLARIS_SMU_LOAD_TIMEOUT;
    }
    return POLARIS_SMU_LOAD_TIMEOUT;
}
/* Why the previous response is only logged: Linux v6.12 smu7_send_msg_to_smc
 * (smu7_smumgr.c:171-181) waits for the previous response, prints it when it is
 * not 1 and goes on to clear the register and send its own message. A previous
 * message that failed or was unsupported (0xfe), or never answered, says nothing
 * about the message about to be sent, and failing here would stop a load that
 * the kernel driver carries on with. */
static void note_previous_response(struct session *session, uint32_t response)
{
    struct polaris_smu_loader *loader = session->loader;
    if (!loader->previous_response_anomalies)
        loader->previous_response_first = response;
    ++loader->previous_response_anomalies;
}

/* Why this order: wait for the previous response and log it, clear
 * SMC_RESP_0, write the argument and the message, wait again; only the response
 * to THIS message decides success. The read after the clear stays: a register
 * that still shows a response after being cleared cannot tell our answer from
 * the old one, so continuing would turn a stale ACK into a success. */
static int send_message(struct session *session, uint32_t message, uint32_t argument)
{
    uint32_t response = 0;
    /* SMU7 RESP is a 16-bit field; reserved upper bits are not completion.
     * read_register() rejects raw all-ones before masking, so an absent PCI
     * function cannot masquerade as an ordinary firmware response. */
    int result = wait_bits(session, RESPONSE, RESPONSE_MASK, 0, 0, 1, &response);
    if (result == POLARIS_SMU_LOAD_TIMEOUT)
        note_previous_response(session, 0);
    else if (result)
        return result;
    else if ((response & RESPONSE_MASK) != RESPONSE_OK)
        note_previous_response(session, response & RESPONSE_MASK);

    result = write_register(session, RESPONSE, 0);
    if (result)
        return result;
    result = read_register(session, RESPONSE, &response);
    if (result)
        return result;
    /* A response still set after clear is an old ACK, never this transaction. */
    if (response & RESPONSE_MASK)
        return POLARIS_SMU_LOAD_REJECTED;

    result = write_register(session, ARGUMENT, argument);
    if (result)
        return result;
    result = write_register(session, MESSAGE, message);
    if (result)
        return result;
    ++session->loader->messages;

    result = wait_bits(session, RESPONSE, RESPONSE_MASK, 0, 0, 1, &response);
    if (result)
        return result;
    return (response & RESPONSE_MASK) == RESPONSE_OK ? 0 : POLARIS_SMU_LOAD_REJECTED;
}
/* Why the duration is measured here: the upload is the one stretch of the load
 * that is not a wait, so its length is the evidence for how slow the register
 * path really is (every word is two config-checked MMIO writes). The clock is
 * read directly, not through clock_check, because the reset window is open. */
static int upload_firmware(struct session *session, const struct polaris_fw_view *firmware)
{
    /* Explicit addressing avoids leaking AUTO_INCREMENT state across SRAM and
     * register transactions after an interrupted upload. Raw firmware DWORDs
     * match Linux's native little-endian upload; reset jump is a separate BE
     * instruction word and is deliberately not encoded through this loop. */
    uint64_t started_us = session->ops->now_us(session->ops->opaque);
    int result = 0;
    for (uint32_t i = 0; i < firmware->bytes && !result; i += FIRMWARE_WORD_BYTES) {
        const uint8_t *word = firmware->ucode + i;
        uint32_t value = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 |
                         (uint32_t)word[3] << 24;
        result = write_smc(session, SMC_RAM_START + i, value);
        if (!result)
            ++session->loader->uploaded_dwords;
    }
    uint64_t finished_us = session->ops->now_us(session->ops->opaque);
    session->loader->upload_microseconds = finished_us > started_us ? finished_us - started_us : 0;
    return result;
}
/* Why a function of its own: a reset pulse (assert, then release) is never
 * interrupted by the whole-load limit, see reset_window_begin. */
static int pulse_smc_reset(struct session *session)
{
    reset_window_begin(session);
    int result = update_smc_bits(session, RESET, 1, 1);
    if (result)
        return result;
    result = update_smc_bits(session, RESET, 1, 0);
    if (result)
        return result;
    reset_window_end(session);
    return 0;
}

/* Why this order: Linux v6.12 polaris10_start_smu_in_non_protection_mode():
 * wait for the boot sequence, clear the interrupt flag, hold the SMC in reset,
 * upload the image, program the jump to address 0, enable the clock, release
 * reset. Everything from asserting to releasing reset is one reset window. */
static int boot_unprotected_firmware(struct session *session,
                                     const struct polaris_fw_view *firmware)
{
    int result = wait_bits(session, EVENTS, 0x80, 0, 1, 1, 0);
    if (result)
        return result;
    result = write_smc(session, FLAGS, 0);
    if (result)
        return result;
    reset_window_begin(session);
    result = update_smc_bits(session, RESET, 1, 1);
    if (result)
        return result;
    result = upload_firmware(session, firmware);
    if (result)
        return result;
    result = write_smc(session, 0, 0xe0008040);
    if (result)
        return result;
    result = update_smc_bits(session, CLOCK, 1, 0);
    if (result)
        return result;
    result = update_smc_bits(session, RESET, 1, 0);
    if (result)
        return result;
    reset_window_end(session);
    return 0;
}

/* Why this order: Linux v6.12 polaris10_start_smu_in_protection_mode(): hold
 * the SMC in reset, upload, clear SMU_STATUS, enable the clock, release reset,
 * wait for the boot sequence, send the authentication request SMU_TEST and
 * check SMU_STATUS, then pulse reset once more. The first reset window ends
 * when reset is released; the closing pulse is a window of its own. */
static int boot_protected_firmware(struct session *session, const struct polaris_fw_view *firmware)
{
    reset_window_begin(session);
    int result = update_smc_bits(session, RESET, 1, 1);
    if (result)
        return result;
    result = upload_firmware(session, firmware);
    if (result)
        return result;
    result = write_smc(session, STATUS, 0);
    if (result)
        return result;
    result = update_smc_bits(session, CLOCK, 1, 0);
    if (result)
        return result;
    result = update_smc_bits(session, RESET, 1, 0);
    if (result)
        return result;
    reset_window_end(session);
    result = wait_bits(session, EVENTS, 0x10000, 0x10000, 1, 0, 0);
    if (result)
        return result;
    result = send_message(session, SMU_TEST, 0x20000);
    if (result)
        return result;

    uint32_t status;
    result = wait_bits(session, STATUS, 1, 1, 1, 0, &status);
    if (result)
        return result;
    if (!(status & 2))
        return POLARIS_SMU_LOAD_REJECTED;
    result = write_smc(session, FLAGS, 0);
    if (result)
        return result;
    return pulse_smc_reset(session);
}

static int boot_firmware(struct session *session, const struct polaris_fw_view *firmware,
                         uint32_t protected_mode)
{
    /* These boot branches have different validation/reset ordering. Keep each
     * sequence visible instead of hiding side effects in a boolean chain. */
    int result;
    if (protected_mode)
        result = boot_protected_firmware(session, firmware);
    else
        result = boot_unprotected_firmware(session, firmware);
    if (result)
        return result;

    result = wait_bits(session, FLAGS, 1, 1, 1, 0, 0);
    if (!result)
        session->loader->started = 1;
    return result;
}
static int valid_cpu_span(const void *pointer, uint64_t bytes)
{
    return pointer && bytes && bytes <= UINTPTR_MAX &&
           (uintptr_t)pointer <= UINTPTR_MAX - (bytes - 1);
}

static int ranges_overlap(uint64_t first, uint64_t first_bytes,
                           uint64_t second, uint64_t second_bytes)
{
    return first <= second ? second - first < first_bytes : first - second < second_bytes;
}

static int valid_gpu_span(uint64_t address, uint64_t bytes)
{
    return !(address & 4095) && address < POLARIS_MEMORY_LIMIT &&
           bytes <= POLARIS_MEMORY_LIMIT - address;
}

struct load_mappings {
    uint8_t toc[POLARIS_SMU_TOC_BYTES];
    size_t count;
    volatile uint8_t *cpu[POLARIS_SMU_TOC_MAX_ENTRIES + 2];
    uint64_t address[POLARIS_SMU_TOC_MAX_ENTRIES + 2];
    uint64_t bytes[POLARIS_SMU_TOC_MAX_ENTRIES + 2];
};

static int validate_load_mappings(const struct polaris_smu_loader_ops *ops,
                                  const struct polaris_smu_load_request *request,
                                  struct load_mappings *mappings)
{
    struct polaris_smu_toc_info inventory;
    if (!request->smc || !valid_cpu_span(request->smc->ucode, request->smc->bytes) ||
        !request->smc->bytes || (request->smc->bytes & 3) || request->smc->bytes > 0x20000 ||
        request->smc->ucode_start_addr != 0x20000 || request->smc_security_key > 1 ||
        !valid_gpu_span(request->toc_gpu, 4096) ||
        !valid_gpu_span(request->scratch_gpu, POLARIS_SMU_SCRATCH_BYTES) ||
        request->image_count > POLARIS_SMU_TOC_MAX_ENTRIES ||
        polaris_smu_toc_encode(mappings->toc, sizeof mappings->toc, request->toc_gpu,
                               request->images, request->image_count, &inventory) ||
        inventory.missing_mask || (inventory.present_mask & 0x180) != 0x180)
        return POLARIS_SMU_LOAD_INVALID;

    /* TOC and scratch occupy the first two mappings; firmware follows in
     * directory order. All aliases are rejected before any mapped-byte write. */
    mappings->count = request->image_count + 2;
    mappings->address[0] = request->toc_gpu;
    mappings->bytes[0] = 4096;
    mappings->address[1] = request->scratch_gpu;
    mappings->bytes[1] = POLARIS_SMU_SCRATCH_BYTES;
    for (size_t index = 2; index < mappings->count; ++index) {
        mappings->address[index] = request->images[index - 2].gpu_addr;
        mappings->bytes[index] = request->images[index - 2].bytes;
    }
    for (size_t index = 0; index < mappings->count; ++index) {
        uint64_t address = mappings->address[index];
        uint64_t bytes = mappings->bytes[index];
        if (ops->resolve_mapping(ops->opaque, address, bytes, &mappings->cpu[index]) ||
            !valid_cpu_span((const void *)mappings->cpu[index], bytes) ||
            ranges_overlap((uintptr_t)mappings->cpu[index], bytes, (uintptr_t)request->smc->ucode,
                           request->smc->bytes))
            return POLARIS_SMU_LOAD_INVALID;
        for (size_t previous = 0; previous < index; ++previous) {
            if (ranges_overlap(address, bytes, mappings->address[previous],
                               mappings->bytes[previous]) ||
                ranges_overlap((uintptr_t)mappings->cpu[index], bytes,
                               (uintptr_t)mappings->cpu[previous], mappings->bytes[previous]))
                return POLARIS_SMU_LOAD_INVALID;
        }
    }
    return 0;
}

/* Why one function names the branch: the decision is what the preflight record
 * reports, and a record that could disagree with the code that acts on it would
 * be worse than none. */
static enum polaris_smu_start_branch choose_start_branch(int refused, int already_running,
                                                         uint32_t protected_mode)
{
    if (refused)
        return POLARIS_SMU_BRANCH_REFUSED;
    if (already_running)
        return POLARIS_SMU_BRANCH_WARM;
    return protected_mode ? POLARIS_SMU_BRANCH_COLD_PROTECTED
                          : POLARIS_SMU_BRANCH_COLD_UNPROTECTED;
}

/* Why the registers are handed out before anything is written: a hang inside
 * the boot sequence leaves the machine with nothing on the screen, so the values
 * the decision rests on, and the decision, reach the caller first
 * (report_preflight) and can be on the serial line before the first SMC write.
 * Linux v6.12 polaris10_start_smu() reads the same registers before it chooses
 * between the running, protected and unprotected sequences. */
static int prepare_firmware(struct session *session, const struct polaris_smu_load_request *request)
{
    uint32_t access_control, firmware_mode, clock, program_counter, flags, reset;
    /* Do not inherit an auto-incrementing indirect port from a previous owner. */
    int result = read_register(session, ACCESS, &access_control);
    if (result)
        return result;
    if (access_control & 0x800)
        return POLARIS_SMU_LOAD_UNSUPPORTED;

    result = read_smc(session, FIRMWARE, &firmware_mode);
    if (result)
        return result;
    result = read_smc(session, CLOCK, &clock);
    if (result)
        return result;
    result = read_smc(session, PC, &program_counter);
    if (result)
        return result;
    result = read_smc(session, FLAGS, &flags);
    if (result)
        return result;
    result = read_smc(session, RESET, &reset);
    if (result)
        return result;

    uint32_t security_key = (firmware_mode >> SMU_FIRMWARE_SEL_SHIFT) & 1u;
    int refused = (firmware_mode & ~SMU_FIRMWARE_KNOWN_BITS) ||
                  (firmware_mode & SMU_FIRMWARE_BUSY_BITS) ||
                  security_key != request->smc_security_key ||
                  program_counter == UINT32_MAX || program_counter >= SMC_RAM_END ||
                  clock == UINT32_MAX || reset == UINT32_MAX || flags == UINT32_MAX;
    uint32_t protected_mode = (firmware_mode >> SMU_FIRMWARE_MODE_SHIFT) & 1u;
    /* A running firmware must satisfy clock, PC, reset and interrupt state;
     * reading a single old flag is not a warm-start proof. */
    int already_running = !(clock & SMC_CLOCK_DISABLED_BIT) &&
                          program_counter >= SMC_PC_RUNNING_MINIMUM &&
                          program_counter < SMC_RAM_END && !(reset & SMC_IN_RESET_BIT);
    struct polaris_smu_loader *loader = session->loader;
    loader->preflight = (struct polaris_smu_preflight){
        .smu_firmware = firmware_mode,
        .clock_control = clock,
        .program_counter = program_counter,
        .reset_control = reset,
        .firmware_flags = flags,
        .branch = choose_start_branch(refused, already_running, protected_mode)
    };
    loader->preflight_valid = 1;
    if (session->ops->report_preflight)
        session->ops->report_preflight(session->ops->opaque, &loader->preflight);
    if (refused)
        return POLARIS_SMU_LOAD_UNSUPPORTED;

    loader->protected_mode = (uint8_t)protected_mode;
    loader->security_key = (uint8_t)security_key;
    if (already_running) {
        /* Do not reset a running PC just because its initialization interrupts
         * are delayed. */
        if (!(flags & FIRMWARE_INTERRUPTS_ENABLED_BIT))
            return wait_bits(session, FLAGS, 1, 1, 1, 0, 0);
        return 0;
    }
    return boot_firmware(session, request->smc, loader->protected_mode);
}

static int publish_firmware_images(struct session *session,
                                   const struct polaris_smu_load_request *request,
                                   const struct load_mappings *mappings)
{
    /* Only after complete mapping validation do we touch any mapped bytes.
     * Clearing scratch and TOC padding avoids giving SMU stale caller data. */
    for (uint64_t index = 0; index < mappings->bytes[0]; ++index)
        mappings->cpu[0][index] = index < sizeof mappings->toc ? mappings->toc[index] : 0;
    for (uint64_t index = 0; index < mappings->bytes[1]; ++index)
        mappings->cpu[1][index] = 0;
    for (size_t index = 0; index < mappings->count; ++index) {
        if (session->ops->sync_to_device(session->ops->opaque, mappings->address[index],
                                         mappings->bytes[index]))
            return POLARIS_SMU_LOAD_IO;
    }

    struct polaris_smu_loader *loader = session->loader;
    uint32_t load_status_address = loader->soft_regs + LOAD_STATUS_OFFSET;
    int result = write_smc(session, load_status_address, 0);
    if (result)
        return result;
    result = read_smc(session, load_status_address, &loader->load_status);
    if (result)
        return result;
    if (loader->load_status)
        return POLARIS_SMU_LOAD_REJECTED;

    result =
        send_message(session, SMU_SET_SCRATCH_ADDRESS_HIGH, (uint32_t)(request->scratch_gpu >> 32));
    if (result)
        return result;
    result = send_message(session, SMU_SET_SCRATCH_ADDRESS_LOW, (uint32_t)request->scratch_gpu);
    if (result)
        return result;
    result = send_message(session, SMU_SET_TOC_ADDRESS_HIGH, (uint32_t)(request->toc_gpu >> 32));
    if (result)
        return result;
    result = send_message(session, SMU_SET_TOC_ADDRESS_LOW, (uint32_t)request->toc_gpu);
    if (result)
        return result;
    result = send_message(session, SMU_LOAD_UCODES, POLARIS_SMU_TOC_REQUIRED_MASK);
    if (result)
        return result;
#ifndef POLARIS_SMU_LOADER_NEGCTL_STATUS
    result = wait_bits(session, load_status_address, POLARIS_SMU_TOC_REQUIRED_MASK,
                       POLARIS_SMU_TOC_REQUIRED_MASK, 1, 0, &loader->load_status);
    if (result)
        return result;
#endif
    return 0;
}

int polaris_smu_load(struct polaris_smu_loader *loader, const struct polaris_smu_loader_ops *ops,
                     const struct polaris_smu_load_request *request)
{
    struct load_mappings mappings;
    struct session session = {.loader = loader, .ops = ops};
    int result = POLARIS_SMU_LOAD_INVALID;
    if (!loader || !ops || !request || !ops->read_identity || !ops->read_reg || !ops->write_reg ||
        !ops->resolve_mapping || !ops->sync_to_device || !ops->now_us)
        return result;
    if (__atomic_exchange_n(&loader->lock, 1, __ATOMIC_ACQUIRE))
        return POLARIS_SMU_LOAD_BUSY;
    if (loader->quarantined) {
        result = POLARIS_SMU_LOAD_QUARANTINED;
        goto done;
    }
    if (loader->loaded)
        goto done;
    result = validate_load_mappings(ops, request, &mappings);
    if (result)
        goto done;

    uint32_t identity;
    if (ops->read_identity(ops->opaque, &identity) || !polaris10_identity_known(identity)) {
        result = POLARIS_SMU_LOAD_UNSUPPORTED;
        goto done;
    }
    session.started_us = session.last_us = ops->now_us(ops->opaque);
    result = prepare_firmware(&session, request);
    if (result)
        goto done;
    result = read_smc(&session, SOFT_POINTER, &loader->soft_regs);
    if (result)
        goto done;
    if (loader->soft_regs < 0x20000 || (loader->soft_regs & 3) ||
        loader->soft_regs > 0x40000 - LOAD_STATUS_OFFSET - 4) {
        result = POLARIS_SMU_LOAD_UNSUPPORTED;
        goto done;
    }
    result = publish_firmware_images(&session, request, &mappings);
    if (result)
        goto done;
    loader->loaded = 1;
done:
    if (result && session.write_attempted)
        loader->quarantined = 1;
    __atomic_store_n(&loader->lock, 0, __ATOMIC_RELEASE);
    return result;
}
