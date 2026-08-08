/* The fixture-site controls of tests/unit/loader_fakebfetch.c. */
#ifndef LOADER_FAKEBFETCH_H
#define LOADER_FAKEBFETCH_H

void  fake_site_reset(void);
void  fake_site_add(const char *url, const char *body);   /* body must outlive the test */
int   fake_site_requests(void);
const char *fake_site_request(int i);
/* How many requested URLs contain `frag` -- the "was it fetched at all" channel. */
int   fake_site_fetched(const char *frag);

#endif /* LOADER_FAKEBFETCH_H */
