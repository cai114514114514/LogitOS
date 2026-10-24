#ifndef LOGIT_FORM_ENTRIES_H
#define LOGIT_FORM_ENTRIES_H
#include "dom/tree.h"
#include <stddef.h>

struct form_entry {
    char *name, *value;
    size_t name_len, value_len;
    int file;
};
struct form_entries {
    struct form_entry *items;
    size_t count, capacity;
};
/* Native snapshots contain no live DOM pointers and execute no script. Both
 * FormData(form) and navigation must consume this collection: duplicating the
 * successful-control rules made multiple select and external controls drift.
 * Return -1 on allocation failure, -2 when a required capability is absent. */
int fc_entries(struct node *form, struct node *submitter, struct form_entries *out);
int fc_entry_add(struct form_entries *list, const char *name, size_t name_len,
                 const char *value, size_t value_len, int file);
int fc_entries_encode(const struct form_entries *list, char *out, int capacity);
void fc_entries_free(struct form_entries *list);
#endif
