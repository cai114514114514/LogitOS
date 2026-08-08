/* The fixture-site controls of tests/unit/loader_fakebfetch.c. */
#ifndef LOADER_FAKEBFETCH_H
#define LOADER_FAKEBFETCH_H

void  fake_site_reset(void);
void  fake_site_add(const char *url, const char *body);   /* body must outlive the test */
int   fake_site_requests(void);
const char *fake_site_request(int i);
/* How many requested URLs contain `frag` -- the "was it fetched at all" channel. */
int   fake_site_fetched(const char *frag);
/* Forget the request log WITHOUT forgetting the routes. This is what makes
 * "switching to this tab cost zero requests" assertable: load a page, clear the
 * log, switch away and back, and the log is the answer. */
void  fake_site_clear_log(void);
/* Connections dialled since the last fake_site_reset()/clear_log(). The fake
 * has no pool, so every request is a dial -- which is the pessimistic direction
 * and therefore the right one for an assertion that says "none". */
int   fake_site_dials(void);

#endif /* LOADER_FAKEBFETCH_H */
