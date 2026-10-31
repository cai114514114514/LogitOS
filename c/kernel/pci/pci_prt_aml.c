/* ACPI PCI routing boundary: this is the only PCI file including uACPI.
 * Sources: ACPI 6.5 sections 5.8.1, 6.2.13, 6.4.2.1, 6.4.3.6;
 * Linux v6.12 drivers/acpi/{bus.c,pci_irq.c,pci_link.c}, drivers/pci/pci.c.
 * https://github.com/torvalds/linux/tree/v6.12/drivers/acpi
 * All current evidence is synthetic AML or QEMU, not verified on X79-VG4 hardware.
 */
#include "pci_prt_aml.h"
#include "pci.h"
#include "driver.h"
#include "smp.h"
#include "kprintf.h"
#include "io_domain.h"
#include <string.h>
#include <stddef.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

/* The native acpi.h and uACPI both define struct acpi_gas with different
 * layout contracts. This boundary needs only the ISA override accessor;
 * importing the native GAS into uACPI's packed FADT would corrupt offsets. */
extern uint32_t acpi_gsi_for_irq(int isa_irq);

/* Bounds are storage policy, not claims about firmware capacity. Cache misses
 * beyond these bounds still evaluate _PRT; topology walks have a 32-hop cycle
 * guard matching the kernel's bounded PCI enumeration depth. */
enum { ROOT_CAPACITY = 16, TABLE_CAPACITY = 16, LINK_CAPACITY = 64,
       BRIDGE_DEPTH = 32, DUMP_LINES = 256 };
struct prt_root {
    uacpi_namespace_node *node;
    uint8_t bus;
};
struct prt_table {
    uacpi_namespace_node *node;
    uacpi_pci_routing_table *table;
    uacpi_status status;
};
struct prt_link {
    uacpi_namespace_node *node;
    uint32_t index;
    uint32_t irq;
    uint32_t gsi;
    uint8_t level, low;
};
static struct prt_root roots[ROOT_CAPACITY];
static struct prt_table tables[TABLE_CAPACITY];
static struct prt_link links[LINK_CAPACITY];
static unsigned root_count, table_count, link_count;
static unsigned irq_users[16];
static int roots_discovered, ioapic_model_set;
static unsigned ready_once;
static struct io_domain route_owner = IO_DOMAIN_INIT;

/* Cache lifetime is the selected interrupt model. A model change must never
 * leave a previously evaluated PIC _PRT masquerading as an APIC table. */
static void clear_tables(void)
{
    for (unsigned i = 0; i < table_count; i++) {
        if (tables[i].table) {
            uacpi_free_pci_routing_table(tables[i].table);
        }
    }
    table_count = 0;
}

/* uACPI source/utilities.c uacpi_set_interrupt_model requires NAMESPACE_LOADED
 * and accepts an absent _PIC. Invoke before namespace_initialize runs _INI,
 * as Linux v6.12 drivers/acpi/bus.c acpi_bus_init_irq does before scanning.
 * PIC fallback must not tell firmware that IOAPIC routing is available. */
int pci_prt_aml_set_model(int ioapic)
{
    IO_DOMAIN_GUARD(&route_owner);
    clear_tables();
    ioapic_model_set = 0;
    if (!ioapic) {
        kprintf("[prt] _PIC skipped: PIC fallback active\n");
        return 0;
    }
    uacpi_status status = UACPI_STATUS_OK;
#ifndef PRT_NEGCTL_NO_PIC_CALL
    status = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
#endif
    if (status != UACPI_STATUS_OK) {
        kprintf("[prt] _PIC(1) failed status=%u\n", (unsigned)status);
        return -1;
    }
    ioapic_model_set = 1;
    kprintf("[prt] _PIC(1) ok\n");
    return 0;
}

/* Presence of LAPIC alone is insufficient: smp's committed external interrupt
 * handoff is authoritative, including the verified-mask PIC fallback path. */
int pci_prt_aml_preinit(void)
{
    return pci_prt_aml_set_model(smp_irq_via_apic());
}

/* Discover segment-zero root bridges. ACPI 6.5 6.5.5 _BBN defaults to zero
 * when absent; unsupported segments must not alias segment zero silently. */
static uacpi_iteration_decision add_root(void *user, uacpi_namespace_node *node,
                                        uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    uacpi_u64 segment = 0, bus = 0;
    uacpi_status status = uacpi_eval_integer(node, "_SEG", NULL, &segment);
    if ((status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) || segment) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    status = uacpi_eval_integer(node, "_BBN", NULL, &bus);
    if ((status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) || bus > 255) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    for (unsigned i = 0; i < root_count; i++) {
        if (roots[i].node == node || roots[i].bus == bus) {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    if (root_count < ROOT_CAPACITY) {
        roots[root_count++] = (struct prt_root){node, (uint8_t)bus};
    } else {
        kprintf("[prt] root bridge skipped: root table full\n");
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Match both PCI Express and conventional PCI roots, including _CID matches.
 * The discovery callback deduplicates roots advertising both identities. */
static void discover_roots(void)
{
    if (roots_discovered) {
        return;
    }
    roots_discovered = 1;
    uacpi_find_devices("PNP0A08", add_root, NULL);
    uacpi_find_devices("PNP0A03", add_root, NULL);
}

/* Find the namespace device owning a bus, walking actual PCI bridges rather
 * than guessing namespace names such as RP01. Missing nodes are legitimate:
 * resolution can still swizzle upwards through the injected PCI topology. */
static uacpi_namespace_node *bus_node(const struct pci_prt_topology *topology,
                                      uint8_t bus, unsigned depth)
{
    if (depth == BRIDGE_DEPTH) {
        return NULL;
    }
    for (unsigned i = 0; i < root_count; i++) {
        if (roots[i].bus == bus) {
            return roots[i].node;
        }
    }
    uint8_t parent, slot, function;
    if (!topology || !topology->bridge_of ||
        topology->bridge_of(topology->ctx, bus, &parent, &slot, &function) != 0 || parent == bus) {
        return NULL;
    }
    uacpi_namespace_node *owner = bus_node(topology, parent, depth + 1);
    if (!owner) {
        return NULL;
    }
    uacpi_namespace_node *child = NULL;
    while (uacpi_namespace_node_next_typed(owner, &child, UACPI_OBJECT_DEVICE_BIT) == UACPI_STATUS_OK && child) {
        uacpi_u64 address;
        if (uacpi_eval_adr(child, &address) == UACPI_STATUS_OK &&
            address == ((uint32_t)slot << 16 | function)) {
            return child;
        }
    }
    return NULL;
}

/* Cache successful tables and absent/error statuses. A full cache still
 * evaluates and explicitly returns temporary ownership to the caller. */
static uacpi_status get_table(uacpi_namespace_node *node,
                               uacpi_pci_routing_table **out, int *temporary)
{
    *out = NULL;
    *temporary = 0;
    for (unsigned i = 0; i < table_count; i++) {
        if (tables[i].node == node) {
            *out = tables[i].table;
            return tables[i].status;
        }
    }
    uacpi_status status = uacpi_get_pci_routing_table(node, out);
    if (table_count < TABLE_CAPACITY) {
        tables[table_count++] = (struct prt_table){node, *out, status};
    } else {
        *temporary = 1;
    }
    return status;
}

struct irq_descriptor {
    uint32_t wanted, seen;
    uacpi_resource *resource;
};

/* Correction: this previously counted IRQ/Extended IRQ records only, following
 * the task's mistaken premise. ACPI 6.4 section 6.2.13 defines SourceIndex as
 * the descriptor index in the COMPLETE resource template. A preceding FixedIO
 * record changes the index even though it cannot itself route an interrupt.
 * https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/06_Device_Configuration/Device_Configuration.html#prt-pci-routing-table
 * Stop at the requested descriptor and validate its type; never skip forward
 * to an unrelated IRQ when firmware points at a non-interrupt resource. */
static uacpi_iteration_decision find_descriptor(void *user, uacpi_resource *resource)
{
    struct irq_descriptor *descriptor = user;
#ifdef PRT_NEGCTL_IRQ_ONLY_INDEX
    if (resource->type != UACPI_RESOURCE_TYPE_IRQ &&
        resource->type != UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
#endif
    if (descriptor->seen++ == descriptor->wanted) {
        if (resource->type == UACPI_RESOURCE_TYPE_IRQ ||
            resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
            descriptor->resource = resource;
        }
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Keep all resource walking in the interpreter's validated iterator. */
static uacpi_resource *interrupt_resource(uacpi_resources *resources, uint32_t index)
{
    if (!resources) {
        return NULL;
    }
    struct irq_descriptor descriptor = {.wanted = index};
    uacpi_for_each_resource(resources, find_descriptor, &descriptor);
    return descriptor.resource;
}

/* Convert descriptor semantics, including active-both as low. Extended IRQs
 * are GSIs even below 16; applying an ISA override to them is silently wrong.
 * ACPI 6.5 sections 6.4.2.1 and 6.4.3.6 define these distinct namespaces. */
static int read_interrupt(uacpi_resource *resource, struct prt_link *link)
{
    if (!resource) {
        return -1;
    }
    if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
        if (!resource->irq.num_irqs || !resource->irq.irqs[0]) {
            return -1;
        }
        link->irq = resource->irq.irqs[0];
        link->gsi = acpi_gsi_for_irq((int)link->irq);
        link->level = resource->irq.triggering == UACPI_TRIGGERING_LEVEL;
        link->low = resource->irq.polarity != UACPI_POLARITY_ACTIVE_HIGH;
    } else {
        if (!resource->extended_irq.num_irqs) {
            return -1;
        }
        link->irq = resource->extended_irq.irqs[0];
        link->gsi = link->irq;
        link->level = resource->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        link->low = resource->extended_irq.polarity != UACPI_POLARITY_ACTIVE_HIGH;
    }
    return 0;
}

/* Local allocation policy follows Linux v6.12 pci_link.c acpi_irq_penalty:
 * do not steal timer/keyboard/cascade/RTC/FPU, SCI, or PS/2 mouse lines.
 * Ties choose the smallest usable IRQ, then share the least-used link IRQ. */
static int choose_irq(uacpi_resource *resource)
{
    struct acpi_fadt *fadt = NULL;
    unsigned sci = 9; /* Conservative conventional-PC default if FADT is absent. */
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK) {
        sci = fadt->sci_int;
    }
    int best = -1;
    unsigned count = resource->type == UACPI_RESOURCE_TYPE_IRQ ?
        resource->irq.num_irqs : resource->extended_irq.num_irqs;
    for (unsigned i = 0; i < count; i++) {
        uint32_t irq = resource->type == UACPI_RESOURCE_TYPE_IRQ ?
            resource->irq.irqs[i] : resource->extended_irq.irqs[i];
        if (irq >= 16 || irq == 0 || irq == 1 || irq == 2 || irq == 8 ||
            irq == 13 || irq == 12 || irq == sci) {
            continue;
        }
        if (best < 0 || irq_users[irq] < irq_users[best] ||
            (irq_users[irq] == irq_users[best] && irq < (unsigned)best)) {
            best = (int)irq;
        }
    }
    return best;
}

struct possible_irq {
    uint32_t wanted;
    uint32_t seen;
    uint32_t dependent_base;
    int dependent;
    int invalid;
    int irq;
    uacpi_resource *alternative;
    uacpi_resource *first_alternative;
    uacpi_resource *selected_alternative;
    uacpi_resource *resource;
};

/* Correction: indexing the raw _PRS with _CRS SourceIndex rejected a valid
 * StartDependentFn at that position. ACPI 6.4 sections 6.4.2.3-4 describe
 * alternatives: each Start begins another configuration, one End closes the
 * set, and neither delimiter appears in the selected _CRS configuration.
 * Count ALL actual descriptors within each configuration, including FixedIO,
 * but restart at the common prefix for each alternative. Do not change the
 * complete-template indexing used by interrupt_resource() for _CRS.
 * https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/06_Device_Configuration/Device_Configuration.html#start-dependent-functions-descriptor
 * The existing IRQ-use policy ranks candidates across alternatives; equal
 * candidates retain firmware order. Correction: the old allocator left
 * unrelated _CRS resources fixed, silently combining IRQ 3 from a 0x320
 * alternative with the old 0x300 FixedIO window. The selected alternative
 * must supply the entire _SRS template, including interrupt attributes. */
static uacpi_iteration_decision find_possible_irq(void *user, uacpi_resource *resource)
{
    struct possible_irq *possible = user;
    if (resource->type == UACPI_RESOURCE_TYPE_START_DEPENDENT) {
        if (!possible->dependent) {
            if (possible->first_alternative) {
                possible->invalid = 1;
            }
            possible->first_alternative = resource;
            possible->dependent_base = possible->seen;
        }
        possible->dependent = 1;
        possible->alternative = resource;
        possible->seen = possible->dependent_base;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (resource->type == UACPI_RESOURCE_TYPE_END_DEPENDENT) {
        if (!possible->dependent) {
            possible->invalid = 1;
        }
        possible->dependent = 0;
        possible->alternative = NULL;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (resource->type == UACPI_RESOURCE_TYPE_END_TAG) {
        if (possible->dependent) {
            possible->invalid = 1;
        }
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (possible->seen++ != possible->wanted ||
        (resource->type != UACPI_RESOURCE_TYPE_IRQ &&
         resource->type != UACPI_RESOURCE_TYPE_EXTENDED_IRQ)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    int irq = choose_irq(resource);
    if (irq >= 0 && (possible->irq < 0 || irq_users[irq] < irq_users[possible->irq] ||
                    (irq_users[irq] == irq_users[possible->irq] && irq < possible->irq))) {
        possible->irq = irq;
        possible->resource = resource;
        possible->selected_alternative = possible->alternative;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

struct possible_template {
    uacpi_resources resources;
    uacpi_resource *selected;
    uacpi_resource *alternative;
    uacpi_resource *chosen_alternative;
    uacpi_resource *chosen_resource;
    int invalid;
};

/* ACPI 6.4 section 6.4.2.3: _SRS takes common resources and ONE complete
 * dependent configuration, without the _PRS alternative markers. Never
 * splice a possible IRQ into _CRS, even when that template is readable.
 * This link allocator cannot assign I/O/memory ranges or a second IRQ set;
 * refuse such configurations rather than pass unresolved choices to _SRS.
 * FixedIO and already singular IRQ resources need no further arbitration.
 * Borrowed descriptor strings stay valid until the original _PRS is freed. */
static uacpi_iteration_decision copy_possible_template(void *user, uacpi_resource *resource)
{
    struct possible_template *copy = user;
    if (resource->type == UACPI_RESOURCE_TYPE_START_DEPENDENT) {
        copy->alternative = resource;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (resource->type == UACPI_RESOURCE_TYPE_END_DEPENDENT) {
        copy->alternative = NULL;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (copy->alternative && copy->alternative != copy->chosen_alternative) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (resource != copy->chosen_resource &&
        resource->type != UACPI_RESOURCE_TYPE_FIXED_IO &&
        resource->type != UACPI_RESOURCE_TYPE_END_TAG &&
        !(resource->type == UACPI_RESOURCE_TYPE_IRQ && resource->irq.num_irqs == 1) &&
        !(resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
          resource->extended_irq.num_irqs == 1)) {
        copy->invalid = 1;
    }
    uacpi_resource *destination = (uacpi_resource *)
        ((uint8_t *)copy->resources.entries + copy->resources.length);
    memcpy(destination, resource, resource->length);
    copy->resources.length += resource->length;
    if (resource == copy->chosen_resource) {
        copy->selected = destination;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Preserve every unrelated descriptor and its order for _SRS (ACPI 6.5
 * 6.2.16). Enlarge only the selected native record if an empty IRQ array has
 * no room for its first value. Source strings remain borrowed until _SRS
 * returns, while the original resources are still alive. */
static int set_link_irq(uacpi_namespace_node *node, uacpi_resources *template,
                         uacpi_resource *selected, unsigned irq)
{
    size_t needed = selected->type == UACPI_RESOURCE_TYPE_IRQ ?
        offsetof(uacpi_resource, irq.irqs) + sizeof(uacpi_u8) :
        offsetof(uacpi_resource, extended_irq.irqs) + sizeof(uacpi_u32);
    size_t alignment = _Alignof(uacpi_resource);
    needed = (needed + alignment - 1) & ~(alignment - 1);
    if (needed < selected->length) {
        needed = selected->length;
    }
    size_t extra = needed - selected->length;
    size_t offset = (uint8_t *)selected - (uint8_t *)template->entries;
    uacpi_resources copy = {.length = template->length + extra};
    copy.entries = uacpi_kernel_alloc(copy.length);
    if (!copy.entries) {
        return -1;
    }
    uint8_t *bytes = (uint8_t *)copy.entries;
    memcpy(bytes, template->entries, offset + selected->length);
    memset(bytes + offset + selected->length, 0, extra);
    memcpy(bytes + offset + needed, (uint8_t *)selected + selected->length,
           template->length - offset - selected->length);
    uacpi_resource *changed = (uacpi_resource *)(bytes + offset);
    changed->length = (uacpi_u32)needed;
    if (changed->type == UACPI_RESOURCE_TYPE_IRQ) {
        changed->irq.num_irqs = 1;
        changed->irq.irqs[0] = (uacpi_u8)irq;
    } else {
        changed->extended_irq.num_irqs = 1;
        changed->extended_irq.irqs[0] = irq;
    }
    uacpi_status status = UACPI_STATUS_OK;
#ifndef PRT_NEGCTL_LINK_SKIP_SRS
    status = uacpi_set_resources(node, &copy);
#else
    (void)node;
#endif
    uacpi_kernel_free(copy.entries);
    return status == UACPI_STATUS_OK ? 0 : -1;
}

/* _SRS replaces the WHOLE link resource template, not just SourceIndex (ACPI
 * 6.4 sections 6.2.13 and 6.2.16). Compare semantic fields rather than native
 * padding/pointers: uACPI rebuilds these records on every _CRS evaluation.
 * A resolved sibling can already have live PCI consumers; changing its IRQ,
 * sharing, wake or source attributes here would silently invalidate them. */
static int same_interrupt(uacpi_resource *current, uacpi_resource *proposed)
{
    if (!current || !proposed || current->type != proposed->type) {
        return 0;
    }
    if (current->type == UACPI_RESOURCE_TYPE_IRQ) {
        const uacpi_resource_irq *before = &current->irq;
        const uacpi_resource_irq *after = &proposed->irq;
        return before->num_irqs == 1 && after->num_irqs == 1 &&
            before->length_kind == after->length_kind &&
            before->triggering == after->triggering && before->polarity == after->polarity &&
            before->sharing == after->sharing && before->wake_capability == after->wake_capability &&
            before->irqs[0] == after->irqs[0];
    }
    const uacpi_resource_extended_irq *before = &current->extended_irq;
    const uacpi_resource_extended_irq *after = &proposed->extended_irq;
    return before->num_irqs == 1 && after->num_irqs == 1 &&
        before->direction == after->direction &&
        before->triggering == after->triggering && before->polarity == after->polarity &&
        before->sharing == after->sharing && before->wake_capability == after->wake_capability &&
        before->irqs[0] == after->irqs[0] &&
        before->source.index_present == after->source.index_present &&
        before->source.index == after->source.index && before->source.length == after->source.length &&
        (!before->source.length || (before->source.string && after->source.string &&
         !memcmp(before->source.string, after->source.string, before->source.length)));
}

/* Correction: copying a complete _PRS alternative previously changed cached
 * siblings behind their consumers: _CRS=[IRQ10,empty], _PRS=[IRQ11,{3,4}]
 * assigned index 1 to IRQ3 while index 0 still routed consumers to cached 10.
 * Refuse before ANY _SRS write if a used sibling would change or its current
 * resource no longer matches our cache. Consumer migration is deliberately
 * absent here: only pci_intx_reroute_all owns release/rebind lifetimes. */
static int preserves_link_users(uacpi_namespace_node *node, uacpi_resources *current,
                               uacpi_resources *proposed)
{
    for (unsigned i = 0; i < link_count; i++) {
        if (links[i].node != node) {
            continue;
        }
        uacpi_resource *before = interrupt_resource(current, links[i].index);
        uacpi_resource *after = interrupt_resource(proposed, links[i].index);
        struct prt_link live = {0};
        if (!same_interrupt(before, after) || read_interrupt(before, &live) != 0 ||
            live.irq != links[i].irq || live.gsi != links[i].gsi ||
            live.level != links[i].level || live.low != links[i].low) {
            return 0;
        }
    }
    return 1;
}

/* Build one complete _PRS configuration before programming the link. Keeping
 * this ownership boundary explicit prevents a readable _CRS from becoming an
 * accidental template for an IRQ selected from a different alternative. */
static int set_possible_link(uacpi_namespace_node *node, uacpi_resources *possible,
                             const struct possible_irq *choice, uacpi_resources *current)
{
    struct possible_template copy = {
        .chosen_alternative = choice->selected_alternative ?
            choice->selected_alternative : choice->first_alternative,
        .chosen_resource = choice->resource,
    };
    /* Removing delimiters and unchosen alternatives only shrinks the native
     * template, so the original length bounds this shallow descriptor copy. */
    copy.resources.entries = uacpi_kernel_alloc(possible->length);
    if (!copy.resources.entries) {
        return -1;
    }
    uacpi_for_each_resource(possible, copy_possible_template, &copy);
    int result = -1;
    int preserves_users = preserves_link_users(node, current, &copy.resources);
#ifdef PRT_NEGCTL_LINK_CHANGE_SIBLING
    preserves_users = 1;
#endif
    if (!copy.invalid && copy.selected &&
        preserves_users &&
        interrupt_resource(&copy.resources, choice->wanted) == copy.selected) {
        result = set_link_irq(node, &copy.resources, copy.selected, (unsigned)choice->irq);
    }
    uacpi_kernel_free(copy.resources.entries);
    return result;
}

/* Resolve or allocate a link once per descriptor, never once per PCI member.
 * Correction: checking only _STA present accepted disabled links with stale
 * _CRS. ACPI 6.4 section 6.3.7 requires BOTH present (bit 0) and enabled
 * (bit 1) for resource decoding:
 * https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/06_Device_Configuration/Device_Configuration.html#sta-device-status
 * Deliberately refuse disabled links rather than guess a firmware enable
 * sequence; PCI retains its fallback. uACPI supplies defaults if _STA is absent.
 * _SRS success alone is not evidence: re-read _CRS AND the enable status. */
static int resolve_link(uacpi_namespace_node *node, uint32_t index, struct prt_link *out,
                        int log_failure)
{
#ifdef PRT_NEGCTL_DUMP_UNBUDGETED
    log_failure = 1;
#endif
    uacpi_u32 flags;
    if (uacpi_eval_sta(node, &flags) != UACPI_STATUS_OK || (flags & 3) != 3) {
        return -1;
    }
    for (unsigned i = 0; i < link_count; i++) {
        if (links[i].node == node && links[i].index == index) {
            *out = links[i];
            return 0;
        }
    }
    /* Refuse new allocations when accounting is full rather than silently
     * undercounting IRQ usage on every subsequent lookup. */
    if (link_count == LINK_CAPACITY) {
        return -1;
    }
    struct prt_link link = {.node = node, .index = index};
    uacpi_resources *current = NULL, *possible = NULL;
    uacpi_get_current_resources(node, &current);
    uacpi_resource *resource = interrupt_resource(current, index);
    int result = read_interrupt(resource, &link);
    if (result != 0) {
        if (uacpi_get_possible_resources(node, &possible) != UACPI_STATUS_OK) {
            goto done;
        }
        /* An existing non-IRQ _CRS descriptor is an invalid SourceIndex,
         * not permission to allocate a different resource from _PRS. */
        if (current && !resource) {
            goto done;
        }
        struct possible_irq choice = {.wanted = index, .irq = -1};
        uacpi_for_each_resource(possible, find_possible_irq, &choice);
#ifdef PRT_NEGCTL_PRS_FLAT_INDEX
        choice = (struct possible_irq){.wanted = index, .irq = -1};
        choice.resource = interrupt_resource(possible, index);
        if (choice.resource) {
            choice.irq = choose_irq(choice.resource);
        }
#endif
        int irq = choice.irq;
        if (choice.invalid || irq < 0) {
            if (log_failure) {
                kprintf("[prt] link idx %u configuration refused: no usable IRQ alternative\n", index);
            }
            goto done;
        }
        int configured;
#ifdef PRT_NEGCTL_SPLICE_CURRENT
        /* Reproduce the former unsupported cross-alternative combination. */
        if (resource) {
            configured = set_link_irq(node, current, resource, (unsigned)irq);
        } else
#endif
        {
            configured = set_possible_link(node, possible, &choice, current);
        }
        if (configured != 0) {
            if (log_failure) {
                kprintf("[prt] link idx %u configuration refused: incomplete or rejected resource set\n", index);
            }
            goto done;
        }
        /* _SRS may silently decline the requested electrical configuration.
         * Confirm its attributes as well as the IRQ before publishing it. */
        int expected_level = choice.resource->type == UACPI_RESOURCE_TYPE_IRQ ?
            choice.resource->irq.triggering == UACPI_TRIGGERING_LEVEL :
            choice.resource->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        int expected_low = choice.resource->type == UACPI_RESOURCE_TYPE_IRQ ?
            choice.resource->irq.polarity != UACPI_POLARITY_ACTIVE_HIGH :
            choice.resource->extended_irq.polarity != UACPI_POLARITY_ACTIVE_HIGH;
        if (current) {
            uacpi_free_resources(current);
            current = NULL;
        }
        if (uacpi_get_current_resources(node, &current) != UACPI_STATUS_OK ||
            read_interrupt(interrupt_resource(current, index), &link) != 0 ||
            link.irq != (unsigned)irq ||
            link.level != expected_level || link.low != expected_low ||
            uacpi_eval_sta(node, &flags) != UACPI_STATUS_OK || (flags & 3) != 3) {
            goto done;
        }
        result = 0;
    }
    if (result == 0) {
        links[link_count++] = link;
        if (link.irq < 16) {
            irq_users[link.irq]++;
        }
        *out = link;
    }
done:
    if (current) {
        uacpi_free_resources(current);
    }
    if (possible) {
        uacpi_free_resources(possible);
    }
    return result;
}

/* Decode Source=0 as a PCI level/low GSI; links carry their own electrical
 * semantics. Never substitute a plausible IRQ when firmware evaluation fails. */
static int entry_route(uacpi_pci_routing_table_entry *entry, struct pci_irq_route *out)
{
    *out = (struct pci_irq_route){.gsi = entry->index, .level = 1,
                                 .active_low = 1, .source = PCI_ROUTE_AML, .pin = entry->pin};
    if (entry->source) {
        struct prt_link link;
        if (resolve_link(entry->source, entry->index, &link, 1) != 0) {
            return -1;
        }
        out->gsi = link.gsi;
        out->level = link.level;
        out->active_low = link.low;
    }
    return 0;
}

/* Linux v6.12 pci_irq.c acpi_pci_irq_lookup and pci.c
 * pci_swizzle_interrupt_pin apply the CHILD slot once at each bridge.
 * This zero-based form is (pin + child_slot) % 4; never add the root port
 * slot after finding its root _PRT. Ignore function bits per PCI routing. */
int pci_prt_aml_resolve(const struct pci_prt_topology *topology,
                       const struct device *device, struct pci_irq_route *out)
{
    if (!device || !out || device->seg || !device->irq_pin || device->irq_pin > 4) {
        return -1;
    }
    IO_DOMAIN_GUARD(&route_owner);
    discover_roots();
    uint8_t bus = device->bus, slot = device->slot;
    unsigned pin = device->irq_pin - 1;
    for (unsigned hops = 0; hops < BRIDGE_DEPTH; hops++) {
        uacpi_namespace_node *node = bus_node(topology, bus, 0);
        if (node) {
            uacpi_pci_routing_table *table;
            int temporary;
            uacpi_status status = get_table(node, &table, &temporary);
            int matched = 0, result = -1;
            if (status == UACPI_STATUS_OK && table) {
                for (uacpi_size i = 0; i < table->num_entries; i++) {
                    uacpi_pci_routing_table_entry *entry = &table->entries[i];
                    if ((entry->address >> 16) == slot && entry->pin == pin) {
                        matched = 1;
                        result = entry_route(entry, out);
                        if (result == 0) {
                            const char *path = uacpi_namespace_node_generate_absolute_path(node);
                            ksnprintf(out->how, sizeof out->how, "%s %02x:%02x.%c",
                                      path ? path : "?", bus, slot, 'A' + pin);
                            uacpi_free_absolute_path(path);
                            out->hops = (uint8_t)hops;
                        }
                        break;
                    }
                }
            }
            if (temporary && table) {
                uacpi_free_pci_routing_table(table);
            }
            if (matched) {
                return result;
            }
            if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) {
                return -1;
            }
        }
        uint8_t parent, bridge_slot, bridge_function;
        if (!topology || !topology->bridge_of ||
            topology->bridge_of(topology->ctx, bus, &parent, &bridge_slot, &bridge_function) != 0 ||
            parent == bus) {
            return -1;
        }
#ifndef PRT_NEGCTL_NO_SWIZZLE
        pin = (pin + slot) % 4;
#endif
        bus = parent;
        slot = bridge_slot;
    }
    return -1;
}

/* Locate an upstream bridge using its enumerated secondary bus register.
 * Header type 1 and PCI 3.0 bridge config offset 0x19 identify the owner;
 * namespace spelling and enumeration order have no routing meaning. */
static int registry_bridge(void *context, uint8_t bus, uint8_t *parent,
                            uint8_t *slot, uint8_t *function)
{
    (void)context;
    for (int i = 0; ; i++) {
        struct device *device = dev_at(i);
        if (!device) {
            return -1;
        }
        if (device->bus_type == DEV_BUS_PCI && !device->seg &&
            (device->header_type & 0x7f) == 1 &&
            pci_cfg_read8(device->bus, device->slot, device->func, PCI_CFG_SEC_BUS) == bus) {
            *parent = device->bus;
            *slot = device->slot;
            *function = device->func;
            return 0;
        }
    }
}

/* Keep the PCI side independent of uACPI's topology representation. */
static int registered_resolve(const struct device *device, struct pci_irq_route *out)
{
    const struct pci_prt_topology topology = {registry_bridge, NULL};
    return pci_prt_aml_resolve(&topology, device, out);
}

/* Bound serial work and include bridge-local tables, not just root entries.
 * Allocation during dump uses the same link cache as resolution, so _SRS and
 * IRQ usage cannot be counted a second time when the live member is moved. */
void pci_prt_aml_dump(const struct pci_prt_topology *topology)
{
    IO_DOMAIN_GUARD(&route_owner);
    discover_roots();
    uacpi_namespace_node *seen[256];
    unsigned seen_count = 0, lines = 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        uacpi_namespace_node *node = bus_node(topology, (uint8_t)bus, 0);
        if (!node) {
            continue;
        }
        int duplicate = 0;
        for (unsigned i = 0; i < seen_count; i++) {
            if (seen[i] == node) {
                duplicate = 1;
            }
        }
        if (duplicate) {
            continue;
        }
        seen[seen_count++] = node;
        uacpi_pci_routing_table *table;
        int temporary;
        if (get_table(node, &table, &temporary) != UACPI_STATUS_OK || !table) {
            continue;
        }
        const char *path = uacpi_namespace_node_generate_absolute_path(node);
        if (lines < DUMP_LINES - 1) {
            kprintf("[prt] %s entries %u\n", path ? path : "?", (unsigned)table->num_entries);
            lines++;
        }
        int truncated = 0;
        for (uacpi_size i = 0; i < table->num_entries; i++) {
            if (lines == DUMP_LINES - 1) {
                truncated = 1;
                break;
            }
            uacpi_pci_routing_table_entry *entry = &table->entries[i];
            if (!entry->source) {
                kprintf("[prt] %s addr %08x pin %c -> gsi %u\n",
                        path ? path : "?", entry->address, 'A' + entry->pin, entry->index);
            } else {
                struct prt_link link;
                /* Exactly one budgeted entry line reports success OR failure.
                 * Inner allocation diagnostics formerly doubled failed-link
                 * output to 510 lines for a 255-entry table. Resolution outside
                 * the dump keeps its detailed refusal reason. */
                int status = resolve_link(entry->source, entry->index, &link, 0);
                const char *source = uacpi_namespace_node_generate_absolute_path(entry->source);
                if (status == 0) {
                    kprintf("[prt] %s addr %08x pin %c -> link %s idx %u irq %u %s %s\n",
                            path ? path : "?", entry->address, 'A' + entry->pin,
                            source ? source : "?", entry->index, link.irq,
                            link.level ? "level" : "edge", link.low ? "low" : "high");
                } else {
                    kprintf("[prt] %s addr %08x pin %c -> link %s idx %u unavailable\n",
                            path ? path : "?", entry->address, 'A' + entry->pin,
                            source ? source : "?", entry->index);
                }
                uacpi_free_absolute_path(source);
            }
            lines++;
        }
        uacpi_free_absolute_path(path);
        if (temporary) {
            uacpi_free_pci_routing_table(table);
        }
        if (truncated) {
            kprintf("[prt] ... truncated\n");
            return;
        }
    }
}

/* Defer transient lifecycle contention on the existing AML worker. uACPI's
 * platform sleep yields the thread; no routing/AML lock is held here. Five
 * milliseconds is retry policy, not an IOAPIC or device timing requirement. */
static void reroute_sleep(void)
{
    uacpi_kernel_sleep(5);
}

/* The worker calls outside aml_lock: IRQ retirement waits for in-flight ISRs.
 * Release the AML owner before moving members so normal resolver calls can
 * take it; never carry a namespace lock into driver callback catchup. */
void pci_prt_aml_ready(void)
{
    if (__atomic_exchange_n(&ready_once, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
    if (!ioapic_model_set) {
        return;
    }
    const struct pci_prt_topology topology = {registry_bridge, NULL};
    pci_intx_set_aml_resolver(registered_resolve);
    pci_prt_aml_dump(&topology);
    struct pci_reroute_stats stats;
    pci_intx_set_reroute_wait(reroute_sleep);
    pci_intx_reroute_all(&stats);
    kprintf("[prt] reroute devices %u same %u moved %u kept %u failed %u lost %u\n",
            stats.devices, stats.same, stats.moved, stats.kept, stats.failed, stats.lost);
}
