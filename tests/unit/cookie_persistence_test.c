/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "cookie_persistence.h"

static int checks, failures, writes, write_mode, read_mode, fail_call;
static unsigned char *files[2];
static int sizes[2];
static struct cookie_jar jar;
static struct cookie_persistence state;
static const int64_t now = 1800000000;
static const struct cookie_ctx ctx = { "persist.example", "/account/page", 1, 1 };
static void ck(int ok, const char *label)
{ checks++; if (!ok) failures++; printf("%s: %s\n", ok ? "ok" : "FAIL", label); }
static int slot(const char *path) { return path[strlen(path) - 1] == '1'; }
static int md(const char *path) { (void)path; return 0; }
static int rd(const char *path, void *out, int max)
{
    int i = slot(path);
    if (read_mode) return -2;
    if (!files[i]) return -1;
    if (max < sizes[i]) return -1; /* Real LogitFS whole-file semantics. */
    memcpy(out, files[i], sizes[i]); return sizes[i];
}
static int wr(const char *path, const void *bytes, int len)
{
    writes++; int i = slot(path);
    if (write_mode == 1 || writes == fail_call) return -1;
    if (write_mode == 4) return 0;
    free(files[i]); files[i] = malloc((size_t)len); memcpy(files[i], bytes, len); sizes[i] = len;
    if (write_mode == 2) { sizes[i]--; return len - 1; }
    if (write_mode == 3) files[i][len - 1] ^= 1;
    return write_mode == 5 ? 0 : len;
}
static const struct bstore_ops store = { rd, wr, md };
static void fresh(void)
{
    cookie_persistence_open(&state, &jar, NULL, now);
    for (int i = 0; i < 2; i++) { free(files[i]); files[i] = NULL; sizes[i] = 0; }
    writes = write_mode = read_mode = fail_call = 0;
    ck(cookie_persistence_open(&state, &jar, &store, now) == 0, "fresh private store opens");
}
static int set(const char *line, int64_t when)
{ return cookie_set(&jar, &ctx, line, when) == 0 && cookie_persistence_flush(&state, &jar, when) == 0; }
static int has(const char *name, const char *value)
{
    for (int i = 0; i < jar.n; i++)
        if (!strcmp(jar.v[i].name, name) && !strcmp(jar.v[i].value, value)) return 1;
    return 0;
}
static void seed(void) { fresh(); ck(set("persistent=old; Max-Age=3600; Path=/; Secure; HttpOnly; SameSite=Strict", now), "persistent seed commits"); }
static void fix_crc(unsigned char *p, size_t n)
{
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; i++) {
        crc ^= i >= 28 && i < 32 ? 0 : p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    crc = ~crc; for (int i = 0; i < 4; i++) p[28 + i] = (unsigned char)(crc >> (8 * i));
}
static const char *directory;
static void disk_path(char *out, const char *path)
{ snprintf(out, 1024, "%s/%s", directory, strrchr(path, '/') + 1); }
static int disk_read(const char *path, void *out, int max)
{
    char file[1024]; disk_path(file, path); FILE *f = fopen(file, "rb"); if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -2; }
    long n = ftell(f); if (n < 0 || n > max) { fclose(f); return -1; }
    rewind(f); int got = (int)fread(out, 1, (size_t)max, f); int bad = ferror(f);
    if (fclose(f)) bad = 1; return bad ? -2 : got;
}
static int disk_write(const char *path, const void *bytes, int len)
{
    char file[1024]; disk_path(file, path); mode_t old = umask(0077);
    FILE *f = fopen(file, "wb"); umask(old); if (!f) return -1;
    int bad = fwrite(bytes, 1, (size_t)len, f) != (size_t)len;
    if (fflush(f) || fsync(fileno(f))) bad = 1;
    if (fclose(f)) bad = 1;
    return bad ? -1 : len;
}
static const struct bstore_ops disk = { disk_read, disk_write, md };
int main(int argc, char **argv)
{
    cookie_jar_init(&jar);
    if (argc == 3) {
        directory = argv[2]; ck(cookie_persistence_open(&state, &jar, &disk, now + 5) == 0, "file store opens");
        if (!strcmp(argv[1], "write")) {
            ck(set("persistent=old; Max-Age=3600; Path=/; Secure; HttpOnly; SameSite=Strict", now + 5), "real file persistent mutation flushes");
            ck(set("session=ephemeral; Secure; Path=/", now + 5), "real file session mutation accepted");
        } else {
            ck(has("persistent", "old"), "new process restores exact persistent cookie");
            ck(!has("session", "ephemeral") && jar.n == 1, "new process excludes previous session cookies");
            struct stat st; char path[1024]; disk_path(path, COOKIE_STORE_SLOT0);
            ck(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "host snapshot created with private permissions");
        }
        cookie_jar_free(&jar); return failures ? 1 : 0;
    }
    fresh();
    ck(set("session=ephemeral; Path=/", now) && writes == 0, "fresh session-only jar never writes a snapshot");
    seed();
    int unchanged_writes = writes;
    ck(set("session=ephemeral; Secure; Path=/", now) && writes == unchanged_writes, "session mutation leaves unchanged persistent snapshot untouched");
    char header[CK_HEADER_MAX]; cookie_header(&jar, &ctx, now + 1, header, sizeof header);
    ck(cookie_persistence_flush(&state, &jar, now + 1) == 0 && writes == unchanged_writes, "last-access change alone does not trigger synchronous disk IO");
    ck(set("short=temporary; Max-Age=2; Secure; Path=/", now), "short expiry commits");
    ck(cookie_persistence_open(&state, &jar, &store, now + 3) == 0, "reopen succeeds");
    ck(has("persistent", "old") && jar.n == 1, "reopen restores persistent and drops session plus expired cookies");
    ck(jar.n == 1 && jar.v[0].secure && jar.v[0].http_only && jar.v[0].host_only &&
       jar.v[0].samesite == CK_SS_STRICT && jar.v[0].created == now && jar.v[0].expires == now + 3600,
       "flags scope creation order and expiry survive reload");
    ck(set("persistent=; Max-Age=0; Secure; Path=/", now + 3), "deletion flushes");
    files[0][0] ^= 1;
    ck(cookie_persistence_open(&state, &jar, &store, now + 4) == 0 && jar.n == 0,
       "corrupt one slot after acknowledged deletion cannot resurrect old cookie");
    seed(); ck(set("persistent=ephemeral; Secure; Path=/", now + 1), "persistent replaced by session flushes removal");
    ck(cookie_persistence_open(&state, &jar, &store, now + 2) == 0 && jar.n == 0,
       "session replacement does not restore old persistent cookie");
    seed(); files[0][sizes[0] - 1] ^= 1;
    ck(cookie_persistence_open(&state, &jar, &store, now + 1) == 0 && has("persistent", "old"), "damaged slot recovers verified same generation replica");
    for (int mode = 1; mode <= 4; mode++) {
        seed(); write_mode = mode;
        ck(cookie_set(&jar, &ctx, "persistent=new; Max-Age=3600; Secure; Path=/", now + 1) == 0, "current process accepts mutation before flush");
        ck(cookie_persistence_flush(&state, &jar, now + 1) == CK_PERSIST_IO, "write short write corrupt readback or no-write failure is reported");
        write_mode = 0; int before = writes;
        ck(cookie_persistence_flush(&state, &jar, now + 1) == CK_PERSIST_IO && writes == before,
           "uncertain commit blocks further disk writes until reopen");
        ck(cookie_persistence_open(&state, &jar, &store, now + 2) == 0 && has("persistent", "old"), "failed first slot preserves verified previous generation");
    }
    seed(); fail_call = writes + 2;
    ck(!set("persistent=new; Max-Age=3600; Secure; Path=/", now + 1), "failed replica write is explicit uncertain commit");
    fail_call = 0;
    ck(cookie_persistence_open(&state, &jar, &store, now + 2) == 0 && has("persistent", "new"), "reopen selects verified newest generation after uncertain replica commit");
    seed(); write_mode = 5;
    ck(set("persistent=zero; Max-Age=3600; Secure; Path=/", now + 1), "zero-success backend verified by readback");
    ck(cookie_persistence_open(&state, &jar, &store, now + 2) == 0 && has("persistent", "zero"), "zero-success write really survives reopen");
    seed(); read_mode = 1;
    ck(cookie_persistence_open(&state, &jar, &store, now + 1) == CK_PERSIST_IO && jar.n == 0, "explicit read failure is not silently treated as first launch");
    read_mode = 0;
    for (int i = 0; i < 2; i++) { files[i][48] = 0; fix_crc(files[i], sizes[i]); }
    ck(cookie_persistence_open(&state, &jar, &store, now + 1) == CK_PERSIST_CORRUPT && jar.n == 0,
       "valid checksum cannot make a session record persistent");
    int before = writes;
    ck(cookie_persistence_flush(&state, &jar, now + 1) == CK_PERSIST_CORRUPT && writes == before, "corrupt files are not overwritten with an empty jar");
    seed(); for (int i = 0; i < 2; i++) { files[i][76] = 0; fix_crc(files[i], sizes[i]); }
    ck(cookie_persistence_open(&state, &jar, &store, now + 1) == CK_PERSIST_CORRUPT && jar.n == 0,
       "embedded NUL in snapshot string is rejected despite valid checksum");
    cookie_persistence_open(&state, &jar, NULL, now); for (int i = 0; i < 2; i++) free(files[i]); cookie_jar_free(&jar);
    printf("cookie-persistence: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
