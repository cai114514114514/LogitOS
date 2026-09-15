#ifndef LOGIT_TLS_DOMAIN_H
#define LOGIT_TLS_DOMAIN_H
#ifdef LOGIT_TLS_SINGLE_PROCESS
/* httpsd forks one single-threaded worker per connection. Its TLS pools are
 * private address-space copies, so kernel owner locks must not be linked or
 * executed in ring 3. This define is ONLY for that process model. */
struct io_domain { int unused; };
#define IO_DOMAIN_INIT {0}
#define IO_DOMAIN_GUARD(d) ((void)(d))
#else
#include "../../drivers/core/io_domain.h"
#endif
#endif
