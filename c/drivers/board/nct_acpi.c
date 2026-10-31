/* Follow Linux v6.12 drivers/hwmon/nct6775-platform.c: sensors_nct6775_platform_init()
 * refuses direct I/O when acpi_check_resource_conflict() finds firmware use.
 * Source: https://github.com/torvalds/linux/blob/v6.12/drivers/hwmon/nct6775-platform.c
 * This is ownership policy, not an SMM lock: undocumented SMM users cannot be
 * discovered by walking AML. Dynamic regions created inside methods are also
 * absent until those methods execute (ACPI 6.5 section 19.6.99); this follows
 * the requested boot-time ownership policy, not a claim to exclude every
 * possible future AML access. Not verified on X79-VG4 hardware. */
#include <stddef.h>
#include "nct_acpi.h"
#include "board_io.h"
#include <uacpi/namespace.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/internal/types.h>

/* Compare half-open intervals without narrowing their exclusive endpoints.
 * Adjacent regions and zero-sized regions do not claim any common byte. */
int nct_region_conflict(const struct nct_io_region *regions, int count,
                        uint16_t base, uint16_t length)
{
    if (!length) {
        return 0;
    }
    uint32_t end = (uint32_t)base + length;
    for (int i = 0; i < count; i++) {
        uint32_t region_end = (uint32_t)regions[i].start + regions[i].len;
        if (regions[i].len && regions[i].start < end && base < region_end) {
            return 1;
        }
    }
    return 0;
}

struct region_scan {
    uint16_t base;
    uint16_t length;
    int conflict;
    uacpi_status error;
    const char *error_step;
};

/* Inspect each OperationRegion without evaluating methods or touching ports.
 * uACPI has no public region-address getter; its internal typed acquire/release
 * pins the object across the iterator's unlocked callback (namespace.c).
 * ACPI 6.5 section 19.6.99, OperationRegion: SystemIO is address-space ID 1.
 * https://uefi.org/specs/ACPI/6.5/19_ASL_Reference.html#operationregion-declare-operation-region
 */
static uacpi_iteration_decision inspect_region(void *user,
                                              uacpi_namespace_node *node,
                                              uacpi_u32 depth)
{
    struct region_scan *scan = user;
    uacpi_object *object;
    (void)depth;
    uacpi_status status = uacpi_namespace_node_acquire_object_typed(
        node, UACPI_OBJECT_OPERATION_REGION_BIT, &object);
    if (status != UACPI_STATUS_OK) {
        scan->error = status;
        scan->error_step = "object acquire";
        return UACPI_ITERATION_DECISION_BREAK;
    }

    uacpi_operation_region *region = object->op_region;
    uint64_t start = region->offset;
    uint64_t length = region->length;
    int system_io = region->space == UACPI_ADDRESS_SPACE_SYSTEM_IO;
    status = uacpi_namespace_node_release_object(object);
    if (status != UACPI_STATUS_OK) {
        scan->error = status;
        scan->error_step = "object release";
        return UACPI_ITERATION_DECISION_BREAK;
    }
    if (!system_io || !length || start >= 0x10000ull) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    /* SystemIO is the 16-bit x86 port space. Clip before adding: AML integers
     * are 64 bits and start+length can overflow. Stream one region at a time,
     * so a fixed collection capacity can never silently omit a late claimant.
     * A full 65536-byte span needs two entries in the public uint16_t format. */
    uint64_t available = 0x10000ull - start;
    if (length > available) {
        length = available;
    }
    struct nct_io_region ranges[2];
    int count = 1;
    ranges[0].start = (uint16_t)start;
    ranges[0].len = (uint16_t)(length > 0xffff ? 0xffff : length);
    if (length == 0x10000ull) {
        ranges[1].start = 0xffff;
        ranges[1].len = 1;
        count = 2;
    }
    if (!nct_region_conflict(ranges, count, scan->base, scan->length)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    kprintf("[nct] HWM I/O 0x%x claimed by ACPI OperationRegion %s: direct access disabled\n",
            scan->base, path ? path : "<path unavailable>");
    if (path) {
        uacpi_free_absolute_path(path);
    }
    scan->conflict = 1;
    return UACPI_ITERATION_DECISION_BREAK;
}

/* Caller holds the sleepable AML initialization mutex, never nct_lock. A
 * namespace error is not evidence of unclaimed ports, so fail closed.
 * Previously callback errors set the same flag as an actual conflict and
 * suppressed the failure log. Keep the uACPI status and failed step so an
 * incomplete scan explains its refusal without inventing firmware ownership. */
int nct_acpi_check(uint16_t base, uint16_t length)
{
    struct region_scan scan = {
        .base = base,
        .length = length,
    };
    uacpi_status status = uacpi_namespace_for_each_child(
        uacpi_namespace_root(), inspect_region, NULL,
        UACPI_OBJECT_OPERATION_REGION_BIT, UACPI_MAX_DEPTH_ANY, &scan);
    if (status != UACPI_STATUS_OK && scan.error == UACPI_STATUS_OK) {
        scan.error = status;
        scan.error_step = "namespace walk";
    }
    if (scan.error != UACPI_STATUS_OK) {
        kprintf("[nct] HWM I/O 0x%x ACPI ownership scan failed (%s status=%u): direct access disabled\n",
                base, scan.error_step, (unsigned)scan.error);
        return -5; /* LogitOS EIO: the ownership scan did not complete. */
    }
    if (scan.conflict) {
        return -16; /* LogitOS EBUSY: an actual SystemIO claimant was found. */
    }
    return 0;
}
