/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <string.h>

/* Only explicit Any values pay for boxing. The payload retains its concrete
 * native layout; its generated scanner describes references inside aggregates.
 * Type IDs are private to the linked program and never serialized as an ABI. */
struct AtAny {
    int type;
    AtScan scan;
    const AtNativeType *description;
    unsigned char data[];
};

static void scan_any(void *pointer)
{
    AtAny *box = pointer;
    if (box->scan) {
        box->scan(box->data);
    }
}

AtAny *at_any_new(int type, int64_t bytes, AtScan scan, const void *value,
                  const AtNativeType *description)
{
    if (bytes < 0 || (uint64_t)bytes > SIZE_MAX - sizeof(AtAny)) {
        return NULL;
    }
    /* The caller's expression root holds value across this safepoint. The
     * returned box is rooted by generated code before any later allocation. */
    AtAny *box = at_gc_allocate(sizeof(*box) + (size_t)bytes, scan_any);
    if (!box) {
        return NULL;
    }
    box->type = type;
    box->scan = scan;
    box->description = description;
    memcpy(box->data, value, (size_t)bytes);
    return box;
}

void *at_any_data(AtAny *box, int type)
{
    return box && box->type == type ? box->data : NULL;
}

const AtNativeType *at_any_type(AtAny *box)
{
    return box->description;
}

const void *at_any_value(AtAny *box)
{
    return box->data;
}
