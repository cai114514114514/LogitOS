/* SPDX-License-Identifier: MIT */
#include "http1.h"
#include "url.h"
#include "../download_name.h"

#define NET_FETCH_MAX (64 * 1024 * 1024)
#define NET_SAVE_MAX (64 * 1024 * 1024)
#define NET_URL_MAX (URL_HOST_MAX + URL_PATH_MAX + 16)
#ifndef NET_FETCH_TIMEOUT_MS
#define NET_FETCH_TIMEOUT_MS 60000
#endif

struct net_body {
    struct net_digest digest;
    struct h1_response *response;
    uint8_t *saved;
    int len, cap, save;
    uint32_t fnv;
};
static int net_body_sink(void *ctx, const uint8_t *data, int len)
{
    struct net_body *b = ctx;
    /* http1's buffer cap intentionally excludes streaming sinks (SSE may
     * last forever). This bounded download consumer owns its transfer cap. */
    if (b->response->body_seen > b->response->body_max) return H1_E_TOOLARGE;
    if (b->response->code < 200 || b->response->code >= 300) return H1_OK;
    if (b->save) {
        if (len > NET_SAVE_MAX - b->len) return H1_E_TOOLARGE;
        if (b->len + len > b->cap) {
            int cap = b->cap ? b->cap : 16384;
            while (cap < b->len + len) cap *= 2;
            uint8_t *p = realloc(b->saved, (size_t)cap);
            if (!p) return H1_E_NOMEM;
            b->saved = p; b->cap = cap;
        }
        memcpy(b->saved + b->len, data, (size_t)len);
    }
    nd_update(&b->digest, data, (size_t)len);
    for (int i = 0; i < len; i++) { b->fnv ^= data[i]; b->fnv *= 16777619u; }
    b->len += len;
    return H1_OK;
}
static int net_read(void *ctx, void *buf, int max)
{
    int fd = *(int *)ctx, n = sock_recv(fd, buf, max);
    if (n >= 0) return n;
    int p = sock_poll(fd);
    return p >= 0 && !(p & SOCK_P_ERROR) && (p & SOCK_P_EOF) ? H1_EOF : H1_TERR;
}
static int net_write(void *ctx, const void *buf, int len)
{ int n = sock_send(*(int *)ctx, buf, len); return n < 0 ? H1_TERR : n; }

/* url.c's legacy parser silently shortens fields. Validate their full extent
 * before calling it: checksums must name the URL the user actually supplied.
 * Strip fragments and insert '/' before an authority-only query. IPv6 literal
 * URLs and userinfo are explicitly unsupported by this small parser. */
static int net_parse_url(const char *input, struct url *u)
{
    char text[NET_URL_MAX]; size_t n = strcspn(input, "#");
    if (n >= sizeof text - 1) return -1;
    memcpy(text, input, n); text[n] = 0;
    int prefix = !strncmp(text, "https://", 8) ? 8 : !strncmp(text, "http://", 7) ? 7 : 0;
    if (!prefix) return -1;
    char *host = text + prefix, *end = host;
    while (*end && *end != '/' && *end != '?') end++;
    char *port = NULL;
    for (char *p = host; p < end; p++) {
        if (*p == ':') { if (port) return -1; port = p; continue; }
        if (port) { if (*p < '0' || *p > '9') return -1; }
        else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return -1;
    }
    size_t hostlen = (size_t)((port ? port : end) - host);
    if (!hostlen || hostlen >= URL_HOST_MAX || (port && port + 1 == end)) return -1;
    for (char *p = end; *p; p++) if ((unsigned char)*p <= 32 || *p == 127 || *p == '\\') return -1;
    if (*end == '?') { memmove(end + 1, end, strlen(end) + 1); *end = '/'; }
    if (strlen(end) >= URL_PATH_MAX) return -1;
    return url_parse(text, u);
}
static void net_host_header(const struct url *u, char out[URL_HOST_MAX + 8])
{
    size_t n = strlen(u->host); memcpy(out, u->host, n);
    if (u->port != (u->https ? 443 : 80)) {
        char digits[5]; int i = 0, v = u->port; out[n++] = ':';
        do { digits[i++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (i) out[n++] = digits[--i];
    }
    out[n] = 0;
}
static int net_redirect(const struct url *base, const char *location, struct url *next)
{
    char absolute[NET_URL_MAX], path[URL_PATH_MAX];
    if (!location || strlen(location) >= URL_PATH_MAX) return -1;
    size_t scheme_end = strcspn(location, ":/?#");
    if (location[scheme_end] == ':' && strncmp(location, "http://", 7) &&
        strncmp(location, "https://", 8)) return -1;
    if (!*location || *location == '#') return -1;
    /* The legacy resolver treats '?v=2' as a filename. Preserve the current
     * path for this common signed-download redirect, excluding the old query. */
    if (*location == '?') {
        size_t n = strcspn(base->path, "?");
        if (n + strlen(location) >= sizeof path) return -1;
        memcpy(path, base->path, n); strcpy(path + n, location); location = path;
    } else if (*location != '/' && !strstr(location, "://")) {
        size_t n = strcspn(base->path, "?");
        while (n && base->path[n-1] != '/') n--;
        if (n + strlen(location) >= URL_PATH_MAX) return -1;
    }
    struct url clean = *base;
    clean.path[strcspn(clean.path, "?")] = 0;
    if (url_resolve(&clean, location, absolute, sizeof absolute) < 0 ||
        net_parse_url(absolute, next) < 0) return -1;
    /* A TLS URL does not grant permission to send its redirected download
     * over cleartext, even when an expected file digest was supplied. */
    return base->https && !next->https ? -1 : 0;
}
/* The staging mkdir and the final no-clobber rename are VFS operations, so
 * browser and CLI downloads cannot race into overwriting one another. */
static void net_download_path(char *out,const char *name,unsigned suffix,int extension)
{
    strcpy(out,LOGIT_DOWNLOAD_DIR);strcat(out,"/");int p=(int)strlen(out),n=(int)strlen(name),cut=n;
    const char *dot=extension?strrchr(name,'.'):NULL;if(dot&&dot!=name)cut=(int)(dot-name);
    memcpy(out+p,name,(size_t)cut);p+=cut;
    if(suffix){out[p++]=' ';out[p++]='(';char b[10];int k=0;do{b[k++]=(char)('0'+suffix%10);suffix/=10;}while(suffix);while(k)out[p++]=b[--k];out[p++]=')';}
    memcpy(out+p,name+cut,(size_t)(n-cut));p+=n-cut;out[p]=0;
}
static int do_get(const char *url, int alg, const uint8_t *expected, const char *save, int automatic)
{
    struct url u;
    if (net_parse_url(url, &u) < 0) { errs("net: invalid or unsupported URL\n"); return 1; }
    unsigned long long started = monotonic_ms();
    for (int hop = 0; hop < 6; hop++) {
        int fd = sock_open(u.host, u.port, u.https ? SOCK_F_TLS | SOCK_F_ALPN_HTTP11 : 0);
        if (fd < 0) { errs("net: socket open failed\n"); return 1; }
        int connected = 0;
        while (monotonic_ms() - started < NET_FETCH_TIMEOUT_MS) {
            int p = sock_poll(fd);
            if (p < 0 || (p & (SOCK_P_ERROR | SOCK_P_EOF))) break;
            if (p & SOCK_P_CONNECTED) { connected = 1; break; }
            sys_yield();
        }
        if (!connected) { sock_close(fd); errs("net: connection failed or timed out\n"); return 1; }
        struct h1_request q; char host[URL_HOST_MAX + 8], *request = NULL; int request_len;
        int rc = h1_request_init(&q, "GET", u.path);
        if (rc == H1_OK) {
            net_host_header(&u, host);
            if (h1_request_set_header(&q, "Host", host) < 0 ||
                h1_request_set_header(&q, "User-Agent", "LogitOS-net/1.0") < 0 ||
                h1_request_set_header(&q, "Accept-Encoding", "identity") < 0 ||
                h1_request_set_header(&q, "Connection", "close") < 0) rc = H1_E_NOMEM;
            if (rc == H1_OK) rc = h1_request_build(&q, &request, &request_len);
            h1_request_free(&q);
        }
        if (rc < 0) { sock_close(fd); errs("net: request construction failed\n"); return 1; }
        struct h1_transport t = { net_read, net_write, NULL, &fd };
        struct h1_conn conn;
        h1_conn_start(&conn, &t, request, request_len);
        struct net_body body = {0}; body.response = &conn.resp;
        body.save = save != NULL || automatic; body.fnv = 2166136261u; nd_init(&body.digest, alg);
        h1_response_limit(&conn.resp, body.save ? NET_SAVE_MAX : NET_FETCH_MAX);
        h1_response_sink(&conn.resp, net_body_sink, &body);
        int done = 0;
        while (monotonic_ms() - started < NET_FETCH_TIMEOUT_MS) {
            uint64_t before = h1_conn_progress(&conn);
            int state = h1_conn_pump(&conn);
            if (h1_response_headers_done(&conn.resp) && conn.resp.clen > conn.resp.body_max) {
                conn.err = H1_E_TOOLARGE; break;
            }
            if (h1_response_headers_done(&conn.resp) && !h1_response_streaming(&conn.resp)) {
                conn.err = H1_E_ENCODING; break;
            }
            if (state == H1_C_DONE) { done = 1; break; }
            if (state == H1_C_ERROR) break;
            if (h1_conn_progress(&conn) == before) sys_yield();
        }
        sock_close(fd);
        if (!done) {
            errs("net: "); errs(conn.err ? h1_strerror(conn.err) : "fetch timed out"); errs("\n");
            h1_conn_free(&conn); free(body.saved); return 1;
        }
        if (h1_is_redirect(conn.resp.code)) {
            struct url next;
            rc = net_redirect(&u, h1_headers_get(&conn.resp.hdr, "location"), &next);
            h1_conn_free(&conn); free(body.saved);
            if (rc < 0) { errs("net: refused redirect\n"); return 1; }
            u = next; continue;
        }
        int code = conn.resp.code;
        char download_name[48];
        if(automatic)dl_filename(u.path,h1_headers_get(&conn.resp.hdr,"content-disposition"),NULL,download_name,sizeof download_name);
        h1_conn_free(&conn);
        if (code < 200 || code >= 300 || code == 206) {
            errs("net: HTTP status "); outn_fd(2, code); errs("\n"); free(body.saved); return 1;
        }
        uint8_t digest[64]; nd_final(&body.digest, digest);
        if (expected && !nd_equal(digest, expected, nd_size(alg))) {
            free(body.saved); errs("net: checksum mismatch\n"); return 2;
        }
        if (save || automatic) {
            /* The whole-file VFS write reports its barrier result. Do not open
             * or truncate the destination until framing, status AND digest
             * pass. There is no atomic replace contract here: a storage error
             * is reported, not represented as a successful download. */
            char target[128],staging[128],directory[128];int staged=0;
            if(automatic){
                make_dir(LOGIT_DOWNLOAD_DIR);
                for(unsigned i=1;i<10000;i++){
                    net_download_path(directory,".net-pending",i,0);
                    if(make_dir(directory)>=0){strcpy(staging,directory);strcat(staging,"/body");staged=1;break;}
                }
                if(!staged){free(body.saved);errs("net: cannot reserve download destination\n");return 1;}
                save=staging;
            }
            int written = write_file(save, body.saved ? body.saved : (const uint8_t *)"", body.len);
            free(body.saved);
            if (written != body.len) { if(staged){delete_file(staging);delete_file(directory);}errs("net: destination write failed\n"); return 1; }
            uint8_t check[64];
            if (nd_file(alg, save, check) < 0 || !nd_equal(check, expected ? expected : digest, nd_size(alg))) {
                if(staged){delete_file(staging);delete_file(directory);}errs("net: destination readback failed\n"); return 1;
            }
            if(staged){
                int committed=0;
                for(unsigned i=0;i<10000;i++){
                    net_download_path(target,download_name,i?i+1:0,1);
                    if(sys_rename(staging,target)>=0){committed=1;break;}
                    int existing=sys_open(target,0);if(existing<0)break;sys_close(existing);
                }
                delete_file(staging);delete_file(directory);
                if(!committed){errs("net: download commit failed\n");return 1;}
                save=target;
            }
            outs("saved "); outs(save); outc('\n');
        }
        outs("http bytes "); outn(body.len); outs(" fnv1a "); outn(body.fnv); outc('\n');
        outs(nd_names[alg]); outc(' '); nd_print(alg, digest); outc('\n');
        if (expected) outs("checksum verified\n");
        return 0;
    }
    errs("net: too many redirects\n"); return 1;
}
