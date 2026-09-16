/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "class.h"

/* All class layouts start with this header, then the inherited field prefix.
 * The shared field list keeps the LLVM prefix aligned with the C ABI. Keeping
 * initialization separate from the GC descriptor lets a base initializer
 * detect an escape before the most-derived object's fields are ready. */
void at_class_prepare(void *object, const void *methods, int64_t required)
{
    AtClassHeader *header = object;
    header->methods = methods;
    header->required = required;
    header->initialized = 0;
}

void at_class_field_initialized(void *object, int32_t field)
{
    AtClassHeader *header = object;
    if (field >= 0 && field < 32) {
        header->initialized |= INT64_C(1) << field;
    }
}

int32_t at_class_ready(const void *object)
{
    const AtClassHeader *header = object;
    return header->required == header->initialized;
}

void *at_object_new(int64_t bytes, AtScan scan)
{
    if (bytes < 0) {
        return NULL;
    }
    /* Even an empty class has distinct reference identity. A zero-byte heap
     * range cannot be marked by an interior-pointer collector, so reserve one
     * byte while retaining the language's zero-field payload layout. */
    return at_gc_allocate(bytes ? (size_t)bytes : 1, scan);
}
