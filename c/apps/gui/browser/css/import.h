#ifndef LOGIT_CSS_IMPORT_H
#define LOGIT_CSS_IMPORT_H
/* A stylesheet dependency expander, independent of the network and DOM. The
 * caller supplies ordinary resource fetching and URL resolution; returned bytes
 * are malloc-owned. final_url must name the response after redirects. */
#define CSS_IMPORT_URL_MAX 1024
#define CSS_IMPORT_DEPTH 8
#define CSS_IMPORT_REQUESTS 64
#define CSS_IMPORT_BYTES (4 * 1024 * 1024)
struct css_import_budget {
    int requests, bytes, loaded, failed, cycles, limited, unsupported;
    int rewrite_delta; /* output - source bytes, excluding imported bodies */
};
struct css_import_io {
    int (*resolve)(void *ctx, const char *base, const char *ref, char *out, int cap);
    int (*fetch)(void *ctx, const char *url, unsigned char **data, int *len,
                 char *final_url, int cap);
    void *ctx;
};
/* Append one sheet, expanding imports at their original positions and rebasing
 * url() resources against that sheet's response URL. Returns the new length;
 * on insufficient output capacity returns -1 and restores the original suffix.
 * Limits are per page via the shared budget, not reset for every linked sheet. */
int css_import_expand(const char *src, int len, const char *base,
                      char *out, int used, int cap,
                      struct css_import_budget *budget,
                      const struct css_import_io *io);
/* Same atomic sheet append with a malloc-owned, geometrically grown output.
 * A single pass avoids downloading @imports again merely to measure output.
 * On refusal the old prefix survives, even if realloc moved the buffer. */
int css_text_reserve(char **out, int *cap, int needed, int limit);
int css_import_expand_alloc(const char *src,int len,const char *base,
                            char **out,int used,int *cap,int limit,
                            struct css_import_budget *budget,const struct css_import_io *io);
#endif
