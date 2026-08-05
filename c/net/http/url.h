#ifndef LOGIT_URL_H
#define LOGIT_URL_H

#include <stdint.h>

#define URL_HOST_MAX 128
#define URL_PATH_MAX 512

struct url {
    int      https;                 /* 1 if scheme is https (M12), else http */
    char     host[URL_HOST_MAX];
    uint16_t port;                  /* default 80 (http) / 443 (https) */
    char     path[URL_PATH_MAX];    /* always begins with '/' */
};

/* Parse "http://host[:port]/path". Returns 0 on success, -1 on a malformed or
 * unsupported URL. A missing path becomes "/". */
int url_parse(const char *s, struct url *out);

/* Resolve `ref` (which may be absolute "http://...", root-relative "/p", or
 * relative "p") against `base`, writing an absolute URL into out (max bytes).
 * Returns 0 on success. */
int url_resolve(const struct url *base, const char *ref, char *out, int max);

#endif /* LOGIT_URL_H */
