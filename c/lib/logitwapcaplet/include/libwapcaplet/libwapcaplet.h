/* SPDX-License-Identifier: GPL-3.0-or-later
 * LogitOS atom interface. The lwc spelling is the interface consumed by LibCSS;
 * storage and ownership belong to logitwapcaplet, not its former provider. */
#ifndef LOGITWAPCAPLET_H
#define LOGITWAPCAPLET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lwc_string_s lwc_string;
typedef uint32_t lwc_hash;
typedef uint32_t lwc_refcounter;
typedef enum lwc_error_e {
    lwc_error_ok = 0,
    lwc_error_oom = 1,
    lwc_error_range = 2
} lwc_error;
typedef void (*lwc_iteration_callback_fn)(lwc_string *, void *);

lwc_error lwc_intern_string(const char *bytes, size_t length, lwc_string **result);
lwc_error lwc_intern_substring(lwc_string *atom, size_t start, size_t count,
                             lwc_string **result);
lwc_string *lwc_string_ref(lwc_string *atom);
void lwc_string_unref(lwc_string *atom);
const char *lwc_string_data(const lwc_string *);
size_t lwc_string_length(const lwc_string *);
lwc_hash lwc_string_hash_value(const lwc_string *);
lwc_error lwc_string_isequal(const lwc_string *a, const lwc_string *b, bool *equal);
lwc_error lwc_string_caseless_isequal(lwc_string *a, lwc_string *b,
                                   bool *equal);
lwc_error lwc_string_caseless_hash_value(lwc_string *atom, lwc_hash *hash);
lwc_error lwc_string_tolower(lwc_string *atom, lwc_string **result);
void lwc_iterate_strings(lwc_iteration_callback_fn callback, void *context);

#ifdef __cplusplus
}
#endif
#endif
