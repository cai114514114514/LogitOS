#ifndef _ARGZ_H
#define _ARGZ_H

/* GNU argz vectors: a malloc-owned sequence of NUL-terminated strings packed
 * back to back.  Keeping the byte length beside the pointer is what permits
 * empty elements internally without inventing another container ABI. */
#include <stddef.h>

#ifndef __error_t_defined
#define __error_t_defined 1
typedef int error_t;
#endif

error_t argz_create(char *const argv[], char **argz, size_t *argz_len);
error_t argz_create_sep(const char *string, int sep, char **argz, size_t *argz_len);
size_t  argz_count(const char *argz, size_t argz_len);
void    argz_extract(const char *argz, size_t argz_len, char **argv);
void    argz_stringify(char *argz, size_t argz_len, int sep);
error_t argz_append(char **argz, size_t *argz_len, const char *buf, size_t buf_len);
error_t argz_add(char **argz, size_t *argz_len, const char *str);
error_t argz_add_sep(char **argz, size_t *argz_len, const char *str, int sep);
void    argz_delete(char **argz, size_t *argz_len, char *entry);
error_t argz_insert(char **argz, size_t *argz_len, char *before, const char *entry);
char   *argz_next(const char *argz, size_t argz_len, const char *entry);
error_t argz_replace(char **argz, size_t *argz_len, const char *str,
                     const char *with, unsigned int *replace_count);

#endif /* _ARGZ_H */
