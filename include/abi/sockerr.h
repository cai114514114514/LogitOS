#ifndef LOGIT_ABI_SOCKERR_H
#define LOGIT_ABI_SOCKERR_H

/* One sentence per SOCK_E_* code, and exactly one copy of it.
 *
 * WHY IT IS HERE AND NOT IN AN APP.  The codes themselves are three lines up
 * the corridor, in logit_abi.h -- SOCK_E_ARG..SOCK_E_NOSLOT and the
 * SOCK_ERR_CODE() that unpacks them out of a sock_poll() result.  A number the
 * ABI defines needs exactly one English rendering, or two programs on the same
 * machine describe the same failure differently and the difference is not a
 * fact about the failure.  That had already started: c/apps/gui/ch.c carried a
 * private `sock_why`, while c/apps/browser/browser_rt.c carried a SECOND,
 * shorter decode inline in one printf ("dns start failed", "net not up",
 * "socket table full") and then threw the code away entirely at the poll site.
 *
 * WHY A SEPARATE HEADER AND NOT MORE OF logit_abi.h.  That file is shared with
 * the kernel and is the numbers; the kernel has no use for a string table and
 * should not link one.  This is the ring-3 presentation of those numbers, so it
 * is a header nothing includes by accident.
 *
 * WHY include/abi AND NOT c/apps.  c/apps is on the include path of every app
 * and of NO host test: tests/range.mk and tests/http2.mk compile the real
 * browser_rt.c on the host against tests/unit/h2stub/logit.h, and they leave
 * c/apps off the path on purpose (see the comment at tests/range.mk:22 -- with
 * it on, the real int-0x80 wrappers win and nothing links).  They do pass
 * -Iinclude/abi, because the stub itself includes logit_abi.h for the real
 * SOCK_F_ and SOCK_P_ constants.  A decoder the browser cannot include in the
 * build that tests the browser is a decoder that gets copied again.
 *
 * The strings are ch.c's, byte for byte, so adopting this header changes what
 * that window prints by nothing at all. */

#include "logit_abi.h"

/* `rc` is a negative SOCK_E_* -- either a sock_open() return, or
 * SOCK_ERR_CODE(bits) from a sock_poll() result carrying SOCK_P_ERROR.  A
 * SOCK_P_ERROR whose byte is zero means the reporter never encoded a code; it
 * lands on the last line, which says so rather than naming a cause. */
static inline const char *sock_why(int rc)
{
    switch (rc) {
    case SOCK_E_ARG:    return "bad argument";
    case SOCK_E_DNS:    return "the host name did not resolve";
    case SOCK_E_CONN:   return "the connection was refused or the network is down";
    case SOCK_E_TLS:    return "TLS refused: handshake or certificate verification failed";
    case SOCK_E_NOSLOT: return "the kernel socket table is full";
    }
    return "unknown socket error";
}

#endif /* LOGIT_ABI_SOCKERR_H */
