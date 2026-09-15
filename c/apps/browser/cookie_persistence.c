/* SPDX-License-Identifier: MIT */
/* Included once by js_webapi.c, as storage_backend.c is. Host embeddings and
 * the ring-3 browser consequently use the same restore and flush path.
 *
 * The old browser stored no cookies on disk because of historical LogitFS
 * durability failures. This implementation uses its checked whole-file write
 * interface and two CRC-protected, versioned slots, never temp+rename (LogitFS
 * cannot replace an existing destination via rename). Readback proves what the
 * adapter stored; guest power-cycle evidence is still required for durability.
 *
 * Unlike ordinary preference snapshots, a completed Cookie deletion must not
 * leave an older backup that can resurrect a logged-out session. Each flush
 * verifies the new generation in BOTH slots before acknowledging success.
 * The first slot preserves recovery during the second write. Failed writes
 * are uncertain commits, explicitly reported and latched until reopen.
 * This is one browser-process writer, not an interprocess synchronization API.
 */
#include "cookie_persistence.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Disk policy is bounded independently of custom in-memory jar limits. */
#define CK_PERSIST_ENTRY_LIMIT ((unsigned)CK_JAR_MAX_TOTAL)
#define CK_PERSIST_RECORD_MAX (44u + CK_NAME_MAX + CK_VALUE_MAX + CK_DOMAIN_MAX + CK_PATH_MAX)
#define CK_PERSIST_MAX (32u + CK_PERSIST_ENTRY_LIMIT * CK_PERSIST_RECORD_MAX)
static const char *const ckp_paths[2] = { COOKIE_STORE_SLOT0, COOKIE_STORE_SLOT1 };

static uint32_t ckp_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t ckp_u64(const unsigned char *p)
{ return ckp_u32(p) | (uint64_t)ckp_u32(p + 4) << 32; }
static void ckp_put32(unsigned char *p, uint32_t v)
{ for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (i * 8)); }
static void ckp_put64(unsigned char *p, uint64_t v)
{ ckp_put32(p, (uint32_t)v); ckp_put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t ckp_crc(const unsigned char *p, size_t n)
{
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; i++) {
        crc ^= i >= 28 && i < 32 ? 0 : p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
/* A valid snapshot can come from a later wall clock: the observed profile was
 * written with QEMU base=localtime, then read by a default UTC guest. Calling
 * this corruption disabled every later flush despite correct CRC/permissions.
 * Validate ORIGINAL timestamps and attributes first. Only afterwards move the
 * bookkeeping timestamps by one shared offset, preserving creation order and
 * avoiding accessed<created when the next request records the current clock.
 * Expiry remains an absolute deadline: it is never shifted or extended, and
 * a future snapshot cannot bypass the current 400-day admission ceiling. */
#ifndef COOKIE_PERSIST_CLOCK_STRICT
static void ckp_clock_rebase(struct cookie_jar *j, int64_t now)
{
    int64_t latest = now;
    for (int i = 0; i < j->n; i++) if (j->v[i].accessed > latest) latest = j->v[i].accessed;
    if (latest == now) return;
    int64_t delta = latest - now;
    /* A rollback all the way toward the epoch can collapse several creation
     * timestamps to zero. Stable original creation order then remains the tie
     * breaker used by cookie_header, even for an unordered input snapshot. */
    for (int i = 1; i < j->n; i++) {
        struct cookie c = j->v[i]; int k = i;
        while (k > 0 && j->v[k - 1].created > c.created) { j->v[k] = j->v[k - 1]; k--; }
        j->v[k] = c;
    }
    for (int i = 0; i < j->n; i++) {
        struct cookie *c = &j->v[i];
        c->created = c->created > delta ? c->created - delta : 0;
        c->accessed = c->accessed > delta ? c->accessed - delta : 0;
        /* Each record is validated at its persisted access timestamp when it
         * is reopened. A ceiling based only on `now` can leave an older,
         * untouched record over that record's 400-day limit after rebasing.
         * The next flush then writes a snapshot our own decoder rejects. */
#ifdef COOKIE_PERSIST_GLOBAL_CEILING
        int64_t anchor = now;
#else
        int64_t anchor = c->accessed;
#endif
        int64_t ceiling = anchor > INT64_MAX - CK_MAX_AGE_SECONDS ? INT64_MAX : anchor + CK_MAX_AGE_SECONDS;
        if (c->expires > ceiling) c->expires = ceiling;
    }
}
#endif
static int ckp_decode(const unsigned char *p, size_t n, struct cookie_jar *j,
                       uint64_t *generation, int64_t now)
{
    if (n < 32 || n > CK_PERSIST_MAX || memcmp(p, "LGTCK01", 8) ||
        ckp_u32(p + 8) != 1 || ckp_u32(p + 12) != n || !ckp_u64(p + 16) ||
        ckp_u32(p + 24) > CK_PERSIST_ENTRY_LIMIT || ckp_u32(p + 28) != ckp_crc(p, n))
        return CK_PERSIST_CORRUPT;
    size_t pos = 32;
    unsigned count = ckp_u32(p + 24);
    for (unsigned i = 0; i < count; i++) {
        if (n - pos < 44) return CK_PERSIST_CORRUPT;
        unsigned lengths[4];
        const unsigned limits[4] = { CK_NAME_MAX, CK_VALUE_MAX, CK_DOMAIN_MAX, CK_PATH_MAX };
        for (int k = 0; k < 4; k++) {
            lengths[k] = ckp_u32(p + pos + 4 * k);
            if (lengths[k] > limits[k]) return CK_PERSIST_CORRUPT;
        }
        uint32_t flags = ckp_u32(p + pos + 16);
        uint64_t expires = ckp_u64(p + pos + 20), created = ckp_u64(p + pos + 28);
        uint64_t accessed = ckp_u64(p + pos + 36);
        if (flags & ~127u || !expires || expires > INT64_MAX ||
            created > INT64_MAX || accessed > INT64_MAX) return CK_PERSIST_CORRUPT;
        pos += 44;
        struct cookie c;
        memset(&c, 0, sizeof c);
        char *strings[4] = { NULL, NULL, NULL, NULL };
        int rc = CK_PERSIST_CORRUPT;
        for (int k = 0; k < 4; k++) {
            if (lengths[k] > n - pos || memchr(p + pos, 0, lengths[k])) goto release;
            strings[k] = malloc((size_t)lengths[k] + 1);
            if (!strings[k]) { rc = CK_PERSIST_NOMEM; goto release; }
            memcpy(strings[k], p + pos, lengths[k]); strings[k][lengths[k]] = 0;
            pos += lengths[k];
        }
        c.name = strings[0]; c.value = strings[1]; c.domain = strings[2]; c.path = strings[3];
        c.persistent = (flags & 1) != 0; c.host_only = (flags & 2) != 0;
        c.secure = (flags & 4) != 0; c.http_only = (flags & 8) != 0;
        c.samesite = (int)(flags >> 4);
        c.expires = (int64_t)expires; c.created = (int64_t)created; c.accessed = (int64_t)accessed;
        if (!c.persistent || c.samesite > CK_SS_STRICT) goto release;
        for (int k = 0; k < j->n; k++)
            if (!strcmp(j->v[k].name, c.name) && !strcmp(j->v[k].domain, c.domain) &&
                !strcmp(j->v[k].path, c.path) && j->v[k].host_only == c.host_only) goto release;
        /* Validate expired records too, so age cannot hide malformed flags or
         * domain data. GC is applied only after the entire snapshot validates.
         * The core owns validation; this decoder must never reproduce a weaker
         * prefix/PSL/parser rule in a second place. */
        if (c.created < 0 || c.accessed < c.created || c.expires <= c.accessed)
            goto release;
#ifdef COOKIE_PERSIST_CLOCK_STRICT
        if (c.accessed > now) goto release;
        int64_t validate_now = c.expires <= now ? c.accessed : now;
#else
        /* The persisted access clock is a lower bound on the original expiry
         * validation time. It admits a legitimate clock rollback but still
         * rejects malformed stored lifetime/flags/domain/prefix before clamp. */
        int64_t validate_now = c.accessed;
#endif
        if (cookie_restore_entry(j, &c, validate_now) < 0) goto release;
        rc = CK_PERSIST_OK;
release:
        for (int k = 0; k < 4; k++) free(strings[k]);
        if (rc != CK_PERSIST_OK) return rc;
    }
    if (pos != n) return CK_PERSIST_CORRUPT;
#ifndef COOKIE_PERSIST_CLOCK_STRICT
    ckp_clock_rebase(j, now);
#endif
    cookie_jar_gc(j, now);
    *generation = ckp_u64(p + 16);
    return CK_PERSIST_OK;
}

int cookie_persistence_open(struct cookie_persistence *s, struct cookie_jar *jar,
                            const struct bstore_ops *ops, int64_t now)
{
    if (!s || !jar) return CK_PERSIST_IO;
    cookie_jar_free(jar); cookie_jar_init(jar);
    free(s->snapshot);
    memset(s, 0, sizeof *s); s->ops = ops; s->active_slot = -1;
    if (!ops) return CK_PERSIST_OK;
    if (now < 0) return s->status = CK_PERSIST_IO;
    if (!ops->read || !ops->write || !ops->mkdir || ops->mkdir(COOKIE_STORE_DIR) < 0)
        return s->status = CK_PERSIST_IO;
    struct cookie_jar candidates[2];
    uint64_t generation[2] = { 0, 0 };
    unsigned char *snapshots[2] = { NULL, NULL };
    unsigned snapshot_sizes[2] = { 0, 0 };
    int status[2] = { CK_PERSIST_IO, CK_PERSIST_IO };
    for (int i = 0; i < 2; i++) {
        cookie_jar_init(&candidates[i]);
        /* LogitFS read_file is all-or-nothing, not fread: reading a small
         * header first would classify every actual snapshot as missing. */
#ifdef COOKIE_PERSIST_PREFIX_READ
        size_t capacity = 32;
#else
        size_t capacity = CK_PERSIST_MAX + 1u;
#endif
        unsigned char *bytes = malloc(capacity);
        if (!bytes) status[i] = CK_PERSIST_NOMEM;
        else {
            int got = ops->read(ckp_paths[i], bytes, (int)capacity);
            /* bstore traditionally conflates missing and read failure. The
             * private guest adapter returns -2 for known I/O/permission errors;
             * -1 remains missing for older host embeddings. */
            status[i] = got == -1 ? 1 : got < 0 ? CK_PERSIST_IO :
                ckp_decode(bytes, (size_t)got, &candidates[i], &generation[i], now);
            if (status[i] == CK_PERSIST_OK) {
                unsigned char *small = realloc(bytes, (size_t)got);
                snapshots[i] = small ? small : bytes;
                snapshot_sizes[i] = (unsigned)got;
                bytes = NULL;
            }
            free(bytes);
        }
    }
    int pick = status[0] == CK_PERSIST_OK ? 0 : -1;
    if (status[1] == CK_PERSIST_OK && (pick < 0 || generation[1] > generation[pick])) pick = 1;
    if (status[0] == CK_PERSIST_NOMEM || status[1] == CK_PERSIST_NOMEM) s->status = CK_PERSIST_NOMEM;
    else if (status[0] == CK_PERSIST_IO || status[1] == CK_PERSIST_IO) s->status = CK_PERSIST_IO;
    else if (pick >= 0) {
        *jar = candidates[pick]; cookie_jar_init(&candidates[pick]);
        s->generation = generation[pick]; s->active_slot = pick;
        s->snapshot = snapshots[pick]; s->snapshot_size = snapshot_sizes[pick];
        snapshots[pick] = NULL;
    } else if (status[0] != 1 || status[1] != 1) s->status = CK_PERSIST_CORRUPT;
    for (int i = 0; i < 2; i++) { cookie_jar_free(&candidates[i]); free(snapshots[i]); }
    return s->status;
}

static int ckp_same_content(const unsigned char *old, unsigned old_size,
                            const unsigned char *fresh, unsigned size)
{
    if (!old || old_size != size || memcmp(old, fresh, 16) || memcmp(old + 24, fresh + 24, 4)) return 0;
    size_t pos = 32;
    for (unsigned i = 0; i < ckp_u32(fresh + 24); i++) {
        if (memcmp(old + pos, fresh + pos, 36)) return 0;
        size_t len = 0;
        for (int k = 0; k < 4; k++) len += ckp_u32(fresh + pos + 4 * k);
        pos += 44;
        if (memcmp(old + pos, fresh + pos, len)) return 0;
        pos += len;
    }
    return pos == size;
}

int cookie_persistence_flush(struct cookie_persistence *s, const struct cookie_jar *jar, int64_t now)
{
    if (!s || !jar) return CK_PERSIST_IO;
    if (!s->ops) return CK_PERSIST_OK;
    if (s->status) return s->status;
    if (s->generation == UINT64_MAX) return s->status = CK_PERSIST_LIMIT;
    unsigned count = 0;
    size_t size = 32;
    for (int i = 0; i < jar->n; i++) {
        const struct cookie *c = &jar->v[i];
        if (!c->persistent || c->expires <= now) continue;
        if (++count > CK_PERSIST_ENTRY_LIMIT) return s->status = CK_PERSIST_LIMIT;
        size += 44 + strlen(c->name) + strlen(c->value) + strlen(c->domain) + strlen(c->path);
        if (size > CK_PERSIST_MAX) return s->status = CK_PERSIST_LIMIT;
    }
    unsigned char *bytes = malloc(size), *readback = malloc(size + 1);
    if (!bytes || !readback) { free(bytes); free(readback); return s->status = CK_PERSIST_NOMEM; }
    memset(bytes, 0, 32); memcpy(bytes, "LGTCK01", 8);
    ckp_put32(bytes + 8, 1); ckp_put32(bytes + 12, (uint32_t)size);
    ckp_put64(bytes + 16, s->generation + 1); ckp_put32(bytes + 24, count);
    size_t pos = 32;
    for (int i = 0; i < jar->n; i++) {
        const struct cookie *c = &jar->v[i];
        if (!c->persistent || c->expires <= now) continue;
        const char *strings[4] = { c->name, c->value, c->domain, c->path };
        for (int k = 0; k < 4; k++) ckp_put32(bytes + pos + 4 * k, (uint32_t)strlen(strings[k]));
        ckp_put32(bytes + pos + 16, 1u | (c->host_only ? 2u : 0u) | (c->secure ? 4u : 0u) |
                  (c->http_only ? 8u : 0u) | ((unsigned)c->samesite << 4));
        ckp_put64(bytes + pos + 20, (uint64_t)c->expires);
        ckp_put64(bytes + pos + 28, (uint64_t)c->created);
        ckp_put64(bytes + pos + 36, (uint64_t)c->accessed); pos += 44;
        for (int k = 0; k < 4; k++) { size_t len = strlen(strings[k]); memcpy(bytes + pos, strings[k], len); pos += len; }
    }
    ckp_put32(bytes + 28, ckp_crc(bytes, size));
    /* A session-only response must not synchronously rewrite the disk twice.
     * Compare exact content, not just a hash (a collision cannot authorize
     * dropping a credential update). Last-access is checkpointed with the next
     * substantive persistent mutation, not on each request/header read. */
    if ((!s->snapshot && !count) || ckp_same_content(s->snapshot, s->snapshot_size, bytes, (unsigned)size)) {
        free(bytes); free(readback); return CK_PERSIST_OK;
    }
    int first = s->active_slot == 0 ? 1 : 0, rc = CK_PERSIST_OK;
#ifndef COOKIE_PERSIST_NO_WRITE
    for (int pass = 0; pass < 2; pass++) {
        int slot = pass == 0 ? first : 1 - first;
        int wrote = s->ops->write(ckp_paths[slot], bytes, (int)size);
        if (wrote != 0 && wrote != (int)size) { rc = CK_PERSIST_IO; break; }
        int got = s->ops->read(ckp_paths[slot], readback, (int)size + 1);
        if (got != (int)size || memcmp(bytes, readback, size)) { rc = CK_PERSIST_IO; break; }
    }
#endif
    free(readback);
    if (rc == CK_PERSIST_OK) {
        s->generation++; s->active_slot = first;
        free(s->snapshot); s->snapshot = bytes; s->snapshot_size = (unsigned)size;
    } else { free(bytes); s->status = rc; }
    return rc;
}
