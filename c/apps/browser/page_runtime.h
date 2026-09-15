#ifndef LOGIT_PAGE_RUNTIME_H
#define LOGIT_PAGE_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* Internal ownership, not a Window/iframe implementation. QuickJS and the DOM
 * are borrowed identities: js_page still frees them, AFTER queued payloads.
 * An asynchronous producer must keep the token captured when it began work,
 * never replace it with the current token when its result finally arrives. */
struct page_runtime;
struct page_runtime_token {
    const struct page_runtime *owner;
    uint64_t epoch;
};
enum page_runtime_state { PAGE_RUNTIME_CLOSED, PAGE_RUNTIME_OPEN, PAGE_RUNTIME_CLOSING };
enum page_task_source { PAGE_TASK_TIMER = 1, PAGE_TASK_ANIMATION_FRAME };
enum page_task_result { PAGE_TASK_OK, PAGE_TASK_CLOSED, PAGE_TASK_STALE, PAGE_TASK_FULL, PAGE_TASK_QUEUED };

struct page_task {
    struct page_task *next;
    struct page_runtime_token token;
    uint64_t due, seq;
    unsigned source;
    unsigned queued;
    void *payload;
    void (*dispose)(void *context, void *payload);
};
struct page_runtime {
    void *runtime, *context, *document;
    uint64_t epoch, sequence;
    enum page_runtime_state state;
    struct page_task *tasks;
    size_t count, capacity;
};

/* Zero-initialize once. Reopening a closed owner preserves its generation.
 * No allocation occurs in this module; payload ownership transfers only on a
 * successful enqueue. A refused task remains the producer's responsibility. */
int page_runtime_open(struct page_runtime *, void *runtime, void *context,
                      void *document, size_t capacity);
struct page_runtime_token page_runtime_token(const struct page_runtime *);
int page_runtime_accepts(struct page_runtime_token);
void page_runtime_invalidate(struct page_runtime *);
void page_runtime_close(struct page_runtime *);
int page_task_enqueue(struct page_runtime *, struct page_task *,
                      struct page_runtime_token, uint64_t due, unsigned source,
                      void *payload, void (*dispose)(void *, void *));
/* Detach does not destroy the payload, so dispatch can retain JS references
 * before releasing a one-shot. Cancel does both, exactly once. */
int page_task_detach(struct page_runtime *, struct page_task *);
int page_task_cancel(struct page_runtime *, struct page_task *);
int page_task_rearm(struct page_runtime *, struct page_task *, uint64_t due);
uint64_t page_runtime_turn(const struct page_runtime *);
struct page_task *page_runtime_ready(struct page_runtime *, uint64_t now, uint64_t limit);
long long page_runtime_next_due(const struct page_runtime *);

#endif
