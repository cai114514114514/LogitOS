/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_ATOM_H
#define LOGIT_ATOM_H
#include <libwapcaplet/libwapcaplet.h>

struct lwc_string_s {
    size_t length;
    uint32_t references;
    uint32_t exact_hash;
    uint32_t folded_hash;
    char bytes[];
};

static inline unsigned char atom_fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

uint32_t atom_hash(const char *, size_t, bool);
#endif
