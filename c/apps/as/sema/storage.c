/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_check_byte_storage(AsTypedProject *project, int type, int writable)
{
    AtType *storage = &project->types[type];
    if (storage->kind == AT_PARAMETER) {
        /* Check generic bodies against their promise, not the first caller's
         * concrete type. Read-only ByteStorage must reject writes even when
         * all currently observed callers happen to pass mutable buffers. */
        return storage->constraint == AT_CONSTRAINT_MUTABLE_BYTE_STORAGE ||
               (!writable && storage->constraint == AT_CONSTRAINT_BYTE_STORAGE);
    }
    return storage->kind == AT_BUFFER || storage->kind == AT_LAYOUT ||
           (!writable && storage->kind == AT_BYTES);
}
