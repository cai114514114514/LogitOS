#ifndef LOGIT_STORAGE_BACKEND_H
#define LOGIT_STORAGE_BACKEND_H
#include <stddef.h>

/* In-memory Web Storage service, independent of JSValue/JSContext. The caller
 * supplies an already canonical origin; this layer does not invent a second
 * URL parser. local areas are process-owned, session areas belong to a stable
 * top-level tab id (not the short-lived document/realm epoch).
 *
 * This is NOT disk persistence, cross-process sharing, or a storage-event bus.
 * Calls are serialized by the browser event loop. A future realm may use the
 * same service only after its binding/lifetime owner is implemented. */
/* Correction (2026-09-09): an installed bstore_ops now persists LOCAL areas
 * with checked dual-slot snapshots. No store still means explicit in-memory
 * embedding. Session storage remains tab-owned; it is never serialized.
 * Configure before opening realms, and keep ops alive for the process. */
struct bstore_ops;
struct storage_key;
int storage_backend_set_store(const struct bstore_ops *);
int storage_backend_status(const struct storage_key *);
enum { STORAGE_LOCAL = 0, STORAGE_SESSION = 1 };
enum { STORAGE_OK = 0, STORAGE_NOMEM = -1, STORAGE_QUOTA = -2, STORAGE_INVALID = -3, STORAGE_IO = -4, STORAGE_CORRUPT = -5, STORAGE_MISSING = -6 };
#define STORAGE_AREA_LIMIT 16
#define STORAGE_ITEM_LIMIT 256
#define STORAGE_BYTE_LIMIT (256u * 1024u)
struct storage_key {
    const char *origin;
    int kind;
    unsigned long long session_id;
};

/* Byte spans retain embedded NULs in JS strings. Returned bytes are borrowed
 * until the next mutation; bindings must copy into their own realm immediately.
 * Reads never reserve an area. A failed set changes neither bytes nor order. */
const char *storage_backend_get(const struct storage_key *, const char *, size_t, size_t *);
const char *storage_backend_key(const struct storage_key *, unsigned int, size_t *);
unsigned int storage_backend_length(const struct storage_key *);
int storage_backend_set(const struct storage_key *, const char *, size_t, const char *, size_t);
int storage_backend_remove(const struct storage_key *, const char *, size_t);
int storage_backend_clear(const struct storage_key *);
/* Call after the tab's live realms/wrappers are gone, not on page navigation.
 * Keys are not retained by the backend, so callers may use stack descriptors. */
void storage_backend_drop_session(unsigned long long);
#ifdef STORAGE_BACKEND_TEST
void storage_backend_fail_alloc_after(int);
#endif
#endif
