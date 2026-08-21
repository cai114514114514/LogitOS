/* THE KERNEL'S MODULE ABI -- the explicit list of symbols a loaded module may
 * resolve against, and nothing else.
 *
 * ===========================================================================
 * WHY EXPLICIT, AND WHY A TABLE RATHER THAN A MACRO
 *
 * Two designs were available and only one of them is a decision the kernel can
 * live with:
 *
 *   (a) EXPORT EVERYTHING -- hand the loader the kernel's own .symtab and let
 *       a module reach any global. It is less code here and it is much worse
 *       everywhere else: every static function that happens to be non-static,
 *       every helper somebody meant to rename, becomes an ABI the moment one
 *       module calls it. The kernel then cannot change its own internals
 *       without breaking a binary it has never seen. The cost is not paid on
 *       the day it is chosen, which is exactly why it gets chosen.
 *
 *   (b) EXPORT A LIST -- what Linux does with EXPORT_SYMBOL. The list IS the
 *       statement of what the kernel's internal API is, and the discipline is
 *       the feature: adding a name to this file is a deliberate act with a
 *       reader, and removing one is a visible break rather than a silent one.
 *
 * This is (b). What is NOT here is Linux's distribution of the decision to the
 * file that owns the function -- an EXPORT_SYMBOL(kprintf) macro next to
 * kprintf, emitting into a `logit_ksyms` linker section exactly the way
 * DRIVER_DECLARE emits into `logit_drivers`. That is the better shape and it
 * is what this should become: it puts the export beside the thing exported, so
 * a rename cannot leave a stale entry behind, and it needs no central file to
 * edit. It is not that today for one reason worth writing down rather than
 * hiding: emitting from the macro requires editing c/drivers/core/device.c,
 * c/kernel/core/kprintf.c and files under c/kernel/mm, which belong to other
 * lines of work right now. The migration is mechanical -- add the macro, add
 * a bracketed section to linker.ld beside the `logit_drivers` one (bracketed
 * EXPLICITLY, per that file's own note about orphan placement landing past
 * _kernel_end), and delete this table.
 *
 * Until then this table has one property the macro version would not, and it
 * is not nothing: it is a single screen that answers "what is the kernel's
 * module ABI?" without grepping. The macro version's answer is `nm`.
 *
 * ===========================================================================
 * WHY THESE NAMES AND NOT MORE
 *
 * Not chosen, MEASURED. Three real drivers were compiled with the kernel's own
 * flags and their undefined symbols listed:
 *
 *   c/drivers/core/qemu_edu.c        dev_enable dev_bar_map dev_irq_request
 *                                    dev_irq_release dev_irq_prefer
 *                                    dev_irq_count kprintf          (7)
 *   c/drivers/virtio/virtio_rng.c    virtio_init virtio_queue_setup
 *                                    virtio_driver_ok virtio_request
 *                                    kprintf                        (5)
 *   c/drivers/virtio/virtio_balloon.c  the four virtio_* above, plus
 *                                    pmm_alloc pmm_free pmm_free_frames
 *                                    kprintf                        (8)
 *
 * That union is 14 names. The other eight below are the ones a driver reaches
 * for on its second day and whose absence would be a link error with no
 * obvious fix: the allocator, the four memory primitives the COMPILER emits
 * calls to on its own (a struct assignment becomes a memcpy the source never
 * wrote), a monotonic clock, and the two device-lookup calls a driver that
 * wants a second device of its own class needs.
 *
 * DELIBERATELY ABSENT, each for a reason:
 *   dev_probe_all   the LOADER calls it, once, after registering the module's
 *                   drivers. A module calling it would re-enter the probe pass
 *                   from inside a probe.
 *   driver_register the loader calls it too, out of the module's own
 *                   logit_drivers section. A module that also called it by
 *                   hand would register the same driver twice -- which
 *                   device.c happens to refuse, but relying on that turns an
 *                   accident into the contract.
 *   pmm_alloc_contig / kmalloc's arena internals, vmm_map_page, the BKL
 *                   Each is a real ask, none of them has a caller yet, and an
 *                   export whose first user does not exist is an ABI frozen
 *                   around a guess about what that user will want.
 * =========================================================================== */

#include <stdint.h>
#include <stddef.h>
#include "module.h"
#include "driver.h"      /* dev_*        -- c/drivers/core/driver.h */
#include "virtio.h"      /* virtio_*     -- c/drivers/virtio/virtio.h */
#include "pmm.h"         /* pmm_*        -- c/kernel/mm/pmm.h */
#include "kheap.h"       /* kmalloc/kfree */
#include "kprintf.h"     /* kprintf */
#include "ktime.h"       /* time_mono_ms */

/* c/lib/string.c has no header in this tree (measured: no `void *memcpy` in
 * any .h under c/ outside mini-libc). Declared here rather than including
 * mini-libc's <string.h>, which is a USERLAND header -- see CLAUDE.md's note
 * on the flat INCDIRS and the two times a libc basename shadowed a kernel one.
 * These four matter more than they look: clang emits calls to memcpy and
 * memset for struct assignment and array zeroing that the driver source never
 * wrote, so a module can need them without a single textual reference. */
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

struct ksym { const char *name; void *addr; };

/* KSYM(x) rather than { "x", (void *)x }: written out twice, a rename touches
 * one half and the table then exports the OLD name pointing at the NEW
 * function, which resolves and misbehaves. The macro makes that unspellable.
 * The cast through uintptr_t is what lets a function pointer sit in a void* --
 * strictly implementation-defined in C11, universally fine on x86-64, and the
 * alternative (a union per entry) buys nothing here. */
#define KSYM(x) { #x, (void *)(uintptr_t)(x) }

static const struct ksym g_ksyms[] = {
    /* -- console. The first thing every driver needs and the reason a module
     *    that resolves nothing else is still worth loading. */
    KSYM(kprintf),

    /* -- device model: what qemu_edu.c needs, exactly. */
    KSYM(dev_enable),
    KSYM(dev_disable),
    KSYM(dev_bar_map),
    KSYM(dev_irq_request),
    KSYM(dev_irq_release),
    KSYM(dev_irq_prefer),
    KSYM(dev_irq_count),
    KSYM(dev_find_class),
    KSYM(dev_find_id),

    /* -- virtio transport: what virtio_rng.c and virtio_balloon.c need. A
     *    virtio driver never touches PCI config space itself; this is the
     *    whole of its interface to the bus. */
    KSYM(virtio_init),
    KSYM(virtio_queue_setup),
    KSYM(virtio_driver_ok),
    KSYM(virtio_request),

    /* -- memory. pmm_* are frames (what a DMA ring is made of), kmalloc is
     *    bytes (what driver state is made of). Both, because a driver that
     *    has only one of them writes the other badly. */
    KSYM(pmm_alloc),
    KSYM(pmm_free),
    KSYM(pmm_free_frames),
    KSYM(kmalloc),
    KSYM(kfree),

    /* -- the four the compiler emits on its own. */
    KSYM(memcpy),
    KSYM(memset),
    KSYM(memmove),
    KSYM(memcmp),

    /* -- a monotonic millisecond. Every driver with a timeout needs a time
     *    base, and the alternative a driver reaches for without one is a
     *    spin-count loop calibrated on the author's machine. */
    KSYM(time_mono_ms),
};

#define NKSYM ((int)(sizeof g_ksyms / sizeof g_ksyms[0]))

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Linear. 22 entries and one lookup per undefined symbol per load -- the edu
 * module has 7 -- so a hash table would be more code than it saves and would
 * put a second copy of the name in the binary. Revisit at a few hundred. */
void *ksym_lookup(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < NKSYM; i++)
        if (streq(g_ksyms[i].name, name)) return g_ksyms[i].addr;
    return NULL;
}

int ksym_count(void) { return NKSYM; }

const char *ksym_name_at(int i)
{
    return (i >= 0 && i < NKSYM) ? g_ksyms[i].name : NULL;
}
