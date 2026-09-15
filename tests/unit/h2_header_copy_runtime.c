/* SPDX-License-Identifier: MIT
 * Redirect only the real adapter's header-allocation call into the fixture.
 * HTTP/1 parsing, request building, HPACK and all other allocations stay real. */
#define h1_headers_add h2_header_copy_add
#include "../../c/apps/browser/browser_rt.c"
