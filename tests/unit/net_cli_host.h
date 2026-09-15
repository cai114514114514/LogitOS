/* SPDX-License-Identifier: MIT */
/* Replace only the syscall door. The CLI helpers, HTTP parser, digest and
 * command dispatch are the shipped sources. Unsupported host services fail. */
#ifndef NET_CLI_HOST_H
#define NET_CLI_HOST_H
#define LOGIT_USERLIB_H
#include "logit_abi.h"
int sys_write(int, const void *, int);
int sys_read(int, void *, int);
int sys_open(const char *, int);
int sys_close(int);
int write_file(const char *, const void *, int);
void sys_yield(void);
unsigned long long monotonic_ms(void);
int net_info(struct logit_netinfo *);
int net_ping(unsigned);
int net_ping_rtt(void);
int net_dns(const char *);
unsigned net_dns_result(void);
int sock_open(const char *, int, int);
int sock_poll(int);
int sock_send(int, const void *, int);
int sock_recv(int, void *, int);
int sock_close(int);
int make_dir(const char *);
int sys_rename(const char *,const char *);
int delete_file(const char *);
#endif
