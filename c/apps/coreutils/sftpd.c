/* SFTP v3, executed by sshd AFTER dropping to the authenticated account.
 * Its stdin/stdout are the SSH channel: no listener, account lookup, or root
 * helper lives here. The ordinary VFS permission checks therefore apply to
 * every operation, including links and paths outside the user's home.
 *
 * The v3 protocol is the one stock OpenSSH sftp/scp speak. Eight opaque
 * handles and 64 KiB requests bound memory; requests are processed in order
 * but clients may pipeline them. Unsupported metadata is refused explicitly:
 * the ABI has no timestamp setter or atomic O_EXCL, so neither is advertised
 * or reported successful. See draft-ietf-secsh-filexfer-02. */
#ifdef SFTPD_HOST
#include "sftpd_host.h"
#else
#include "clib.h"
#include "logit_stat.h"
#endif

#define SF_MAX 65536
#define SF_PATH 256
#define SF_HANDLES 8
enum { SF_OK, SF_EOF, SF_NOENT, SF_PERM, SF_FAIL, SF_BAD, SF_NOCONN, SF_LOST, SF_UNSUPPORTED };
static unsigned char in[SF_MAX], out[SF_MAX + 4];
static int pos, end, bad, used;
static unsigned int next_handle;
struct handle { unsigned int token; int fd, dir, cursor; char path[SF_PATH]; };
static struct handle handles[SF_HANDLES];

static unsigned int u32(void)
{
    if (pos > end - 4) { bad = 1; return 0; }
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | in[pos++];
    return v;
}
static unsigned long long u64(void)
{ unsigned long long hi = u32(); return (hi << 32) | u32(); }
static const unsigned char *str(int *len)
{
    unsigned int n = u32();
    if (bad || n > (unsigned int)(end - pos)) { bad = 1; *len = 0; return in; }
    const unsigned char *p = in + pos; pos += n; *len = n; return p;
}
static void put32(unsigned int v)
{ if (used > (int)sizeof out - 4) { bad = 1; return; } for (int i = 3; i >= 0; i--) out[used++] = v >> (i * 8); }
static void put64(unsigned long long v) { put32(v >> 32); put32(v); }
static void bytes(const void *data, int n)
{ const unsigned char *p = data; if (n < 0 || n > (int)sizeof out - used) { bad = 1; return; } for (int i = 0; i < n; i++) out[used++] = p[i]; }
static void string(const void *p, int n) { put32(n); bytes(p, n); }
static void text(const char *p) { string(p, c_strlen(p)); }
static void begin(int type, unsigned int id) { used = 4; out[used++] = type; put32(id); }
static int exact(int fd, void *b, int n, int writing)
{
    int done = 0;
    while (done < n) {
        int r = writing ? sys_write(fd, (char *)b + done, n - done) : sys_read(fd, (char *)b + done, n - done);
        if (r == SIG_E_INTR) continue;
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}
static void status(unsigned int id, int code)
{
    static const char *messages[] = { "OK", "End of file", "No such file", "Permission denied", "Filesystem operation failed", "Bad request", "No connection", "Connection lost", "Operation unsupported by this filesystem" };
    begin(101, id); put32(code); text(messages[code]); text("");
}
static int path(char *p)
{
    int n; const unsigned char *s = str(&n);
    if (bad || n >= SF_PATH) { bad = 1; return -1; }
    for (int i = 0; i < n; i++) { if (!s[i]) { bad = 1; return -1; } p[i] = s[i]; }
    p[n] = 0; return 0;
}
static struct handle *get_handle(void)
{
    int n; const unsigned char *p = str(&n); unsigned int token = 0;
    if (bad || n != 4) return 0;
    for (int i = 0; i < 4; i++) token = (token << 8) | p[i];
    for (unsigned int i = 0; i < SF_HANDLES; i++) if (token && handles[i].token == token) return &handles[i];
    return 0;
}
struct attrs { unsigned int flags, uid, gid, mode, atime, mtime; unsigned long long size; };
static struct attrs attrs(void)
{
    struct attrs a = {0}; a.flags = u32();
    if (a.flags & ~15u) { bad = 1; return a; }
    if (a.flags & 1) a.size = u64();
    if (a.flags & 2) { a.uid = u32(); a.gid = u32(); }
    if (a.flags & 4) a.mode = u32();
    if (a.flags & 8) { a.atime = u32(); a.mtime = u32(); }
    return a;
}
static void put_attrs(const struct logit_stat *s)
{
    put32(7 | ((s->attr & LSTA_TIMES) ? 8 : 0));
    put64(s->size); put32(s->uid); put32(s->gid); put32(s->mode);
    if (s->attr & LSTA_TIMES) { put32(s->atime); put32(s->mtime); }
}
/* Resolve directory components and symlinks for REALPATH, not just '..'.
 * No chdir is needed, and a bounded link walk cannot change later requests'
 * interpretation of a relative name. */
static int canonical(const char *source, char *dest)
{
    char work[SF_PATH], next[SF_PATH], link[SF_PATH];
    if (source[0] != '/') {
        if (sys_getcwd(work, sizeof work) < 0) return -1;
        int n = c_strlen(work), m = c_strlen(source);
        if (n + m + 2 > SF_PATH) return -1;
        work[n++] = '/'; for (int i = 0; i <= m; i++) work[n+i] = source[i];
    } else c_strcpy(work, source, sizeof work);
    for (int depth = 0; depth < 16; depth++) {
        int w = 1, i = 1, restart = 0; dest[0] = '/'; dest[1] = 0;
        while (work[i]) {
            while (work[i] == '/') i++;
            int start = i; while (work[i] && work[i] != '/') i++;
            int n = i - start; if (!n) break;
            if (n == 1 && work[start] == '.') continue;
            if (n == 2 && work[start] == '.' && work[start+1] == '.') {
                while (w > 1 && dest[w-1] != '/') w--;
                if (w > 1) w--; dest[w] = 0; continue;
            }
            int parent = w;
            if (w > 1) dest[w++] = '/';
            if (w + n >= SF_PATH) return -1;
            for (int k = 0; k < n; k++) dest[w++] = work[start+k]; dest[w] = 0;
            struct logit_stat s;
            if (st_lstat(dest, &s) < 0) return -1;
            if ((s.mode & LST_IFMT) == LST_IFLNK) {
                int z = st_readlink(dest, link, sizeof link - 1); if (z < 0) return -1; link[z] = 0;
                int a = 0;
                if (link[0] != '/') { for (int k = 0; k < parent; k++) next[a++] = dest[k]; if (a > 1) next[a++] = '/'; }
                if (a + z + c_strlen(work+i) >= SF_PATH) return -1;
                for (int k = 0; k < z; k++) next[a++] = link[k];
                for (int k = i;; k++) { next[a++] = work[k]; if (!work[k]) break; }
                c_strcpy(work, next, sizeof work); restart = 1; break;
            }
        }
        if (!restart) return 0;
    }
    return -1;
}
static int set_attrs(const char *p, int fd, struct attrs a)
{
    /* Reject unsupported fields BEFORE making changes. No timestamp syscall
     * exists, and a pathname chmod is not a correct fchmod after rename. */
    if ((a.flags & 8) || (fd >= 0 && (a.flags & 6)) || a.size > 0x7fffffffffffffffULL) return SF_UNSUPPORTED;
    if (a.flags & 1) {
        int own = fd < 0; if (own) fd = sys_open(p, O_WRONLY);
        int r = fd < 0 ? -1 : (int)_sys(SYS_FTRUNCATE, fd, a.size, 0);
        if (own && fd >= 0 && sys_close(fd) < 0) r = -1;
        if (r < 0) return SF_FAIL;
    }
    if ((a.flags & 2) && st_chown(p, a.uid, a.gid) < 0) return SF_PERM;
    if ((a.flags & 4) && st_chmod(p, a.mode & 0777) < 0) return SF_PERM;
    return SF_OK;
}
static void request(int type, unsigned int id)
{
    char p[SF_PATH], q[SF_PATH]; struct logit_stat s;
    struct handle *h = 0; int rc = SF_FAIL;
    switch (type) {
    case 3: case 11: { /* OPEN / OPENDIR */
        if (path(p) < 0) break;
        unsigned int flags = type == 3 ? u32() : 0;
        struct attrs a = {0}; if (type == 3) a = attrs();
        if (bad || pos != end) break;
        if ((flags & ~31u) || (flags & 16 && !(flags & 2)) || (a.flags & ~4u)) { rc = SF_UNSUPPORTED; break; }
        for (int i = 0; i < SF_HANDLES; i++) if (!handles[i].token) { h = &handles[i]; break; }
        if (!h) break;
        if (type == 11) {
            if (st_stat(p, &s) < 0 || (s.mode & LST_IFMT) != LST_IFDIR) { rc = SF_NOENT; break; }
            h->fd = -1;
        } else {
            if (!(flags & 3)) { rc = SF_BAD; break; }
            int mode = (flags & 3) == 3 ? O_RDWR : (flags & 2) ? O_WRONLY : O_RDONLY;
            if (flags & 4) mode |= O_APPEND;
            if (flags & 8) mode |= O_CREAT;
            if (flags & 16) mode |= O_TRUNC;
            int exists = st_lstat(p, &s) == 0;
            h->fd = sys_open(p, mode); if (h->fd < 0) { rc = SF_PERM; break; }
            if (st_fstat(h->fd, &s) < 0 || (s.mode & LST_IFMT) != LST_IFREG) { sys_close(h->fd); break; }
            /* O_CREAT is buffered on LogitOS: fsync materializes the empty
             * file before pathname metadata can be set. No payload is
             * written until its requested permissions are in place. */
            if (!exists && (a.flags & 4) &&
                (_sys(SYS_FSYNC,h->fd,0,0)<0 || st_chmod(p, a.mode & 0777) < 0)) { sys_close(h->fd); break; }
        }
        h->dir = type == 11; h->cursor = 0; c_strcpy(h->path, p, sizeof h->path);
        h->token = ++next_handle; if (!h->token) h->token = ++next_handle;
        begin(102, id); put32(4); put32(h->token); return;
    }
    case 4: /* CLOSE */
        h = get_handle(); if (!h || bad || pos != end) break;
        rc = h->dir || sys_close(h->fd) == 0 ? SF_OK : SF_FAIL; h->token = 0; break;
    case 5: case 6: { /* READ / WRITE */
        h = get_handle(); unsigned long long offset = u64();
        unsigned int n; const unsigned char *data = 0;
        if (type == 5) n = u32(); else { int z; data = str(&z); n = z; }
        if (!h || h->dir || bad || pos != end || offset > 0x7fffffffffffffffULL) break;
        if (sys_lseek(h->fd, offset, SEEK_SET) < 0) break;
        if (type == 6) {
#ifdef SFTPD_DISABLE_WRITE
            rc = SF_FAIL; /* private test build: the real put must fail */
#else
            rc = exact(h->fd, (void *)data, n, 1) < 0 ? SF_FAIL : SF_OK;
#endif
            break;
        }
        if (n > SF_MAX - 16) n = SF_MAX - 16;
        int z = sys_read(h->fd, out + 13, n);
        if (z < 0) break; if (!z && n) { rc = SF_EOF; break; }
        begin(103, id); put32(z); used += z; return;
    }
    case 7: case 17: /* LSTAT / STAT */
        if (path(p) < 0 || pos != end) break;
        if ((type == 7 ? st_lstat(p, &s) : st_stat(p, &s)) < 0) { rc = SF_NOENT; break; }
        begin(105, id); put_attrs(&s); return;
    case 8: /* FSTAT */
        h = get_handle(); if (!h || h->dir || pos != end || st_fstat(h->fd, &s) < 0) break;
        begin(105, id); put_attrs(&s); return;
    case 9: case 10: { /* SETSTAT / FSETSTAT */
        if (type == 9) { if (path(p) < 0) break; } else { h = get_handle(); if (!h || h->dir) break; }
        struct attrs a = attrs(); if (bad || pos != end) break;
        rc = set_attrs(type == 9 ? p : h->path, h ? h->fd : -1, a); break;
    }
    case 12: { /* READDIR */
        h = get_handle(); if (!h || !h->dir || bad || pos != end) break;
        struct logit_dirent d[16]; int n = st_getdents(h->path, &h->cursor, d, 16);
        if (n < 0) break; if (!n) { rc = SF_EOF; break; }
        begin(104, id); put32(n);
        for (int i = 0; i < n; i++) {
            int a = c_strlen(h->path), b = c_strlen(d[i].name);
            if (a + b + 2 > SF_PATH) { status(id, SF_FAIL); return; }
            c_strcpy(p, h->path, sizeof p); p[a++] = '/';
            for (int j = 0; j <= b; j++) p[a+j] = d[i].name[j];
            if (st_lstat(p, &s) < 0) { status(id, SF_FAIL); return; }
            text(d[i].name); text(d[i].name); put_attrs(&s);
        }
        return;
    }
    case 13: case 15: /* REMOVE / RMDIR */
        if (path(p) < 0 || pos != end || st_lstat(p, &s) < 0) { rc = SF_NOENT; break; }
        if (((s.mode & LST_IFMT) == LST_IFDIR) != (type == 15)) break;
        rc = delete_file(p) < 0 ? SF_FAIL : SF_OK; break;
    case 14: { /* MKDIR */
        if (path(p) < 0) break; struct attrs a = attrs(); if (bad || pos != end) break;
        if (a.flags & ~4u) { rc = SF_UNSUPPORTED; break; }
        if (make_dir(p) < 0) break;
        rc = (a.flags & 4) && st_chmod(p, a.mode & 0777) < 0 ? SF_FAIL : SF_OK; break;
    }
    case 16: /* REALPATH */
        if (path(p) < 0 || pos != end || canonical(p, q) < 0 || st_stat(q, &s) < 0) { rc = SF_NOENT; break; }
        begin(104, id); put32(1); text(q); text(q); put_attrs(&s); return;
    case 18: /* v3 RENAME must not overwrite; ABI rename replaces atomically,
              * but has no atomic no-replace flag. Refuse that stronger claim. */
        rc = SF_UNSUPPORTED; break;
    case 19: /* READLINK */
        if (path(p) < 0 || pos != end) break;
        { int n = st_readlink(p, q, sizeof q); if (n < 0) break;
          begin(104, id); put32(1); string(q, n); string(q, n); put32(0); return; }
    case 20: /* OpenSSH v3 uses target, linkpath (reversed from the draft).
              * Relative targets must be stored literally, not canonicalized
              * against this process's cwd: they resolve at the link's parent. */
        if (path(p) < 0 || path(q) < 0 || pos != end) break;
#ifndef SFTPD_DISABLE_LINKS
        rc = st_symlink(p,q) < 0 ? SF_FAIL : SF_OK;
#endif
        break;
    case 200: /* OpenSSH's explicitly negotiated filesystem extensions. */
        if (path(p) < 0) break;
        if (c_streq(p, "posix-rename@openssh.com")) {
            if (path(p) < 0 || path(q) < 0 || pos != end) break;
            rc = sys_rename(p, q) < 0 ? SF_FAIL : SF_OK;
        } else if (c_streq(p, "fsync@openssh.com")) {
            h = get_handle(); if (!h || h->dir || pos != end) break;
            rc = _sys(SYS_FSYNC, h->fd, 0, 0) < 0 ? SF_FAIL : SF_OK;
        } else if (c_streq(p, "hardlink@openssh.com")) {
            if (path(p) < 0 || path(q) < 0 || pos != end) break;
#ifndef SFTPD_DISABLE_LINKS
            rc = st_link(p,q) < 0 ? SF_FAIL : SF_OK;
#endif
        } else if (c_streq(p, "limits@openssh.com")) {
            if (pos != end) break;
            /* Packet limit includes its four-byte length; WRITE has 25
             * bytes of body framing with our four-byte handle. Declaring
             * SF_MAX as a data limit would make clients overrun the parser. */
            begin(201,id);put64(SF_MAX+4);put64(SF_MAX-16);
            put64(SF_MAX-25);put64(SF_HANDLES);return;
        } else rc = SF_UNSUPPORTED;
        break;
    default: rc = SF_UNSUPPORTED; break;
    }
    status(id, bad ? SF_BAD : rc);
}
int main(void)
{
    int initialized = 0;
    for (;;) {
        if (exact(0, in, 4, 0) < 0) break;
        pos = 0; end = 4; bad = 0; unsigned int n = u32();
        if (n < 5 || n > SF_MAX || exact(0, in, n, 0) < 0) return 1;
        pos = 1; end = n; bad = 0; int type = in[0]; unsigned int id = u32();
        if (!initialized) {
            if (type != 1 || id < 3) return 1;
            begin(2, 3); text("posix-rename@openssh.com"); text("1"); text("fsync@openssh.com"); text("1");
            text("hardlink@openssh.com");text("1");text("limits@openssh.com");text("1");initialized = 1;
        } else request(type, id);
        if (bad && used > SF_MAX) return 1;
        int size = used - 4;
        for (int i = 0; i < 4; i++) out[i] = (unsigned int)size >> (24 - i*8);
        if (exact(1, out, used, 1) < 0) return 1;
    }
    for (int i = 0; i < SF_HANDLES; i++) if (handles[i].token && !handles[i].dir) sys_close(handles[i].fd);
    return 0;
}
