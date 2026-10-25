/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_CHARSET_REGISTRY_H
#define LOGIT_CHARSET_REGISTRY_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t mib;
    const char *name;
    const char *aliases;
} charset_record;

extern const charset_record charset_registry[];
extern const size_t charset_registry_count;
#endif
