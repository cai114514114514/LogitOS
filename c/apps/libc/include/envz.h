#ifndef _ENVZ_H
#define _ENVZ_H

/* GNU envz is argz with NAME[=VALUE] elements.  A bare NAME is deliberately
 * distinct from NAME=; envz_get() returns NULL for the former and "" for the
 * latter. */
#include <argz.h>

char   *envz_entry(const char *envz, size_t envz_len, const char *name);
char   *envz_get(const char *envz, size_t envz_len, const char *name);
error_t envz_add(char **envz, size_t *envz_len, const char *name, const char *value);
error_t envz_merge(char **envz, size_t *envz_len, const char *envz2,
                   size_t envz2_len, int override);
void    envz_remove(char **envz, size_t *envz_len, const char *name);
void    envz_strip(char **envz, size_t *envz_len);

#endif /* _ENVZ_H */
