/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "atom.h"
#include <stdlib.h>

const char *lwc_string_data(const lwc_string *atom) { return atom ? atom->bytes : NULL; }
size_t lwc_string_length(const lwc_string *atom) { return atom ? atom->length : 0; }
lwc_hash lwc_string_hash_value(const lwc_string *atom) { return atom ? atom->exact_hash : 0; }

lwc_error lwc_intern_substring(lwc_string *atom, size_t start, size_t count,
                             lwc_string **result)
{
    if (!atom || !result || start > atom->length || count > atom->length - start)
        return lwc_error_range;
    return lwc_intern_string(atom->bytes + start, count, result);
}

lwc_error lwc_string_isequal(const lwc_string *a, const lwc_string *b, bool *equal)
{
    if (!a || !b || !equal) return lwc_error_range;
    *equal = a == b;
    return lwc_error_ok;
}

lwc_error lwc_string_caseless_hash_value(lwc_string *atom, lwc_hash *hash)
{
    if (!atom || !hash) return lwc_error_range;
    *hash = atom->folded_hash;
    return lwc_error_ok;
}

lwc_error lwc_string_caseless_isequal(lwc_string *a, lwc_string *b,
                                   bool *equal)
{
    if (!a || !b || !equal) return lwc_error_range;
    *equal = false;
    if (a->length != b->length || a->folded_hash != b->folded_hash) return lwc_error_ok;
    /* The CSS caller requires ASCII folding, not locale-sensitive tolower().
     * UTF-8 bytes above ASCII must remain exact, including embedded NULs. */
    for (size_t i = 0; i < a->length; i++)
        if (atom_fold((unsigned char)a->bytes[i]) != atom_fold((unsigned char)b->bytes[i]))
            return lwc_error_ok;
    *equal = true;
    return lwc_error_ok;
}

lwc_error lwc_string_tolower(lwc_string *atom, lwc_string **result)
{
    if (!atom || !result) return lwc_error_range;
    bool changed = false;
    for (size_t i = 0; i < atom->length; i++)
        if (atom_fold((unsigned char)atom->bytes[i]) != (unsigned char)atom->bytes[i])
            changed = true;
    if (!changed) {
        *result = lwc_string_ref(atom);
        return *result ? lwc_error_ok : lwc_error_oom;
    }
    char *bytes = malloc(atom->length);
    if (!bytes) return lwc_error_oom;
    for (size_t i = 0; i < atom->length; i++) bytes[i] = (char)atom_fold((unsigned char)atom->bytes[i]);
    lwc_error error = lwc_intern_string(bytes, atom->length, result);
    free(bytes);
    return error;
}
