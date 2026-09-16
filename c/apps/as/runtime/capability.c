/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "capability.h"
#include <stdlib.h>
#include <string.h>

#if !__STDC_HOSTED__
#include "logit_abi.h"
#endif

#define CAP_PATH_LIMIT AT_CAP_PATH_LIMIT

struct AtCap {
    uint32_t bits;
    int64_t length; /* -1 is the unrestricted path; zero is never a valid scope. */
    char path[];
};

static uint32_t held_bits;
static char *held_prefix;
static int normalize(char *out, const char *path, int64_t length);

void at_caps_set(uint32_t bits, const char *prefix)
{
    char *copy = NULL;
    if (prefix) {
        char canonical[CAP_PATH_LIMIT + 1];
        int length = normalize(canonical, prefix, (int64_t)strlen(prefix));
        if (!length || !(copy = malloc((size_t)length + 1))) {
            /* Keeping bits but losing the path would silently widen a grant. */
            free(held_prefix);
            held_prefix = NULL;
            held_bits = 0;
            return;
        }
        memcpy(copy, canonical, (size_t)length + 1);
    }
    free(held_prefix);
    held_prefix = copy;
    held_bits = bits;
}

void at_caps_init(void)
{
    at_caps_set(0, NULL);
#if __STDC_HOSTED__
    /* A directly launched host program has its process's ordinary authority.
     * Hosted x86_64 must never execute LogitOS int 0x80 on the host kernel. */
    at_caps_set(AS_CAP_FS_READ | AS_CAP_FS_WRITE | AS_CAP_NET | AS_CAP_PROC | AS_CAP_GUI |
                    AS_CAP_RAW,
                NULL);
#else
    char prefix[128] = {0};
    long grant;
    __asm__ volatile("int $0x80"
                     : "=a"(grant)
                     : "a"((long)SYS_CAP_QUERY), "D"(prefix), "S"((long)sizeof prefix), "d"(0L)
                     : "memory");
    if (grant < 0 || !memchr(prefix, 0, sizeof prefix)) {
        return;
    }

    /* Kernel and language assignments differ: copying the mask would turn
     * kernel networking into language file-write permission. */
    uint32_t bits = 0;
    if (grant & CAP_FS) {
        bits |= AS_CAP_FS_READ | AS_CAP_FS_WRITE;
    }
    if (grant & CAP_NET) {
        bits |= AS_CAP_NET;
    }
    if (grant & CAP_RAW) {
        bits |= AS_CAP_RAW;
    }
    if (grant & CAP_PROC) {
        bits |= AS_CAP_PROC;
    }
    if (grant & CAP_GUI) {
        bits |= AS_CAP_GUI;
    }
    at_caps_set(bits, prefix[0] ? prefix : NULL);
#endif
}

int at_caps_have(uint32_t bits)
{
    return (held_bits & bits) == bits;
}

const char *at_caps_prefix(void)
{
    return held_prefix;
}

int at_caps_resolve_path(char *out, const char *path, int64_t length)
{
    int count = normalize(out, path, length);
    if (!count) {
        return AT_E_VALUE;
    }
    if (held_prefix) {
        size_t prefix = strlen(held_prefix);
        if (prefix > 1 && ((size_t)count < prefix || memcmp(out, held_prefix, prefix) ||
                           ((size_t)count > prefix && out[prefix] != '/'))) {
            return AT_E_PERMISSION;
        }
    }
    return 0;
}

static AtCap *make_cap(uint32_t bits, const char *path, int64_t length)
{
    size_t bytes = path ? (size_t)length + 1 : 0;
    AtCap *capability = at_gc_allocate(sizeof *capability + bytes, NULL);
    if (capability) {
        capability->bits = bits;
        capability->length = path ? length : -1;
        if (path) {
            memcpy(capability->path, path, bytes);
        }
    }
    return capability;
}

AtCap *at_caps_value(void)
{
    return make_cap(held_bits, held_prefix, held_prefix ? (int64_t)strlen(held_prefix) : -1);
}

int64_t at_cap_bits(const AtCap *capability)
{
    return capability->bits;
}

const char *at_cap_path(const AtCap *capability)
{
    return capability->length < 0 ? NULL : capability->path;
}

int64_t at_cap_path_length(const AtCap *capability)
{
    return capability->length < 0 ? 0 : capability->length;
}

AtCap *at_cap_without(const AtCap *capability, int64_t mask)
{
    return make_cap(capability->bits & ~(uint32_t)mask, at_cap_path(capability),
                    capability->length);
}

static int normalize(char *out, const char *path, int64_t length)
{
    if (length <= 0 || length > CAP_PATH_LIMIT || path[0] != '/' ||
        memchr(path, 0, (size_t)length)) {
        return 0;
    }
    int write = 1;
    out[0] = '/';
    for (int read = 1; read < length;) {
        int start = read;
        while (read < length && path[read] != '/') {
            read++;
        }
        int count = read - start;
        read++;
        if (!count || (count == 1 && path[start] == '.')) {
            continue;
        }
        if (count == 2 && path[start] == '.' && path[start + 1] == '.') {
            while (write > 1 && out[write - 1] != '/') {
                write--;
            }
            if (write > 1) {
                write--;
            }
            continue;
        }
        if (write > 1) {
            out[write++] = '/';
        }
        memcpy(out + write, path + start, (size_t)count);
        write += count;
    }
    out[write] = 0;
    return write;
}

int at_cap_scope(AtCap **out, const AtCap *parent, const char *path, int64_t length)
{
    *out = NULL;
    char canonical[CAP_PATH_LIMIT + 1];
    int count = normalize(canonical, path, length);
    if (!count) {
        return AT_E_VALUE;
    }

    /* Compare canonical path components, including their boundary. Scoping
     * is only metadata attenuation; actual filesystem resolution and its
     * kernel capability ceiling still apply when a resource is acquired. */
    if (parent->length > 1 &&
        (count < parent->length || memcmp(parent->path, canonical, (size_t)parent->length) ||
         (count > parent->length && canonical[parent->length] != '/'))) {
        return AT_E_PERMISSION;
    }
    *out = make_cap(parent->bits, canonical, count);
    return *out ? 0 : AT_E_MEMORY;
}
