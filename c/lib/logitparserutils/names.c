/* SPDX-License-Identifier: GPL-3.0-or-later
 * Registry lookup is separate from codec availability. Recognising Shift_JIS,
 * for example, does not promise that a Shift_JIS decoder has been implemented.
 */
#include <parserutils/charset/mibenum.h>
#include "registry.h"
#include "text.h"
#include <string.h>

/* These identifiers were observable through the previous public name API but
 * are not IANA assignments. Keep their names queryable without presenting them
 * as standards data or pretending that a decoder exists. The list was checked
 * through retained baseline objects, not imported from the removed alias table.
 * ISO-8859-11 is intentionally resolved by the current IANA alias to 2259. */
static const charset_record local_names[] = {
    {3004, "CP874", ""},
    {3027, "CP949", ""},
    {4000, "ISO-IR-182", ""},
    {4002, "ISO-IR-197", ""},
    {4008, "ISO-2022-JP-1", ""},
    {4009, "MACCYRILLIC", ""},
    {4010, "MACUKRAINE", ""},
    {4011, "MACCENTRALEUROPE", ""},
    {4012, "JOHAB", ""},
    {4014, "ISO-8859-11", ""},
    {4999, "X-CURRENT", ""},
    {5001, "X-ACORN-LATIN1", ""},
    {5002, "X-ACORN-FUZZY", ""}
};

static unsigned folded(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool separator(unsigned char c)
{
    /* Label spelling has historically ignored ASCII punctuation and spaces,
     * not only '-' and '_'. ANSI_X3.4-1968 and ansi_x3_4_1968 must agree.
     * Keep NUL and non-ASCII bytes significant rather than hiding bad input. */
    return c > 0 && c < 128 && !((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static bool same_label(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t ai = 0, bi = 0;
    for (;;) {
        while (ai < alen && separator((unsigned char)a[ai])) ai++;
        while (bi < blen && separator((unsigned char)b[bi])) bi++;
        if (ai == alen || bi == blen) return ai == alen && bi == blen;
        if (folded((unsigned char)a[ai++]) != folded((unsigned char)b[bi++]))
            return false;
    }
}

uint16_t parserutils_charset_mibenum_from_name(const char *label, size_t length)
{
    if (!label || !length) return 0;
    /* Common caller spellings absent from the current registry's alias list.
     * These are names only; they do not redirect ASCII/ISO bytes through web
     * Windows-1252 fallback policy. Conversion support is checked separately. */
    if (same_label(label, length, "ascii", 5)) return TEXT_ASCII;
    if (same_label(label, length, "iso-ir-166", 10)) return 2259;
    if (same_label(label, length, "euc-cn", 6)) return 2025;
    if (same_label(label, length, "macroman", 8)) return 2027;
    for (unsigned digit = 0; digit <= 8; digit++) {
        char alias[] = "cp1250";
        alias[5] += digit;
        if (same_label(label, length, alias, 6)) return 2250 + digit;
    }
    for (size_t i = 0; i < charset_registry_count; i++) {
        const charset_record *record = &charset_registry[i];
        if (same_label(label, length, record->name, strlen(record->name)))
            return record->mib;
        const char *alias = record->aliases;
        while (*alias) {
            const char *end = strchr(alias, '|');
            size_t n = end ? (size_t)(end - alias) : strlen(alias);
            if (same_label(label, length, alias, n)) return record->mib;
            if (!end) break;
            alias = end + 1;
        }
    }
    for (size_t i = 0; i < sizeof(local_names) / sizeof(*local_names); i++) {
        const charset_record *record = &local_names[i];
        if (same_label(label, length, record->name, strlen(record->name)))
            return record->mib;
    }
    return 0;
}

const char *parserutils_charset_mibenum_to_name(uint16_t mib)
{
    size_t first = 0, limit = charset_registry_count;
    while (first < limit) {
        size_t middle = first + (limit - first) / 2;
        const charset_record *record = &charset_registry[middle];
        if (record->mib < mib) first = middle + 1;
        else if (record->mib > mib) limit = middle;
        else return record->name;
    }
    for (size_t i = 0; i < sizeof(local_names) / sizeof(*local_names); i++)
        if (local_names[i].mib == mib) return local_names[i].name;
    return NULL;
}

bool parserutils_charset_mibenum_is_unicode(uint16_t mib)
{
    return mib == TEXT_UTF8 || mib == 1000 || mib == 1001 ||
           (mib >= TEXT_UTF16BE && mib <= TEXT_UTF16) ||
           (mib >= TEXT_UTF32 && mib <= TEXT_UTF32LE);
}
