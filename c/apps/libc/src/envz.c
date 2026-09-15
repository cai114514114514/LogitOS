/* GNU envz, layered on argz so there is one implementation of vector growth
 * and deletion.  Name matching stops at '='; later '=' bytes belong to the
 * value and are intentionally opaque here. */
#include <envz.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static size_t name_len(const char *s)
{
    const char *eq = strchr(s, '=');
    return eq ? (size_t)(eq - s) : strlen(s);
}

char *envz_entry(const char *envz, size_t envz_len, const char *name)
{
    if (!name) return NULL;
    size_t nl = name_len(name);
    const char *entry = NULL;
    while ((entry = argz_next(envz, envz_len, entry))) {
        size_t el = name_len(entry);
        if (el == nl && memcmp(entry, name, nl) == 0) return (char *)entry;
    }
    return NULL;
}

char *envz_get(const char *envz, size_t envz_len, const char *name)
{
    char *entry = envz_entry(envz, envz_len, name);
    if (!entry) return NULL;
    char *eq = strchr(entry, '=');
    return eq ? eq + 1 : NULL;
}

void envz_remove(char **envz, size_t *envz_len, const char *name)
{
    if (!envz || !envz_len) return;
    char *entry;
    while ((entry = envz_entry(*envz, *envz_len, name)))
        argz_delete(envz, envz_len, entry);
}

error_t envz_add(char **envz, size_t *envz_len, const char *name, const char *value)
{
    if (!envz || !envz_len || !name || strchr(name, '=')) return EINVAL;
    size_t nl = strlen(name), vl = value ? strlen(value) : 0;
    if (value && (nl > (size_t)-1 - vl - 2)) return ENOMEM;
    char *entry = malloc(nl + (value ? vl + 2 : 1));
    if (!entry) return ENOMEM;
    memcpy(entry, name, nl);
    if (value) { entry[nl] = '='; memcpy(entry + nl + 1, value, vl + 1); }
    else entry[nl] = 0;

    /* Work on a private vector so allocation failure is transactional: the
     * caller keeps the old NAME=VALUE if either copy or append cannot grow. */
    size_t next_len = *envz_len;
    char *next = next_len ? malloc(next_len) : NULL;
    if (next_len && !next) { free(entry); return ENOMEM; }
    if (next_len) memcpy(next, *envz, next_len);
    envz_remove(&next, &next_len, name);
    error_t e = argz_add(&next, &next_len, entry);
    free(entry);
    if (e) { free(next); return e; }
    free(*envz);
    *envz = next;
    *envz_len = next_len;
    return e;
}

error_t envz_merge(char **envz, size_t *envz_len, const char *envz2,
                   size_t envz2_len, int override)
{
    if (!envz || !envz_len || (envz2_len && !envz2) ||
        (*envz_len && !*envz)) return EINVAL;

    /* Snapshot BOTH sides. Besides making ENOMEM transactional, this makes
     * envz_merge(&z, &n, z, n, ...) well-defined: envz_add replaces its target
     * vector, so walking the caller's aliased source directly would otherwise
     * continue through freed storage after the first entry. */
    char *source = envz2_len ? malloc(envz2_len) : NULL;
    char *next = *envz_len ? malloc(*envz_len) : NULL;
    if ((envz2_len && !source) || (*envz_len && !next)) {
        free(source);
        free(next);
        return ENOMEM;
    }
    if (envz2_len) memcpy(source, envz2, envz2_len);
    size_t next_len = *envz_len;
    if (next_len) memcpy(next, *envz, next_len);

    const char *entry = NULL;
    while ((entry = argz_next(source, envz2_len, entry))) {
        size_t nl = name_len(entry);
        char *name = malloc(nl + 1);
        if (!name) { free(source); free(next); return ENOMEM; }
        memcpy(name, entry, nl); name[nl] = 0;
        if (!override && envz_entry(next, next_len, name)) {
            free(name);
            continue;
        }
        const char *eq = strchr(entry, '=');
        error_t e = envz_add(&next, &next_len, name, eq ? eq + 1 : NULL);
        free(name);
        if (e) { free(source); free(next); return e; }
    }
    free(source);
    free(*envz);
    *envz = next;
    *envz_len = next_len;
    return 0;
}

void envz_strip(char **envz, size_t *envz_len)
{
    if (!envz || !envz_len) return;
    size_t off = 0;
    while (off < *envz_len) {
        char *entry = *envz + off;
        size_t bytes = strlen(entry) + 1;
        if (!strchr(entry, '=')) {
            argz_delete(envz, envz_len, entry);
        } else off += bytes;
    }
}
