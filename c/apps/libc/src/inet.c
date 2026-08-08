/* <arpa/inet.h>: text <-> binary address conversion. Pure computation -- no
 * syscall anywhere in this file -- so "real" here means "matches glibc",
 * which is what tests/unit/libc_inet_test.c checks byte-for-byte against the
 * host's inet_ntop/inet_pton over a large case set (dotted-quad, all-zeros,
 * loopback, IPv4-mapped IPv6, every legal placement of "::", and the ones
 * that must be REJECTED: extra octets, out-of-range decimal, double "::"). */
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>

const struct in6_addr in6addr_any      = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

/* ---- IPv4 --------------------------------------------------------------- */

/* inet_pton(AF_INET): STRICT. Exactly 4 groups of 1-3 decimal digits, each
 * 0-255, separated by '.', nothing else -- unlike inet_aton below, this never
 * accepts octal/hex or fewer than 4 parts. That strictness is the documented
 * POSIX difference between the two calls, and callers (getaddrinfo-alikes)
 * depend on inet_pton REJECTING what inet_aton would accept. */
static int pton4(const char *src, unsigned char *dst)
{
    unsigned char out[4];
    const char *p = src;
    for (int i = 0; i < 4; i++) {
        if (i > 0) { if (*p != '.') return 0; p++; }
        if (!isdigit((unsigned char)*p)) return 0;
        /* glibc's inet_pton rejects a leading zero followed by more digits
         * ("04", "01.2.3.4") -- a real octal/decimal ambiguity elsewhere in
         * the address-parsing world (see inet_aton below, which DOES accept
         * octal), so inet_pton is deliberately the strict one. "0" alone is
         * still a legal octet. */
        if (p[0] == '0' && isdigit((unsigned char)p[1])) return 0;
        int v = 0, n = 0;
        while (isdigit((unsigned char)*p)) {
            if (n == 3) return 0;
            v = v * 10 + (*p - '0');
            n++; p++;
        }
        if (v > 255) return 0;
        out[i] = (unsigned char)v;
    }
    if (*p != 0) return 0;
    memcpy(dst, out, 4);
    return 1;
}

static int pton6(const char *src, unsigned char *dst);   /* defined below, with the IPv6 code */

int inet_pton(int af, const char *src, void *dst)
{
    if (!src || !dst) { errno = EINVAL; return -1; }
    if (af == AF_INET)  return pton4(src, (unsigned char *)dst);
    if (af == AF_INET6) return pton6(src, (unsigned char *)dst);
    errno = EAFNOSUPPORT;
    return -1;
}

int inet_aton(const char *cp, struct in_addr *addr)
{
    /* Historical BSD form: 1-4 parts, each decimal/octal(0..)/hex(0x..), the
     * LAST part absorbs whatever bits remain (so "127.1" is 127.0.0.1). */
    if (!cp) return 0;
    unsigned long parts[4];
    int n = 0;
    const char *p = cp;
    for (;;) {
        if (!isdigit((unsigned char)*p)) return 0;
        unsigned long v = 0;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            if (!isxdigit((unsigned char)*p)) return 0;
            while (isxdigit((unsigned char)*p)) {
                v = v * 16 + (isdigit((unsigned char)*p) ? (unsigned)(*p - '0')
                                                           : (unsigned)(tolower((unsigned char)*p) - 'a' + 10));
                p++;
                if (v > 0xFFFFFFFFul) return 0;
            }
        } else if (p[0] == '0' && isdigit((unsigned char)p[1])) {
            p++;
            while (*p >= '0' && *p <= '7') { v = v * 8 + (unsigned)(*p - '0'); p++; if (v > 0xFFFFFFFFul) return 0; }
        } else {
            while (isdigit((unsigned char)*p)) { v = v * 10 + (unsigned)(*p - '0'); p++; if (v > 0xFFFFFFFFul) return 0; }
        }
        if (n == 4) return 0;
        parts[n++] = v;
        if (*p == '.') { p++; continue; }
        break;
    }
    if (*p != 0) return 0;
    unsigned long a;
    switch (n) {
    case 1: a = parts[0]; break;
    case 2: if (parts[0] > 0xff || parts[1] > 0xFFFFFFul) return 0; a = (parts[0] << 24) | parts[1]; break;
    case 3: if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xFFFFul) return 0;
            a = (parts[0] << 24) | (parts[1] << 16) | parts[2]; break;
    case 4: if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xff || parts[3] > 0xff) return 0;
            a = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]; break;
    default: return 0;
    }
    if (addr) addr->s_addr = htonl((uint32_t)a);
    return 1;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr a;
    if (!inet_aton(cp, &a)) return INADDR_NONE;
    return a.s_addr;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];
    unsigned char *b = (unsigned char *)&in.s_addr;
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static const char *ntop4(const void *src, char *dst, size_t size)
{
    const unsigned char *b = (const unsigned char *)src;
    char tmp[INET_ADDRSTRLEN];
    int n = snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if ((size_t)n >= size) { errno = ENOSPC; return NULL; }
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

/* ---- IPv6 ----------------------------------------------------------------
 * RFC 5952 canonical form on output: lowercase hex, no leading zeros in a
 * group, the FIRST-AND-LONGEST run of >=2 zero groups compressed to "::"
 * (a lone zero group is never compressed -- "1::1" not "1:0:0:0:0:0:0:1"
 * only applies to runs >= 2, and a single zero group prints as "0"). */
static const char *ntop6(const void *src, char *dst, size_t size)
{
    uint16_t g[8];
    const unsigned char *sb = (const unsigned char *)src;
    for (int i = 0; i < 8; i++) g[i] = (uint16_t)((sb[i * 2] << 8) | sb[i * 2 + 1]);

    /* find longest run of zero groups (>=2), first one wins on ties */
    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (cur_start < 0) { cur_start = i; cur_len = 1; } else cur_len++;
            if (cur_len > best_len) { best_len = cur_len; best_start = cur_start; }
        } else { cur_start = -1; cur_len = 0; }
    }
    if (best_len < 2) best_start = -1;

    /* IPv4-mapped/compatible: ::ffff:a.b.c.d or ::a.b.c.d, printed with the
     * trailing 32 bits as dotted-quad -- the form every implementation uses. */
    int v4tail = 0;
    if (best_start == 0 && (best_len == 6 || (best_len == 5 && g[5] == 0xffff))) {
        int allzero_before = 1;
        for (int i = 0; i < best_len; i++) if (g[i] != 0 && !(i == 5 && best_len == 5)) allzero_before = 0;
        if (allzero_before) v4tail = 1;
    }

    char tmp[INET6_ADDRSTRLEN];
    int n = 0;
    if (v4tail) {
        int mapped = (best_len == 5);
        n += snprintf(tmp + n, sizeof tmp - n, mapped ? "::ffff:" : "::");
        const unsigned char *b = (const unsigned char *)src;
        n += snprintf(tmp + n, sizeof tmp - n, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    } else {
        /* need_sep: does the NEXT token need a ':' printed before it? Only
         * true right after an ordinary hex group -- "::" always supplies its
         * own trailing colon, so the token after a compression must never
         * get a second one (that bug used to print "1:::1" for "1::1"). */
        int i = 0, need_sep = 0;
        while (i < 8) {
            if (i == best_start) {
                n += snprintf(tmp + n, sizeof tmp - n, "::");
                i += best_len;
                need_sep = 0;
                continue;
            }
            if (need_sep) n += snprintf(tmp + n, sizeof tmp - n, ":");
            n += snprintf(tmp + n, sizeof tmp - n, "%x", g[i]);
            need_sep = 1;
            i++;
        }
    }
    if ((size_t)n >= size) { errno = ENOSPC; return NULL; }
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

const char *inet_ntop(int af, const void *src, char *dst, size_t size)
{
    if (!src || !dst) { errno = EINVAL; return NULL; }
    if (af == AF_INET)  return ntop4(src, dst, size);
    if (af == AF_INET6) return ntop6(src, dst, size);
    errno = EAFNOSUPPORT;
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int pton6(const char *src, unsigned char *dst)
{
    uint16_t head[8]; int nhead = 0;
    uint16_t tail[8]; int ntail = 0;
    const char *p = src;
    int has_dcolon = 0;
    uint16_t *cur = head; int *ncur = &nhead;

    if (p[0] == ':') {
        if (p[1] != ':') return 0;
        has_dcolon = 1; p += 2;
        cur = tail; ncur = &ntail;
        if (*p == 0) { memset(dst, 0, 16); return 1; }   /* "::" */
    }

    while (*p) {
        /* embedded IPv4 tail: a run of digits/dots that pton4 accepts, only
         * legal as the LAST component. */
        const char *dotcheck = p;
        int has_dot = 0;
        while (*dotcheck && *dotcheck != ':') { if (*dotcheck == '.') has_dot = 1; dotcheck++; }
        if (has_dot) {
            unsigned char v4[4];
            char part[64];
            size_t len = (size_t)(dotcheck - p);
            if (len >= sizeof part) return 0;
            memcpy(part, p, len); part[len] = 0;
            if (!pton4(part, v4)) return 0;
            if (*ncur >= 7) return 0;
            cur[(*ncur)++] = (uint16_t)((v4[0] << 8) | v4[1]);
            cur[(*ncur)++] = (uint16_t)((v4[2] << 8) | v4[3]);
            p = dotcheck;
            if (*p != 0) return 0;   /* must be the last component */
            break;
        }
        int v = 0, n = 0;
        while (hexval(*p) >= 0) {
            if (n == 4) return 0;
            v = v * 16 + hexval(*p);
            p++; n++;
        }
        if (n == 0) return 0;
        if (*ncur >= 8) return 0;
        cur[(*ncur)++] = (uint16_t)v;
        if (*p == 0) break;
        if (*p != ':') return 0;
        p++;
        if (*p == ':') {
            if (has_dcolon) return 0;
            has_dcolon = 1;
            cur = tail; ncur = &ntail;
            p++;
            if (*p == 0) break;
        }
    }

    if (!has_dcolon) {
        if (nhead != 8) return 0;
        for (int i = 0; i < 8; i++) { dst[i * 2] = (unsigned char)(head[i] >> 8); dst[i * 2 + 1] = (unsigned char)head[i]; }
        return 1;
    }
    if (nhead + ntail > 8) return 0;
    uint16_t full[8] = {0};
    for (int i = 0; i < nhead; i++) full[i] = head[i];
    for (int i = 0; i < ntail; i++) full[8 - ntail + i] = tail[i];
    for (int i = 0; i < 8; i++) { dst[i * 2] = (unsigned char)(full[i] >> 8); dst[i * 2 + 1] = (unsigned char)full[i]; }
    return 1;
}
