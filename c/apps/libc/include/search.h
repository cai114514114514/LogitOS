#ifndef _SEARCH_H
#define _SEARCH_H

/* System V / POSIX <search.h>: the container library old C software reaches
 * for instead of hand-rolling its own tree, hash table or linked list --
 * tsearch/tfind/tdelete/twalk (a caller-comparator binary search tree),
 * hcreate/hsearch/hdestroy (+ the reentrant _r trio) for a fixed-capacity
 * hash table, lsearch/lfind (linear array search, lsearch APPENDS on miss)
 * and insque/remque (doubly-linked queue splice). See c/apps/libc/src/
 * search.c for the implementation notes -- in particular the tsearch/twalk
 * shape decision and the hcreate capacity formula, both reverse-engineered
 * against real glibc behaviour and documented there.
 *
 * BASENAME CHECK, per CLAUDE.md's rule about the flat INCDIRS list: no
 * search.h exists under c/kernel, c/drivers, c/net, c/fs or c/lib, so this
 * lives at the top level rather than under include/uonly/. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- insque / remque ---------------------------------------------------
 * Generic doubly-linked-list splice. The only assumption made about `elem`
 * and `prev` is that the object POINTED TO begins with two pointers, next
 * then previous -- exactly the layout below, which is why a caller's own
 * struct works as long as those two fields come first. `struct qelem` is
 * provided as a convenience for code that has no payload struct of its own
 * yet (glibc guards it behind __USE_GNU; there is no such macro here, and
 * declaring it unconditionally costs nothing). */
struct qelem {
    struct qelem *q_forw;
    struct qelem *q_back;
    char q_data[1];
};

void insque(void *elem, void *prev);
void remque(void *elem);

/* ---- hsearch family ------------------------------------------------------
 * `__compar_fn_t` is guarded the way glibc guards it: <stdlib.h> or another
 * header may already have defined the same typedef, and redefining an
 * identical typedef is legal in C11 but there is no reason to rely on that
 * when a guard is one line. */
#ifndef __COMPAR_FN_T
#define __COMPAR_FN_T
typedef int (*__compar_fn_t)(const void *, const void *);
#endif

typedef enum { FIND, ENTER } ACTION;

typedef struct entry {
    char *key;
    void *data;
} ENTRY;

/* Opaque: the real slot layout (hash + ENTRY) is search.c's business only.
 * `size`/`filled` are named to match glibc's public struct exactly, since a
 * ported program that pokes at ->filled for a fill-ratio metric expects
 * those names to exist. */
struct __hsearch_entry;
struct hsearch_data {
    struct __hsearch_entry *table;
    unsigned int size;
    unsigned int filled;
};

int    hcreate(size_t nel);
ENTRY *hsearch(ENTRY item, ACTION action);
void   hdestroy(void);

int  hcreate_r(size_t nel, struct hsearch_data *htab);
int  hsearch_r(ENTRY item, ACTION action, ENTRY **retval, struct hsearch_data *htab);
void hdestroy_r(struct hsearch_data *htab);

/* ---- tsearch family --------------------------------------------------- */

typedef enum { preorder, postorder, endorder, leaf } VISIT;

#ifndef __ACTION_FN_T
#define __ACTION_FN_T
typedef void (*__action_fn_t)(const void *nodep, VISIT value, int level);
#endif

void *tsearch(const void *key, void **rootp, __compar_fn_t compar);
void *tfind(const void *key, void *const *rootp, __compar_fn_t compar);
void *tdelete(const void *restrict key, void **restrict rootp, __compar_fn_t compar);
void  twalk(const void *root, __action_fn_t action);

typedef void (*__free_fn_t)(void *nodep);
void tdestroy(void *root, __free_fn_t freefct);

/* ---- linear search ------------------------------------------------------
 * compar follows the memcmp/bsearch convention (0 == match), not qsort's
 * ordering convention -- lsearch/lfind have no notion of "less than". */

void *lfind(const void *key, const void *base, size_t *nmemb, size_t size, __compar_fn_t compar);
void *lsearch(const void *key, void *base, size_t *nmemb, size_t size, __compar_fn_t compar);

#ifdef __cplusplus
}
#endif

#endif /* _SEARCH_H */
