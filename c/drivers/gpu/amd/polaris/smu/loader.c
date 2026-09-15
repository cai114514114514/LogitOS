#include "loader.h"
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
#define LOAD_TIMEOUT_US 2000000u
#define POLL_TIMEOUT_US 100000u
#define POLL_LIMIT 100000u
#define RESPONSE_MASK 0xffffu

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
};
static int clock_check(struct session *session, uint64_t *now)
{
    *now = session->ops->now_us(session->ops->opaque);
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
static int send_message(struct session *session, uint32_t message, uint32_t argument)
{
    uint32_t response;
    /* SMU7 RESP is a 16-bit field; reserved upper bits are not completion.
     * read_register() rejects raw all-ones before masking, so an absent PCI
     * function cannot masquerade as an ordinary firmware response. */
    int result = wait_bits(session, RESPONSE, RESPONSE_MASK, 0, 0, 1, &response);
    if (result)
        return result;
    if ((response & RESPONSE_MASK) != 1)
        return POLARIS_SMU_LOAD_REJECTED;

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
    return (response & RESPONSE_MASK) == 1 ? 0 : POLARIS_SMU_LOAD_REJECTED;
}
static int upload_firmware(struct session *session, const struct polaris_fw_view *firmware)
{
    /* Explicit addressing avoids leaking AUTO_INCREMENT state across SRAM and
     * register transactions after an interrupted upload. Raw firmware DWORDs
     * match Linux's native little-endian upload; reset jump is a separate BE
     * instruction word and is deliberately not encoded through this loop. */
    for (uint32_t i = 0; i < firmware->bytes; i += 4) {
        const uint8_t *word = firmware->ucode + i;
        uint32_t value = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 |
                         (uint32_t)word[3] << 24;
        int result = write_smc(session, 0x20000 + i, value);
        if (result)
            return result;
        ++session->loader->uploaded_dwords;
    }
    return 0;
}
static int boot_unprotected_firmware(struct session *session,
                                     const struct polaris_fw_view *firmware)
{
    int result = wait_bits(session, EVENTS, 0x80, 0, 1, 1, 0);
    if (result)
        return result;
    result = write_smc(session, FLAGS, 0);
    if (result)
        return result;
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
    return update_smc_bits(session, RESET, 1, 0);
}

static int boot_protected_firmware(struct session *session, const struct polaris_fw_view *firmware)
{
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
    result = update_smc_bits(session, RESET, 1, 1);
    if (result)
        return result;
    return update_smc_bits(session, RESET, 1, 0);
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

    if ((firmware_mode & ~0x30f1fu) || (firmware_mode & 0x19) ||
        ((firmware_mode >> 17) & 1) != request->smc_security_key || program_counter == UINT32_MAX ||
        program_counter >= 0x40000 || clock == UINT32_MAX || reset == UINT32_MAX ||
        flags == UINT32_MAX)
        return POLARIS_SMU_LOAD_UNSUPPORTED;

    session->loader->protected_mode = (uint8_t)((firmware_mode >> 16) & 1);
    session->loader->security_key = (uint8_t)((firmware_mode >> 17) & 1);
    if (!(clock & 1) && program_counter >= 0x20100 && program_counter < 0x40000 && !(reset & 1)) {
        /* A running firmware must satisfy clock, PC, reset and interrupt state;
         * reading a single old flag is not a warm-start proof. Do not reset a
         * running PC just because its initialization interrupts are delayed. */
        if (!(flags & 1))
            return wait_bits(session, FLAGS, 1, 1, 1, 0, 0);
        return 0;
    }
    return boot_firmware(session, request->smc, session->loader->protected_mode);
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
    if (ops->read_identity(ops->opaque, &identity) || identity != 0x67df1002) {
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
