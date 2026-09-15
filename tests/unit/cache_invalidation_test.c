/* Real browser_rt/cache/HTTP1/HTTP2 code; only the existing h2mux socket
 * fixture is substituted. GET populates the production cache over wire bytes,
 * POST changes server state, and a second GET must actually reach that server.
 * A cache-policy unit test calling invalidate itself cannot catch a missing
 * production consumer -- that was the original green-but-unwired defect. */
#define main h2mux_existing_main
#define hstub_send h2mux_original_send
#include "h2mux_test.c"
#undef hstub_send
#undef main
#include "http_cache.h"

static int version, get_requests, mutation_requests, reply_status = 200;

int hstub_send(int fd, const void *buf, int len)
{
    struct hsock *s = sk(fd);
    if (!s) return -1;
    if (s->h2) {
        /* Before bfetch used bxfer, warm GETs happened to use the stateful H1
         * peer below even on this H2 origin. Real ALPN now reaches the shared
         * peer's stateless 900-byte REPLY: that produced 35 false failures.
         * Let its real HPACK/frame decoder consume the request, but suppress
         * its stock response. GET gets the same version/cache policy as H1;
         * mutation headers/bodies remain explicitly released by each case. */
        s->h2s.hold_body = 1;
        for (int i=0; i<SRV_STREAMS; i++) s->h2s.hdr_sent[i] = 1;
        int rc = h2mux_original_send(fd, buf, len);
        for (int i=0; i<SRV_STREAMS; i++) {
            const char *method=hpack_list_get(&s->h2s.req[i], ":method");
            if (!s->h2s.req_seen[i] || s->h2s.done_sent[i] ||
                !method || strcmp(method,"GET")) continue;
            char body[32],cl[16];
            int n=snprintf(body,sizeof body,"version-%d",version);
            snprintf(cl,sizeof cl,"%d",n);
            const char *kv[]={":status","200","content-length",cl,
                "cache-control","max-age=120",NULL};
            srv_headers(&s->h2s,(uint32_t)(i*2+1),kv,0);
            srv_send(&s->h2s,H2_F_DATA,H2_FLAG_END_STREAM,
                (uint32_t)(i*2+1),body,n);
            s->h2s.done_sent[i]=1;
            get_requests++;
        }
        return rc;
    }
    pb_put(&s->c2s, buf, len);
    /* The fixture requests below have no entity, and the entire header must
     * arrive before this origin can update state or produce any reply. */
    const unsigned char *p = s->c2s.b + s->c2s.off;
    int n = pb_avail(&s->c2s), end = -1;
    for (int i = 0; i + 3 < n; i++)
        if (!memcmp(p + i, "\r\n\r\n", 4)) { end = i + 4; break; }
    if (end < 0) return len;
    int get = n >= 4 && !memcmp(p, "GET ", 4);
    int safe = get || (n >= 5 && !memcmp(p,"HEAD ",5)) ||
        (n >= 8 && !memcmp(p,"OPTIONS ",8)) || (n >= 6 && !memcmp(p,"TRACE ",6));
    int status = get ? 200 : reply_status;
    if (get) get_requests++;
    if (!safe) { mutation_requests++; if (status >= 200 && status < 400) version++; }
    s->c2s.off += end;
    if (s->c2s.off == s->c2s.len) s->c2s.off = s->c2s.len = 0;
    char body[32], head[256];
    int bl = snprintf(body,sizeof body,"version-%d",version);
    if (status == 204 || (n >= 5 && !memcmp(p,"HEAD ",5))) bl = 0;
    int hl = snprintf(head,sizeof head,
        "HTTP/1.1 %d Result\r\nContent-Length: %d\r\nCache-Control: max-age=120\r\n"
        "Location: https://foreign.example/target\r\n\r\n",status,bl);
    pb_put(&s->s2c,head,hl); pb_put(&s->s2c,body,bl);
    return len;
}

/* x_start comes from the shared fixture and its renamed sender deliberately
 * retains the original stateless server; redirect its HTTP/1 transport to our
 * stateful server before the first pump. This changes only the socket adapter. */
static int mutation_write(void *ctx, const void *buf, int len)
{ return hstub_send((int)(long)ctx,buf,len); }

static void fresh_world(void)
{
    reset_world(); bfetch_http_cache_clear(); bfetch_cache_clear();
    version = get_requests = mutation_requests = 0; reply_status = 200;
}

/* Return the server-side stream that carried this request.  The original
 * fixture used stream 1 unconditionally, which was accidentally true while
 * bxfer closed h2 after every request: each case got a new connection whose
 * first stream is 1.  Idle h2 reuse correctly leaves the warm GET on stream 1
 * and puts the following mutation on stream 3, so sending the synthetic
 * response back on 1 merely talks to an already-finished GET and reports a
 * cache failure that production never made.  Match what the fixture actually
 * decoded instead; method plus :path is the wire identity under test. */
static int request_stream(const struct hsock *s, const char *method, const char *path)
{
    if (!s || !s->h2) return -1;
    for (int i = 0; i < SRV_STREAMS; i++) {
        if (!s->h2s.req_seen[i]) continue;
        const char *m = hpack_list_get(&s->h2s.req[i], ":method");
        const char *p = hpack_list_get(&s->h2s.req[i], ":path");
        if (m && p && !strcmp(m, method) && !strcmp(p, path)) return i;
    }
    return -1;
}

static void get_version(const char *url, int expected)
{
    int id = bfetch_start_from(url,url);
    OK(id >= 0); if (id < 0) return;
    for (int k = 0; k < 100 && bfetch_state(id) == BF_PENDING; k++) bfetch_pump();
    OK(bfetch_state(id) == BF_DONE); OK(bfetch_status(id) == 200);
    int len = 0; const unsigned char *body = bfetch_body(id,&len);
    char wanted[32]; int wn=snprintf(wanted,sizeof wanted,"version-%d",expected);
    OKM(body && len == wn && !memcmp(body,wanted,(size_t)wn),
        "cache-invalidation: GET stale body for %s; expected %s",url,wanted);
    bfetch_release(id);
}

static void h1_case(const char *method, int status, int invalidates, const char *url,
                    const char *path, int port)
{
    fresh_world();
    get_version(url,0); get_version(url,0);
    OKM(get_requests == 1,"cache-invalidation: warm GET did not use real cache");
    /* A different origin must survive even when advertised as Location. */
    get_version("https://foreign.example/target",0);
    get_version("https://h1.example/target?other=1",0);
    get_version("https://h1.example:8444/target",0);
    reply_status = status;
    struct xfer x; struct xfer *v[] = {&x};
    OK(x_start(&x,method,path,"h1.example",port,0,0,0) == 0);
    x.c.t.write = mutation_write;
    x_spin(v,1,100); OK(x.c.state == H1_C_DONE);
    OKM(x.c.resp.code == status,"server status got=%d expected=%d",x.c.resp.code,status);
    OK(mutation_requests == (strcmp(method,"GET") && strcmp(method,"HEAD") &&
        strcmp(method,"OPTIONS") && strcmp(method,"TRACE")));
    x_free(&x);
    int before = get_requests;
    get_version(url,invalidates ? 1 : 0);
    OKM(get_requests == before + invalidates,
        "cache-invalidation: %s HTTP %d network GET count=%d expected=%d",
        method,status,get_requests,before+invalidates);
    get_version("https://foreign.example/target",0);
    get_version("https://h1.example/target?other=1",0);
    get_version("https://h1.example:8444/target",0);
    OK(get_requests == before + invalidates);
}

static void h2_headers_case(const char *method, int status, int invalidates)
{
    fresh_world();
    const char *url="https://h2.example/target";
    get_version(url,0); get_version(url,0); OK(get_requests == 1);
    /* Also retain another cookie variant: invalidation must not be limited
     * to the Cookie header of the mutating request. */
    OK(wacache_store(url,"session=other",(const unsigned char *)"variant",7,
        "max-age=120",0,0,0,0,0,0) == 0);
    struct xfer x;
    OK(x_start(&x,method,"/target","h2.example",443,0,0,1) == 0);
    struct hsock *s=sk(x.fd); OK(s && s->h2);
    if (!s || !s->h2) {x_free(&x);return;}
    s->h2s.hold_body=1;
    int si=-1;
    for(int k=0;k<20&&si<0;k++){bxfer_pump(&x.c);si=request_stream(s,method,"/target");}
    OK(si>=0);
    char code[8];snprintf(code,sizeof code,"%d",status);
    const char *kv[]={":status",code,"content-length","100","location","/redirect",NULL};
    if(si>=0)srv_headers(&s->h2s,(uint32_t)(si*2+1),kv,0);
    if (invalidates) version++;
    for(int k=0;k<20&&!h1_response_headers_done(&x.c.resp);k++)bxfer_pump(&x.c);
    OK(h1_response_headers_done(&x.c.resp)); OK(x.c.state != H1_C_DONE);
    /* Fetch follows redirects and cancels this body at the header boundary.
     * Waiting for DONE to invalidate would miss this real consumer sequence. */
    x_free(&x);
    unsigned char *body=NULL;int len=0;
    int hit=wacache_lookup(url,"session=other",&body,&len);
    OKM((hit==0)==!invalidates,"cache-invalidation: H2 %s %d cookie variant survived",method,status);
    free(body);
    get_version(url,invalidates ? 1 : 0);
    OKM(get_requests==1+invalidates,"cache-invalidation: H2 headers %s %d missing network GET",method,status);
}

static void h2_cancel_or_finish(int cancel_before_headers)
{
    fresh_world();
    const char *url="https://h2.example/target";
    get_version(url,0);
    struct xfer x;
    OK(x_start(&x,"POST","/target","h2.example",443,0,0,1)==0);
    struct hsock *s=sk(x.fd);OK(s&&s->h2);
    if(!s||!s->h2){x_free(&x);return;}
    s->h2s.hold_body=1;
    int si=-1;
    for(int k=0;k<20&&si<0;k++){bxfer_pump(&x.c);si=request_stream(s,"POST","/target");}
    OK(si>=0);
    if(cancel_before_headers) {
        x_free(&x);get_version(url,0);
        OKM(get_requests==1,"cache-invalidation: cancelled request evicted target without response");
        return;
    }
    const char *kv[]={":status","200","content-length","100",NULL};
    if(si>=0)srv_headers(&s->h2s,(uint32_t)(si*2+1),kv,0);version++;
    for(int k=0;k<20&&!h1_response_headers_done(&x.c.resp);k++)bxfer_pump(&x.c);
    OK(h1_response_headers_done(&x.c.resp));
    get_version(url,1);OK(get_requests==2);
    unsigned char bytes[100]={0};
    if(si>=0){
        srv_send(&s->h2s,H2_F_DATA,H2_FLAG_END_STREAM,(uint32_t)(si*2+1),bytes,sizeof bytes);
        s->h2s.done_sent[si]=1;
    }
    struct xfer *v[]={&x};x_spin(v,1,100);OK(x.c.state==H1_C_DONE);
    x_free(&x);get_version(url,1);
    OKM(get_requests==2,"cache-invalidation: body completion invalidated the newer GET a second time");
}

int main(void)
{
    h1_case("POST",200,1,"https://h1.example/target","/target",443);
    h1_case("PUT",204,1,"https://h1.example/target","/target",443);
    h1_case("DELETE",303,1,"https://h1.example/target","/target",443);
    h1_case("CUSTOM",200,1,"https://h1.example/target","/target",443);
    h1_case("POST",500,0,"https://h1.example/target","/target",443);
    h1_case("POST",404,0,"https://h1.example/target","/target",443);
    h1_case("GET",200,0,"https://h1.example/target","/target",443);
    h1_case("HEAD",200,0,"https://h1.example/target","/target",443);
    h1_case("OPTIONS",200,0,"https://h1.example/target","/target",443);
    h1_case("TRACE",200,0,"https://h1.example/target","/target",443);
    h1_case("POST",200,1,"https://H1.EXAMPLE:443/target?x=1","/target?x=1",443);
    h1_case("POST",200,1,"https://h1.example:8443/target","/target",8443);
    h2_headers_case("POST",200,1);
    h2_headers_case("POST",302,1);
    h2_headers_case("POST",307,1);
    h2_headers_case("PATCH",403,0);
    h2_headers_case("OPTIONS",200,0);
    h2_cancel_or_finish(1); h2_cancel_or_finish(0);
    fresh_world();
    printf("cache-invalidation: %d checks, %d failures\n",checks,fails);
    return fails ? 1 : 0;
}
