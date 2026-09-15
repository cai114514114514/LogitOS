/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_COOKIE_PERSISTENCE_H
#define LOGIT_COOKIE_PERSISTENCE_H

#include "cookies.h"
#include "tabs.h"

/* The store must protect COOKIE_STORE_DIR (0700) and its snapshots (0600)
 * before either reading or writing. bstore is reused for its whole-file,
 * checked-write contract; an arbitrary public/default-mode store is unsuitable
 * for cookies. browser.c supplies the private guest adapter. */
#define COOKIE_STORE_DIR BROWSER_DIR "/cookies"
#define COOKIE_STORE_SLOT0 COOKIE_STORE_DIR "/jar.0"
#define COOKIE_STORE_SLOT1 COOKIE_STORE_DIR "/jar.1"
enum { CK_PERSIST_OK = 0, CK_PERSIST_IO = -1, CK_PERSIST_CORRUPT = -2,
       CK_PERSIST_NOMEM = -3, CK_PERSIST_LIMIT = -4 };
struct cookie_persistence {
    const struct bstore_ops *ops;
    uint64_t generation;
    int active_slot;
    int status;
    unsigned char *snapshot;
    unsigned snapshot_size;
};

/* Before the first request, with a zero-initialized state and initialized jar.
 * Reopening discards the
 * process jar, including session cookies. NULL explicitly selects memory only.
 * Unreadable/corrupt data never supplies cookies to a request. Valid future
 * bookkeeping timestamps are rebased after validation if the clock moved
 * backwards; absolute expiry is never extended and is capped at now+400 days.
 * The RTC/Unix clock contract itself belongs to the embedding, not this store. */
int cookie_persistence_open(struct cookie_persistence *, struct cookie_jar *,
                            const struct bstore_ops *, int64_t now);
/* Call after a successful mutation, including deletion/replacement with a
 * session cookie. Every successful return is already flushed by the backend;
 * browser shutdown does not have to run for recovery to work. Failure leaves
 * the current process jar usable but latches persistence failure until reopen.
 * Never log the serialized data or Cookie names/values to diagnose a failure. */
int cookie_persistence_flush(struct cookie_persistence *, const struct cookie_jar *,
                             int64_t now);
#endif
