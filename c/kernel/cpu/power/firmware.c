#include "firmware.h"
#include "hardware.h"
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>

/* ACPI 6.4 8.4.4 (_CST) and 8.4.6 (_PSS/_PCT/_PPC):
 * https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/08_Processor_Configuration_and_Control/declaring-processors.html
 * Intel FFH encoding (Register bit offset 2 means native MWAIT):
 * https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/processor-vendor-specific-acpi-specification.pdf
 * Independent parser; no Linux/uACPI implementation copied. AML execution
 * stays in the existing uACPI interpreter. Not verified on X79-VG4 hardware. */
struct firmware_policy {
    struct power_pss pss[POWER_STATES];
    struct power_cstate cst[POWER_STATES];
    unsigned npss, ncst;
    int invalid_pss;
};
static struct firmware_policy tables[POWER_CPUS];

static int integer_at(const uacpi_object_array *p, unsigned index, uint64_t *value)
{
    return index < p->count &&
           uacpi_object_get_integer(p->objects[index], value) == UACPI_STATUS_OK;
}
static int fixed_register(uacpi_object *o, uacpi_data_view *view)
{
    if (uacpi_object_get_string_or_buffer(o, view) != UACPI_STATUS_OK || view->length < 15)
        return 0;
    const uint8_t *b = view->const_bytes;
    return b[0] == 0x82 && b[1] == 12 && b[2] == 0 && b[3] == 0x7f;
}

static int read_pss(uacpi_namespace_node *node, struct firmware_policy *t)
{
    uacpi_object *result = NULL;
    uacpi_status status = uacpi_eval(node, "_PSS", NULL, &result);
    if (status == UACPI_STATUS_NOT_FOUND) return 0;
    if (status != UACPI_STATUS_OK) return -1;
    uacpi_object_array rows;
    int ok = uacpi_object_get_package(result, &rows) == UACPI_STATUS_OK &&
             rows.count && rows.count <= POWER_STATES;
    for (unsigned i = 0; ok && i < rows.count; i++) {
        uacpi_object_array row; uint64_t values[6];
        ok = uacpi_object_get_package(rows.objects[i], &row) == UACPI_STATUS_OK && row.count == 6;
        for (unsigned j = 0; ok && j < 6; j++) ok = integer_at(&row, j, &values[j]);
        if (!ok) break;
        /* Frequency is informational: a turbo PSS may say 2501MHz for a
         * 2600MHz control ratio. Deriving Control from frequency is wrong. */
        if (!values[0] || values[4] > 0xffff || !(values[4] >> 8) || (values[4] & 255)) { ok = 0; break; }
        t->pss[i] = (struct power_pss){values[4] >> 8, values[4]};
    }
    if (ok) t->npss = rows.count;
    uacpi_object_unref(result);
    if (!ok) return -1;

    /* An IO-port _PCT cannot be driven by writing PERF_CTL, even if its _PSS
     * control numbers happen to resemble ratios. Do not silently derive a
     * fallback table for malformed/unsupported firmware. */
    result = NULL;
    status = uacpi_eval(node, "_PCT", NULL, &result);
    ok = status == UACPI_STATUS_OK &&
         uacpi_object_get_package(result, &rows) == UACPI_STATUS_OK && rows.count == 2;
    for (unsigned i = 0; ok && i < 2; i++) {
        uacpi_data_view view; ok = fixed_register(rows.objects[i], &view);
    }
    uacpi_object_unref(result);
    if (!ok) return -1;

    result = NULL;
    status = uacpi_eval(node, "_PPC", NULL, &result);
    if (status == UACPI_STATUS_OK) {
        uint64_t limit;
        ok = uacpi_object_get_integer(result, &limit) == UACPI_STATUS_OK && limit < t->npss;
        if (ok) {
            for (unsigned i = 0; i < t->npss - limit; i++) t->pss[i] = t->pss[i + limit];
            t->npss -= limit;
        }
        uacpi_object_unref(result);
        if (!ok) return -1;
    } else if (status != UACPI_STATUS_NOT_FOUND) return -1;
    return 0;
}

static void read_cst(uacpi_namespace_node *node, struct firmware_policy *t)
{
    uacpi_object *result = NULL;
    if (uacpi_eval(node, "_CST", NULL, &result) != UACPI_STATUS_OK) return;
    uacpi_object_array rows;
    uint64_t count;
    int ok = uacpi_object_get_package(result, &rows) == UACPI_STATUS_OK &&
             integer_at(&rows, 0, &count) && count <= POWER_STATES && count + 1 == rows.count;
    for (unsigned i = 1; ok && i <= count; i++) {
        uacpi_object_array row; uacpi_data_view gas;
        uint64_t type, latency, power;
        if (uacpi_object_get_package(rows.objects[i], &row) != UACPI_STATUS_OK || row.count != 4 ||
            !integer_at(&row, 1, &type) || !integer_at(&row, 2, &latency) ||
            !integer_at(&row, 3, &power)) { ok = 0; break; }
        if (!fixed_register(row.objects[0], &gas)) continue;
        const uint8_t *b = gas.const_bytes;
        /* Intel _CST FFH vendor ID 1, class 2 (MWAIT). AccessSize bit 1
         * requests bus-master checks we do not implement: refuse that state
         * rather than enter a deep C-state while DMA still needs coherency. */
        if (b[4] != 1 || b[5] != 2 || (b[6] & 2) || !type || type > 255 || latency > UINT32_MAX)
            continue;
        uint64_t hint = 0;
        for (unsigned j = 0; j < 8; j++) hint |= (uint64_t)b[7 + j] << (j * 8);
        if (hint > 255) continue;
        t->cst[t->ncst++] = (struct power_cstate){type, latency, hint};
    }
    if (!ok) t->ncst = 0;
    uacpi_object_unref(result);
}

int cpu_power_load_acpi(unsigned cpu, const char *path)
{
    if (cpu >= POWER_CPUS || !path ||
        uacpi_get_current_init_level() < UACPI_INIT_LEVEL_NAMESPACE_INITIALIZED) return -1;
    uacpi_namespace_node *node;
    if (uacpi_namespace_node_find(NULL, path, &node) != UACPI_STATUS_OK) return -1;
    struct firmware_policy t = {0};
    t.invalid_pss = read_pss(node, &t) != 0;
    if (t.invalid_pss) t.npss = 0;
    /* Correction: this used to read _CST only when _PSS/_PCT was usable.
     * ACPI defines independent idle and frequency facilities (sections cited
     * above); an unsupported frequency register must not disable valid idle
     * states. Keep the frequency refusal while publishing _CST independently. */
    read_cst(node, &t);
    tables[cpu] = t;
    return t.invalid_pss ? -1 : 0;
}

unsigned power_get_pss(unsigned cpu, struct power_pss *out)
{
    if (cpu >= POWER_CPUS) return POWER_STATES + 1;
    const struct firmware_policy *t = &tables[cpu];
    if (t->invalid_pss) return POWER_STATES + 1;
    for (unsigned i = 0; i < t->npss; i++) out[i] = t->pss[i];
    return t->npss;
}
unsigned power_get_cst(unsigned cpu, struct power_cstate *out)
{
    if (cpu >= POWER_CPUS) return 0;
    const struct firmware_policy *t = &tables[cpu];
    for (unsigned i = 0; i < t->ncst; i++) out[i] = t->cst[i];
    return t->ncst;
}
