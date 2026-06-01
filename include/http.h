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

/* Start fetching `url` (absolute http://...). Non-blocking: the work advances in
 * http_poll() (pumped from net_poll). Returns 0 if accepted, <0 on a bad URL. */
int  http_get(const char *url);

/* Drive the in-flight fetch (called from net_poll). */
void http_poll(void);

/* Current status: HTTP_BUSY / HTTP_DONE / HTTP_ERR* . */
int  http_status(void);

/* After HTTP_DONE: copy up to max bytes of the rendered page text starting at
 * byte offset `off`; returns bytes copied (0 at end). */
int  http_read(int off, char *buf, int max);

/* After HTTP_DONE: number of links found, and the absolute URL of link i
 * (returns 0 on success into buf, -1 if i is out of range). */
int  http_link_count(void);
int  http_link_url(int i, char *buf, int max);

#endif /* AQUA_HTTP_H */
