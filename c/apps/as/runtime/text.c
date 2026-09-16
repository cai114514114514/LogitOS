/* SPDX-License-Identifier: MIT */
/* Immutable byte-oriented UTF-8 text operations, with explicit native roots. */
#include "native.h"
#include <string.h>

int at_text_concat(AtNativeText *out, const char *left, int64_t left_length, const char *right,
                   int64_t right_length)
{
    if (left_length > INT64_MAX - right_length - 1) {
        return 0;
    }
    int64_t length = left_length + right_length;
    char *data = at_gc_allocate((size_t)length + 1, NULL);
    if (!data) {
        return 0;
    }
    if (left_length) {
        memcpy(data, left, (size_t)left_length);
    }
    if (right_length) {
        memcpy(data + left_length, right, (size_t)right_length);
    }
    *out = (AtNativeText){data, length};
    return 1;
}

int at_text_repeat(AtNativeText *out, const char *text, int64_t length, int64_t count)
{
    *out = (AtNativeText){"", 0};
    if (count <= 0 || !length) {
        return 1;
    }
    if (count > (INT64_MAX - 1) / length) {
        return 0;
    }
    int64_t total = length * count;
    char *data = at_gc_allocate((size_t)total + 1, NULL);
    if (!data) {
        return 0;
    }
    for (int64_t i = 0; i < count; i++) {
        memcpy(data + i * length, text, (size_t)length);
    }
    *out = (AtNativeText){data, total};
    return 1;
}

void at_text_slice(AtNativeText *out, const char *text, int64_t length, int64_t start, int64_t stop)
{
    if (start < 0) {
        start += length;
    }
    if (stop < 0) {
        stop += length;
    }
    start = start < 0 ? 0 : start > length ? length : start;
    stop = stop < start ? start : stop > length ? length : stop;
    /* Empty slices use static storage, so a one-past pointer is never a root. */
    *out = stop == start ? (AtNativeText){"", 0} : (AtNativeText){text + start, stop - start};
}

int at_text_join(AtNativeText *out, const char *separator, int64_t separator_length, AtList *parts)
{
    int64_t length = 0;
    for (int64_t i = 0; i < parts->count; i++) {
        AtNativeText *part = (AtNativeText *)(parts->data + i * parts->stride);
        int64_t gap = i ? separator_length : 0;
        if (gap > INT64_MAX - 1 - length || part->length > INT64_MAX - 1 - length - gap) {
            return 0;
        }
        length += gap + part->length;
    }
    char *data = at_gc_allocate((size_t)length + 1, NULL);
    if (!data) {
        return 0;
    }
    int64_t offset = 0;
    for (int64_t i = 0; i < parts->count; i++) {
        AtNativeText *part = (AtNativeText *)(parts->data + i * parts->stride);
        if (i && separator_length) {
            memcpy(data + offset, separator, (size_t)separator_length);
            offset += separator_length;
        }
        if (part->length) {
            memcpy(data + offset, part->data, (size_t)part->length);
            offset += part->length;
        }
    }
    *out = (AtNativeText){data, length};
    return 1;
}

static int ascii_space(unsigned char character)
{
    return character == ' ' || (character >= '\t' && character <= '\r');
}

void at_text_strip(AtNativeText *out, const char *text, int64_t length)
{
    int64_t start = 0;
    while (start < length && ascii_space((unsigned char)text[start])) {
        start++;
    }
    while (length > start && ascii_space((unsigned char)text[length - 1])) {
        length--;
    }
    *out = (AtNativeText){text + start, length - start};
    if (start == length) {
        *out = (AtNativeText){"", 0};
    }
}

int at_text_case(AtNativeText *out, const char *text, int64_t length, int upper)
{
    char *data = at_gc_allocate((size_t)length + 1, NULL);
    if (!data) {
        return 0;
    }
    for (int64_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char)text[i];
        if (upper && character >= 'a' && character <= 'z') {
            character -= 'a' - 'A';
        } else if (!upper && character >= 'A' && character <= 'Z') {
            character += 'a' - 'A';
        }
        data[i] = (char)character;
    }
    *out = (AtNativeText){data, length};
    return 1;
}

static void scan_text_slot(void *slot)
{
    at_gc_mark((void *)((AtNativeText *)slot)->data);
}

static void scan_list_slot(void *slot)
{
    at_gc_mark(*(AtList **)slot);
}

int at_text_split(AtList **out, const char *text, int64_t length, const char *separator,
                  int64_t separator_length)
{
    if (separator && separator_length <= 0) {
        return 0; /* Generated callers raise ValueError before reaching here. */
    }
    AtList *parts = at_list_new(sizeof(AtNativeText), scan_text_slot);
    if (!parts) {
        return 0;
    }
    /* C helpers obey the same root protocol as generated functions. append()
     * can collect while growing the backing buffer; parts must survive it. */
    void *frame = at_gc_frame();
    AtRoot root;
    at_gc_root(&root, &parts, scan_list_slot);
    int ok = 1;
    int64_t start = 0;
    if (separator) {
        for (int64_t i = 0; ok && i <= length - separator_length;) {
            if (!memcmp(text + i, separator, (size_t)separator_length)) {
                AtNativeText part = {text + start, i - start};
                if (!part.length) {
                    part.data = "";
                }
                ok = at_list_append(parts, &part);
                i += separator_length;
                start = i;
            } else {
                i++;
            }
        }
        if (ok) {
            AtNativeText part = {text + start, length - start};
            if (!part.length) {
                part.data = "";
            }
            ok = at_list_append(parts, &part);
        }
    } else {
        while (ok && start < length) {
            while (start < length && ascii_space((unsigned char)text[start])) {
                start++;
            }
            int64_t end = start;
            while (end < length && !ascii_space((unsigned char)text[end])) {
                end++;
            }
            if (end > start) {
                AtNativeText part = {text + start, end - start};
                ok = at_list_append(parts, &part);
            }
            start = end;
        }
    }
    *out = parts;
    at_gc_restore(frame);
    return ok;
}

int at_text_replace(AtNativeText *out, const char *text, int64_t length, const char *old,
                    int64_t old_length, const char *replacement, int64_t replacement_length)
{
    AtList *parts;
    if (!at_text_split(&parts, text, length, old, old_length)) {
        return 0;
    }
    void *frame = at_gc_frame();
    AtRoot root;
    at_gc_root(&root, &parts, scan_list_slot);
    int ok = at_text_join(out, replacement, replacement_length, parts);
    at_gc_restore(frame);
    return ok;
}

int at_text_compare(const char *left, int64_t left_length, const char *right, int64_t right_length)
{
    int64_t common = left_length < right_length ? left_length : right_length;
    int compared = common ? memcmp(left, right, (size_t)common) : 0;
    if (compared) {
        return compared < 0 ? -1 : 1;
    }
    return (left_length > right_length) - (left_length < right_length);
}

int64_t at_text_find(const char *text, int64_t length, const char *needle, int64_t needle_length)
{
    /* A2's string search uses byte offsets, including embedded NULs. Empty
     * needles match at zero. Subtraction bounds the last start without an
     * overflowing index + needle_length intermediate. This allocates nothing. */
    if (!needle_length) {
        return 0;
    }
    if (needle_length > length) {
        return -1;
    }
    for (int64_t i = 0; i <= length - needle_length; i++) {
        if (!memcmp(text + i, needle, (size_t)needle_length)) {
            return i;
        }
    }
    return -1;
}
