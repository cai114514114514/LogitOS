#ifndef _SYS_QUEUE_H
#define _SYS_QUEUE_H

/* Intrusive BSD queues.  These are macro-only by design: an element owns its
 * linkage, so insertion never allocates and is usable in libc-free low-level
 * ports as well as ordinary applications. */
#include <stddef.h>

#define SLIST_HEAD(name, type) struct name { struct type *slh_first; }
#define SLIST_HEAD_INITIALIZER(head) { NULL }
#define SLIST_ENTRY(type) struct { struct type *sle_next; }
#define SLIST_INIT(head) ((head)->slh_first = NULL)
#define SLIST_EMPTY(head) ((head)->slh_first == NULL)
#define SLIST_FIRST(head) ((head)->slh_first)
#define SLIST_NEXT(elm, field) ((elm)->field.sle_next)
#define SLIST_FOREACH(var, head, field) \
    for ((var) = SLIST_FIRST(head); (var); (var) = SLIST_NEXT(var, field))
#define SLIST_FOREACH_SAFE(var, head, field, tmp) \
    for ((var) = SLIST_FIRST(head); \
         (var) && (((tmp) = SLIST_NEXT(var, field)), 1); (var) = (tmp))
#define SLIST_INSERT_HEAD(head, elm, field) do { \
    SLIST_NEXT(elm, field) = SLIST_FIRST(head); SLIST_FIRST(head) = (elm); \
} while (0)
#define SLIST_INSERT_AFTER(pos, elm, field) do { \
    SLIST_NEXT(elm, field) = SLIST_NEXT(pos, field); SLIST_NEXT(pos, field) = (elm); \
} while (0)
#define SLIST_REMOVE_HEAD(head, field) \
    (SLIST_FIRST(head) = SLIST_NEXT(SLIST_FIRST(head), field))
#define SLIST_REMOVE_AFTER(pos, field) \
    (SLIST_NEXT(pos, field) = SLIST_NEXT(SLIST_NEXT(pos, field), field))
#define SLIST_REMOVE(head, elm, type, field) do { \
    if (SLIST_FIRST(head) == (elm)) SLIST_REMOVE_HEAD(head, field); \
    else { struct type *__q = SLIST_FIRST(head); \
        while (__q && SLIST_NEXT(__q, field) != (elm)) __q = SLIST_NEXT(__q, field); \
        if (__q) SLIST_REMOVE_AFTER(__q, field); } \
} while (0)

#define LIST_HEAD(name, type) struct name { struct type *lh_first; }
#define LIST_HEAD_INITIALIZER(head) { NULL }
#define LIST_ENTRY(type) struct { struct type *le_next; struct type **le_prev; }
#define LIST_INIT(head) ((head)->lh_first = NULL)
#define LIST_EMPTY(head) ((head)->lh_first == NULL)
#define LIST_FIRST(head) ((head)->lh_first)
#define LIST_NEXT(elm, field) ((elm)->field.le_next)
#define LIST_FOREACH(var, head, field) \
    for ((var) = LIST_FIRST(head); (var); (var) = LIST_NEXT(var, field))
#define LIST_FOREACH_SAFE(var, head, field, tmp) \
    for ((var) = LIST_FIRST(head); \
         (var) && (((tmp) = LIST_NEXT(var, field)), 1); (var) = (tmp))
#define LIST_INSERT_HEAD(head, elm, field) do { \
    if ((LIST_NEXT(elm, field) = LIST_FIRST(head)) != NULL) \
        LIST_FIRST(head)->field.le_prev = &LIST_NEXT(elm, field); \
    LIST_FIRST(head) = (elm); (elm)->field.le_prev = &LIST_FIRST(head); \
} while (0)
#define LIST_INSERT_AFTER(pos, elm, field) do { \
    if ((LIST_NEXT(elm, field) = LIST_NEXT(pos, field)) != NULL) \
        LIST_NEXT(pos, field)->field.le_prev = &LIST_NEXT(elm, field); \
    LIST_NEXT(pos, field) = (elm); (elm)->field.le_prev = &LIST_NEXT(pos, field); \
} while (0)
#define LIST_INSERT_BEFORE(pos, elm, field) do { \
    (elm)->field.le_prev = (pos)->field.le_prev; \
    LIST_NEXT(elm, field) = (pos); *(pos)->field.le_prev = (elm); \
    (pos)->field.le_prev = &LIST_NEXT(elm, field); \
} while (0)
#define LIST_REMOVE(elm, field) do { \
    if (LIST_NEXT(elm, field) != NULL) \
        LIST_NEXT(elm, field)->field.le_prev = (elm)->field.le_prev; \
    *(elm)->field.le_prev = LIST_NEXT(elm, field); \
} while (0)

#define STAILQ_HEAD(name, type) struct name { struct type *stqh_first; struct type **stqh_last; }
#define STAILQ_HEAD_INITIALIZER(head) { NULL, &(head).stqh_first }
#define STAILQ_ENTRY(type) struct { struct type *stqe_next; }
#define STAILQ_INIT(head) do { (head)->stqh_first = NULL; (head)->stqh_last = &(head)->stqh_first; } while (0)
#define STAILQ_EMPTY(head) ((head)->stqh_first == NULL)
#define STAILQ_FIRST(head) ((head)->stqh_first)
#define STAILQ_LAST(head, type, field) (STAILQ_EMPTY(head) ? NULL : \
    (struct type *)((char *)(head)->stqh_last - offsetof(struct type, field.stqe_next)))
#define STAILQ_NEXT(elm, field) ((elm)->field.stqe_next)
#define STAILQ_FOREACH(var, head, field) \
    for ((var) = STAILQ_FIRST(head); (var); (var) = STAILQ_NEXT(var, field))
#define STAILQ_FOREACH_SAFE(var, head, field, tmp) \
    for ((var) = STAILQ_FIRST(head); \
         (var) && (((tmp) = STAILQ_NEXT(var, field)), 1); (var) = (tmp))
#define STAILQ_INSERT_HEAD(head, elm, field) do { \
    if ((STAILQ_NEXT(elm, field) = STAILQ_FIRST(head)) == NULL) (head)->stqh_last = &STAILQ_NEXT(elm, field); \
    STAILQ_FIRST(head) = (elm); \
} while (0)
#define STAILQ_INSERT_TAIL(head, elm, field) do { \
    STAILQ_NEXT(elm, field) = NULL; \
    *(head)->stqh_last = (elm); (head)->stqh_last = &STAILQ_NEXT(elm, field); \
} while (0)
#define STAILQ_INSERT_AFTER(head, pos, elm, field) do { \
    if ((STAILQ_NEXT(elm, field) = STAILQ_NEXT(pos, field)) == NULL) (head)->stqh_last = &STAILQ_NEXT(elm, field); \
    STAILQ_NEXT(pos, field) = (elm); \
} while (0)
#define STAILQ_REMOVE_HEAD(head, field) do { \
    if ((STAILQ_FIRST(head) = STAILQ_NEXT(STAILQ_FIRST(head), field)) == NULL) (head)->stqh_last = &STAILQ_FIRST(head); \
} while (0)
#define STAILQ_REMOVE_AFTER(head, pos, field) do { \
    if ((STAILQ_NEXT(pos, field) = STAILQ_NEXT(STAILQ_NEXT(pos, field), field)) == NULL) \
        (head)->stqh_last = &STAILQ_NEXT(pos, field); \
} while (0)
#define STAILQ_REMOVE(head, elm, type, field) do { \
    if (STAILQ_FIRST(head) == (elm)) STAILQ_REMOVE_HEAD(head, field); \
    else { struct type *__q = STAILQ_FIRST(head); \
        while (__q && STAILQ_NEXT(__q, field) != (elm)) __q = STAILQ_NEXT(__q, field); \
        if (__q) STAILQ_REMOVE_AFTER(head, __q, field); } \
} while (0)
#define STAILQ_CONCAT(a, b) do { \
    if (!STAILQ_EMPTY(b)) { *(a)->stqh_last = STAILQ_FIRST(b); \
        (a)->stqh_last = (b)->stqh_last; STAILQ_INIT(b); } \
} while (0)

#define TAILQ_HEAD(name, type) struct name { struct type *tqh_first; struct type **tqh_last; }
#define TAILQ_HEAD_INITIALIZER(head) { NULL, &(head).tqh_first }
#define TAILQ_ENTRY(type) struct { struct type *tqe_next; struct type **tqe_prev; }
#define TAILQ_INIT(head) do { (head)->tqh_first = NULL; (head)->tqh_last = &(head)->tqh_first; } while (0)
#define TAILQ_EMPTY(head) ((head)->tqh_first == NULL)
#define TAILQ_FIRST(head) ((head)->tqh_first)
#define TAILQ_LAST(head, headname) (*(((struct headname *)((head)->tqh_last))->tqh_last))
#define TAILQ_NEXT(elm, field) ((elm)->field.tqe_next)
#define TAILQ_PREV(elm, headname, field) (*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))
#define TAILQ_FOREACH(var, head, field) \
    for ((var) = TAILQ_FIRST(head); (var); (var) = TAILQ_NEXT(var, field))
#define TAILQ_FOREACH_REVERSE(var, head, headname, field) \
    for ((var) = TAILQ_LAST(head, headname); (var); (var) = TAILQ_PREV(var, headname, field))
#define TAILQ_FOREACH_SAFE(var, head, field, tmp) \
    for ((var) = TAILQ_FIRST(head); \
         (var) && (((tmp) = TAILQ_NEXT(var, field)), 1); (var) = (tmp))
#define TAILQ_INSERT_HEAD(head, elm, field) do { \
    if (((elm)->field.tqe_next = (head)->tqh_first) != NULL) \
        (head)->tqh_first->field.tqe_prev = &(elm)->field.tqe_next; \
    else (head)->tqh_last = &(elm)->field.tqe_next; \
    (head)->tqh_first = (elm); (elm)->field.tqe_prev = &(head)->tqh_first; \
} while (0)
#define TAILQ_INSERT_TAIL(head, elm, field) do { \
    (elm)->field.tqe_next = NULL; (elm)->field.tqe_prev = (head)->tqh_last; \
    *(head)->tqh_last = (elm); (head)->tqh_last = &(elm)->field.tqe_next; \
} while (0)
#define TAILQ_INSERT_AFTER(head, pos, elm, field) do { \
    if (((elm)->field.tqe_next = (pos)->field.tqe_next) != NULL) \
        (elm)->field.tqe_next->field.tqe_prev = &(elm)->field.tqe_next; \
    else (head)->tqh_last = &(elm)->field.tqe_next; \
    (pos)->field.tqe_next = (elm); (elm)->field.tqe_prev = &(pos)->field.tqe_next; \
} while (0)
#define TAILQ_INSERT_BEFORE(pos, elm, field) do { \
    (elm)->field.tqe_prev = (pos)->field.tqe_prev; (elm)->field.tqe_next = (pos); \
    *(pos)->field.tqe_prev = (elm); (pos)->field.tqe_prev = &(elm)->field.tqe_next; \
} while (0)
#define TAILQ_REMOVE(head, elm, field) do { \
    if ((elm)->field.tqe_next) (elm)->field.tqe_next->field.tqe_prev = (elm)->field.tqe_prev; \
    else (head)->tqh_last = (elm)->field.tqe_prev; \
    *(elm)->field.tqe_prev = (elm)->field.tqe_next; \
} while (0)
#define TAILQ_CONCAT(a, b, field) do { \
    if (!TAILQ_EMPTY(b)) { \
        *(a)->tqh_last = (b)->tqh_first; (b)->tqh_first->field.tqe_prev = (a)->tqh_last; \
        (a)->tqh_last = (b)->tqh_last; TAILQ_INIT(b); } \
} while (0)

#endif /* _SYS_QUEUE_H */
