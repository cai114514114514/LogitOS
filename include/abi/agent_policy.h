/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_AGENT_POLICY_H
#define LOGIT_AGENT_POLICY_H
#include "logit_abi.h"
/* Opt-in worker policy: new syscall numbers are denied until reviewed. The
 * legacy CAP_* classifier intentionally permits clipboard/settings/process
 * calls and cannot express this boundary. Workers acquire objects via fd 3. */
static inline int aex_agent_syscall_allowed(uint64_t n, uint64_t fd)
{
    switch (n) {
    case SYS_AGENT_PEER: case SYS_READ: case SYS_WRITE: case SYS_CLOSE: case SYS_SETNB:
        return fd == AEX_AGENT_FD;
    case SYS_EXIT: case SYS_YIELD: case SYS_GETPID: case SYS_CPU_COUNT:
    case SYS_MONOTONIC_MS:
    case SYS_MMAP: case SYS_MUNMAP: case SYS_MPROTECT: case SYS_POLL:
    case SYS_THREAD_SELF: case SYS_SET_TLS: case SYS_FUTEX:
    case SYS_AGENT_SELF: case SYS_RUSAGE:
        return 1;
    default: return 0;
    }
}
#endif
