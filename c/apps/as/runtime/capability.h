/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_CAPABILITY_H
#define AS_NATIVE_CAPABILITY_H
#include "capability_bits.h"
#include <stdint.h>

typedef struct AtCap AtCap;

#define AT_CAP_PATH_LIMIT 4096

/* These installation hooks belong to the trusted process launcher. No source
 * language builtin may construct a capability from arbitrary bits or a path.
 * Snapshot/attenuation objects never replace the process's held authority. */
void at_caps_init(void);
void at_caps_set(uint32_t bits, const char *prefix);
int at_caps_have(uint32_t bits);
const char *at_caps_prefix(void);
/* Resolve an absolute lexical path into a caller-owned LIMIT + 1 buffer.
 * This checks the held scope; file acquisition must additionally constrain
 * filesystem traversal so symlinks cannot bypass that lexical check. */
int at_caps_resolve_path(char *out, const char *path, int64_t length);
AtCap *at_caps_value(void);
int64_t at_cap_bits(const AtCap *capability);
const char *at_cap_path(const AtCap *capability);
int64_t at_cap_path_length(const AtCap *capability);
AtCap *at_cap_without(const AtCap *capability, int64_t mask);

/* Zero means success; failures use AtExceptionCode and leave *out null. The
 * caller attaches the original source location before propagating the error. */
int at_cap_scope(AtCap **out, const AtCap *parent, const char *path, int64_t length);
#endif
