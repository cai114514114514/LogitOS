/* SPDX-License-Identifier: MIT */
#include "port.h"
#include <string.h>

static void scan_text(void *slot)
{
    at_gc_mark((void *)((AtNativeText *)slot)->data);
}

static void scan_dictionary(void *slot)
{
    at_gc_mark(*(AtDict **)slot);
}

AtDict *at_port_stats(AtHash hash, AtEqual equal)
{
    AtPortCounters counters;
    at_port_counters(&counters);

    const struct {
        const char *name;
        int64_t value;
    } entries[] = {
        {"open", counters.open},
        {"closed", counters.closed},
        {"close_errors", counters.close_errors},
        /* These retain their old meaning: cleanup performed by the GC.
         * A3 Port/Process owners cannot escape with scopes and have no GC
         * finalizers, so both counts are zero by construction. In particular,
         * do not disguise deterministic closes as GC finalization events. */
        {"finalized", 0},
        {"orphans", 0},
    };

    void *frame = at_gc_frame();
    AtDict *dictionary =
        at_dict_new(sizeof(AtNativeText), sizeof(int64_t), scan_text, NULL, hash, equal);
    if (!dictionary) {
        return NULL;
    }
    AtRoot root;
    at_gc_root(&root, &dictionary, scan_dictionary);
    for (unsigned i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        AtNativeText key = {entries[i].name, (int64_t)strlen(entries[i].name)};
        if (!at_dict_set(dictionary, &key, &entries[i].value)) {
            dictionary = NULL;
            break;
        }
    }
    at_gc_restore(frame);
    return dictionary;
}
