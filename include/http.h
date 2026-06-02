#ifndef AQUA_HTTP_H
#define AQUA_HTTP_H

#include <stdint.h>

/* HTTP fetch status. */
#define HTTP_IDLE     0
#define HTTP_BUSY     1
#define HTTP_DONE     2
#define HTTP_ERR     -1     /* generic */
#define HTTP_ERR_URL -2
#define HTTP_ERR_DNS -3
#define HTTP_ERR_CONN -4
#define HTTP_ERR_TLS  -5

/* Start fetching `url` (absolute http://...). Non-blocking: the work advances in
 * http_poll() (pumped from net_poll). Returns 0 if accepted, <0 on a bad URL. */
int  http_get(const char *url);

/* Drive the in-flight fetch (called from net_poll). */
void http_poll(void);

/* Current status: HTTP_BUSY / HTTP_DONE / HTTP_ERR* . */
int  http_status(void);

/* After HTTP_DONE: the response body (HTML source) and its length. The pointer
 * is valid until the next http_get/res_fetch. */
const char *http_body(int *len);

/* Fetch a sub-resource (image) relative to the page loaded by the last
 * http_get; handles data: URIs. On success returns 0 with a kmalloc'd buffer the
 * caller must kfree. Used by the layout engine for <img>. */
int  res_fetch(const char *src, uint8_t **buf, int *len);

#endif /* AQUA_HTTP_H */
