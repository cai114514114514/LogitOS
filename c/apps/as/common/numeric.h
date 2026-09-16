/* SPDX-License-Identifier: MIT */
#ifndef AS_NUMERIC_H
#define AS_NUMERIC_H
#include <stdint.h>
#include <stddef.h>
/* Unversioned source is not A3. A1 is retired (-1); zero means an unsupported
 * declaration. A3/3.0 selects the native frontend. `#aether: 3` is the same
 * cookie as `# aether: 3.0`; the space after `#` is optional. */
#define AS_LANGUAGE_RETIRED (-1)
#define AS_LANGUAGE_VM 2
#define AS_LANGUAGE_NATIVE 3
#define AS_LANGUAGE_MINOR 0
int as_source_version(const char *source);
const char *as_version_error(int version);
/* Exact signed decimal/hexadecimal parser. Never saturates or uses floating
 * arithmetic; this is shared by the C compiler and self-host parse_int(). */
int as_parse_i64(const char *text, size_t bytes, int64_t *out);
int64_t as_i64_bits(uint64_t bits);
#endif
