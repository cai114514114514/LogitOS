/* SPDX-License-Identifier: MIT */
#include "net_cli_host.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>

static const char *download_host_path(const char *,char [4096]);
static int eof, failed, reads;
int sys_write(int fd, const void *p, int n) { return (int)write(fd, p, (size_t)n); }
int sys_open(const char *p, int flags) { (void)flags;char path[4096];return open(download_host_path(p,path), O_RDONLY); }
int sys_close(int fd) { return close(fd); }
int sys_read(int fd, void *p, int n)
{
    if (getenv("NET_TEST_READ_FAIL") && reads++) return -1;
    if (n > 137) n = 137; /* file reads need not fill their requested buffer */
    return (int)read(fd, p, (size_t)n);
}
int write_file(const char *path, const void *p, int n)
{
    if (getenv("NET_TEST_WRITE_FAIL")) return -1;
    char mapped[4096];path=download_host_path(path,mapped);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return -1;
    int done = 0;
    while (done < n) {
        ssize_t got = write(fd, (const char *)p + done, (size_t)(n - done));
        if (got <= 0) { close(fd); return -1; }
        done += (int)got;
    }
    int rc = fsync(fd), cc = close(fd);
    return rc < 0 || cc < 0 ? -1 : n;
}
unsigned long long monotonic_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void sys_yield(void) { usleep(1000); }
int net_info(struct logit_netinfo *n) { (void)n; return -1; }
int net_ping(unsigned n) { (void)n; return -1; }
int net_ping_rtt(void) { return -1; }
int net_dns(const char *n) { (void)n; return -1; }
unsigned net_dns_result(void) { return ~0u; }
int sock_open(const char *host, int port, int flags)
{
    if (flags) return SOCK_E_TLS; /* TLS is measured by the real guest gate. */
    struct addrinfo hints = {0}, *ai = NULL;
    char service[8]; int k = 0; unsigned v = (unsigned)port; char d[5];
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < k; i++) service[i] = d[k-i-1]; service[k] = 0;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &ai)) return SOCK_E_DNS;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        if (fd >= 0) close(fd); freeaddrinfo(ai); return SOCK_E_CONN;
    }
    freeaddrinfo(ai); fcntl(fd, F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    eof = failed = 0; return fd;
}
int sock_poll(int fd)
{ (void)fd; return failed ? SOCK_P_ERROR : SOCK_P_CONNECTED | SOCK_P_WRITABLE | (eof ? SOCK_P_EOF : 0); }
int sock_send(int fd, const void *p, int n)
{
    if (n > 17) n = 17; /* exercise the production request writer's short-send loop */
    int rc = (int)send(fd, p, (size_t)n, 0);
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (rc < 0) failed = 1;
    return rc;
}
int sock_recv(int fd, void *p, int n)
{
    if (n > 137) n = 137;
    int rc = (int)recv(fd, p, (size_t)n, 0);
    if (rc == 0) { eof = 1; return -1; }
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (rc < 0) failed = 1;
    return rc;
}
int sock_close(int fd) { return close(fd); }

/* Translate only the product download directory into the test's private root. */
#include <sys/stat.h>
#include <string.h>
static const char *download_host_path(const char *p,char out[4096])
{
    const char *root=getenv("NET_TEST_DOWNLOAD_ROOT");
    if(root&&!strncmp(p,"/download",9)&&(p[9]=='/'||!p[9])){size_t n=strlen(root);if(n+strlen(p)>=4096)return "/nonexistent/net-test-path";memcpy(out,root,n);strcpy(out+n,p);return out;}return p;
}
int make_dir(const char *p){char b[4096];return mkdir(download_host_path(p,b),0700);}
int sys_rename(const char *a,const char *b){char x[4096],y[4096];a=download_host_path(a,x);b=download_host_path(b,y);if(link(a,b))return -1;return unlink(a);}
int delete_file(const char *p){char b[4096];p=download_host_path(p,b);return unlink(p)==0?0:rmdir(p);}
