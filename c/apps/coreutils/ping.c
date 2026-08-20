/* /bin/ping -- ICMP echo over a real SOCK_RAW socket.
 *
 * usage: ping [-c count] <host-or-ip>
 *
 * WHY THIS EXISTS ALONGSIDE SYS_NET_PING. The old path (c/net/ip/icmp.c's
 * icmp_ping()/icmp_last_rtt(), reached from userland through SYS_NET_PING /
 * SYS_NET_PING_RTT) is a bespoke syscall pair that only the Network GUI app
 * uses -- there was no way for an ORDINARY program to send an ICMP echo, the
 * way it can open a TCP connection or a UDP socket. This is that: an
 * ordinary program, using the same socket() / sendto() / recvfrom() / close()
 * shape httpd.c already uses for TCP (c/net/core/lsock.c, SYS_SOCKET..
 * SYS_SOCKSTAT), over a new socket kind -- LOGIT_SOCK_RAW -- rather than a
 * sixth bespoke syscall.
 *
 * WHAT IT DOES NOT DO FOR YOU. A raw ICMP socket delivers a copy of EVERY
 * inbound ICMP message this machine receives -- someone else's ping reply,
 * an echo REQUEST icmp_input is about to auto-answer, and so on (see
 * c/net/core/raw.h) -- not just replies to what this process sent. This
 * program filters by source address + id + sequence itself, which is not a
 * missing feature: every ping(8) on every OS with raw sockets does exactly
 * this, because that is what a raw socket is.
 *
 * WHAT IT NEEDS TO RUN AT ALL: a root credential. socket(AF_INET, SOCK_RAW,
 * IPPROTO_ICMP) refuses anyone else with LSK_E_PERM -- see
 * c/net/core/lsock.c's lsock_create() for why a raw socket needs that check
 * and c/fs/vfs_cred.c for what "root" means on this machine today. */

#include "clib.h"

#define PING_TIMEOUT_MS  2000
#define PING_INTERVAL_MS 1000
#define PING_PAYLOAD     32     /* matches icmp.c's own icmp_ping() */

struct icmp_echo {
    unsigned char  type, code;
    unsigned short checksum;
    unsigned short id, seq;
} __attribute__((packed));

static unsigned short bswap16(unsigned short x)
{ return (unsigned short)((x << 8) | (x >> 8)); }

/* The same 16-bit ones'-complement algorithm as c/net/ip/ip.c's
 * ip_checksum() -- duplicated rather than shared because this program links
 * only clib.h (the inline-syscall header every coreutils program uses), not
 * the kernel's network TU; see CLAUDE.md's "Source layout" note that CLI
 * programs use logit.h inline syscalls rather than a shared implementation.
 * Reads each pair of bytes as a big-endian 16-bit word regardless of host
 * endianness, exactly like the original, so the result needs the same
 * bswap16() before it goes on the wire that ip_checksum()'s callers apply. */
static unsigned short icmp_checksum(const void *data, int len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((unsigned)p[i] << 8) | p[i + 1];
    if (len & 1) sum += (unsigned)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

static void ip_print(unsigned ip)        /* host order: a.b.c.d */
{
    outn((ip >> 24) & 0xFF); outc('.');
    outn((ip >> 16) & 0xFF); outc('.');
    outn((ip >> 8)  & 0xFF); outc('.');
    outn( ip        & 0xFF);
}

/* "a.b.c.d" -> host-order uint32. Returns 0 (not 1) for anything else,
 * including a truncated or over-long address -- a hostname goes through
 * resolve()'s DNS path instead. */
static int parse_ipv4(const char *s, unsigned *out)
{
    unsigned v = 0, byte = 0, digits = 0, n = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            byte = byte * 10 + (unsigned)(*p - '0');
            digits++;
            if (byte > 255 || digits > 3) return 0;
        } else if (*p == '.' || *p == 0) {
            if (digits == 0) return 0;
            v = (v << 8) | byte;
            byte = 0; digits = 0; n++;
            if (*p == 0) break;
        } else {
            return 0;
        }
    }
    if (n != 4) return 0;
    *out = v;
    return 1;
}

/* Dotted quad or hostname -> host-order IP. Same poll+yield DNS loop
 * do_dns() in c/apps/coreutils/net.c uses, for the reason that file gives:
 * the first query is dropped while the resolver's ARP entry is cold, so a
 * miss (0xFFFFFFFF) re-sends at once instead of waiting out the second. */
static int resolve(const char *host, unsigned *out)
{
    if (parse_ipv4(host, out)) return 1;
    net_dns(host);
    unsigned long long t0 = monotonic_ns();
    int last_sec = -1;
    for (;;) {
        unsigned r = net_dns_result();
        if (r && r != 0xFFFFFFFFu) { *out = r; return 1; }
        int sec = (int)((monotonic_ns() - t0) / 1000000000ull);
        if (sec != last_sec || r == 0xFFFFFFFFu) { net_dns(host); last_sec = sec; }
        if (sec >= 8) return 0;
        sys_yield();
    }
}

int main(int argc, char **argv)
{
    int count = 4, argi = 1;
    if (argc > 3 && c_streq(argv[1], "-c")) { count = c_atoi(argv[2]); argi = 3; }
    if (argi >= argc) { errs("usage: ping [-c count] <host-or-ip>\n"); return 1; }
    if (count < 1) count = 1;
    const char *host = argv[argi];

    unsigned dst;
    if (!resolve(host, &dst)) {
        errs("ping: cannot resolve "); errs(host); errs("\n");
        return 1;
    }
    outs("PING "); outs(host); outs(" ("); ip_print(dst); outs(")\n");

    int fd = sys_socket(LOGIT_AF_INET, LOGIT_SOCK_RAW, LOGIT_IPPROTO_ICMP);
    if (fd < 0) {
        errs("ping: socket: ");
        if (fd == LSK_E_PERM)      errs("permission denied (raw sockets are root-only)\n");
        else if (fd == LSK_E_NET)  errs("no network interface\n");
        else if (fd == LSK_E_FULL) errs("socket table full\n");
        else                       { outn_fd(2, fd); errs("\n"); }
        return 1;
    }
    /* Non-blocking + our own poll/timeout loop, not SO_RCVTIMEO's blocking
     * park: matches do_ping()/do_dns() in net.c, which need the WM thread to
     * keep running net_poll() (the RX path) between polls -- see logit.h's
     * long note above SYS_SOCKET for why a blocking read still needs
     * something pumping the network on this machine. */
    sys_set_nonblock(fd);

    unsigned short my_id = (unsigned short)(sys_getpid() & 0xFFFF);
    int sent = 0, received = 0;
    long long rtt_total_us = 0;
    int rtt_min_us = -1, rtt_max_us = -1;

    for (int seq = 1; seq <= count; seq++) {
        struct { struct icmp_echo h; unsigned char pad[PING_PAYLOAD]; } msg;
        msg.h.type = 8;       /* ICMP_ECHO_REQUEST */
        msg.h.code = 0;
        msg.h.checksum = 0;
        msg.h.id  = bswap16(my_id);
        msg.h.seq = bswap16((unsigned short)seq);
        for (int i = 0; i < PING_PAYLOAD; i++) msg.pad[i] = (unsigned char)i;
        msg.h.checksum = bswap16(icmp_checksum(&msg, (int)sizeof msg));

        struct logit_dgram d;
        d.buf = (unsigned char *)&msg; d.len = (int)sizeof msg; d.flags = 0;
        d.family = LOGIT_AF_INET; d.port = 0; d.addr = dst;
        int rc = sys_sendto(fd, &d);
        if (rc != (int)sizeof msg) {
            errs("ping: sendto rc="); outn_fd(2, rc); errs("\n");
            if (seq < count) sys_sleep_ms(PING_INTERVAL_MS);
            continue;
        }
        sent++;

        unsigned long long t_send = monotonic_ns();
        int got = 0;
        for (;;) {
            unsigned long long now = monotonic_ns();
            if ((now - t_send) / 1000000ull > PING_TIMEOUT_MS) break;

            unsigned char buf[128];
            struct logit_dgram rd;
            rd.buf = buf; rd.len = (int)sizeof buf; rd.flags = 0;
            rd.family = 0; rd.port = 0; rd.addr = 0;
            int n = sys_recvfrom(fd, &rd);
            if (n >= (int)sizeof(struct icmp_echo)) {
                struct icmp_echo *rh = (struct icmp_echo *)buf;
                if (rh->type == 0 && rh->code == 0 &&           /* ICMP_ECHO_REPLY */
                    rh->id == bswap16(my_id) &&
                    rh->seq == bswap16((unsigned short)seq) &&
                    rd.addr == dst) {
                    unsigned long long rtt_us = (now - t_send) / 1000ull;
                    outn(n); outs(" bytes from "); ip_print(rd.addr);
                    outs(": icmp_seq="); outn(seq);
                    outs(" time="); outn((long)(rtt_us / 1000));
                    outc('.'); outn((long)((rtt_us % 1000) / 100));
                    outs(" ms\n");
                    received++;
                    rtt_total_us += (long long)rtt_us;
                    if (rtt_min_us < 0 || (int)rtt_us < rtt_min_us) rtt_min_us = (int)rtt_us;
                    if ((int)rtt_us > rtt_max_us) rtt_max_us = (int)rtt_us;
                    got = 1;
                    break;
                }
                /* Not the reply to THIS request -- another process's ping,
                 * or an echo request icmp_input is about to auto-answer.
                 * Keep waiting; it does not count against our timeout
                 * budget any differently than an empty poll would. */
            }
            sys_yield();
        }
        if (!got) { outs("Request timeout for icmp_seq="); outn(seq); outc('\n'); }
        if (seq < count) sys_sleep_ms(PING_INTERVAL_MS);
    }

    sys_close(fd);

    outc('\n');
    outs("--- "); outs(host); outs(" ping statistics ---\n");
    outn(sent); outs(" packets transmitted, "); outn(received); outs(" received, ");
    outn(sent ? (long)((sent - received) * 100 / sent) : 0); outs("% packet loss\n");
    if (received > 0) {
        outs("rtt min/avg/max = "); outn(rtt_min_us / 1000); outc('/');
        outn((long)(rtt_total_us / received / 1000)); outc('/');
        outn(rtt_max_us / 1000); outs(" ms\n");
    }
    return received > 0 ? 0 : 1;
}
