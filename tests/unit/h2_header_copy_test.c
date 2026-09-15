/* SPDX-License-Identifier: MIT
 * Reuse the H2 socket/server harness while compiling the actual browser
 * adapter. A successful HPACK decode is not proof that its subsequent H1
 * metadata allocation succeeded. Inject failure at THAT copy, after ordinary
 * headers have copied, and observe the public response and bfetch boundaries.
 * No host networking, third-party request, or real profile is involved. */
#define main h2mux_existing_main
#include "h2mux_test.c"
#undef main

static const char *fail_field;
static int injected;
int h2_header_copy_add(struct h1_headers *h,const char *name,int nlen,
                       const char *value,int vlen)
{
    if(nlen<0)nlen=(int)strlen(name);
    if(fail_field && (int)strlen(fail_field)==nlen && !memcmp(name,fail_field,(size_t)nlen)) {
        injected++;
        return H1_E_NOMEM;
    }
    return h1_headers_add(h,name,nlen,value,vlen);
}

static void check(int ok,const char *name)
{ checks++;if(!ok){printf("FAIL: %s\n",name);fails++;} }

static void fixture_headers(struct hsock *s,int stream)
{
    const char *kv[]={":status","200","content-type","text/html",
        "content-security-policy","script-src 'none'",
        "content-security-policy","frame-ancestors 'none'",
        "x-frame-options","DENY","content-length","16",0};
    static const unsigned char body[]="synthetic-body!!";
    uint32_t sid=(uint32_t)(stream*2+1);
    srv_headers(&s->h2s,sid,kv,0);
    srv_send(&s->h2s,H2_F_DATA,H2_FLAG_END_STREAM,sid,body,16);
    s->h2s.done_sent[stream]=1;
}

static void stream_case(int broken)
{
    reset_world();fail_field=0;injected=0;
    struct xfer x;
    check(x_start(&x,"GET","/headers","h2.example",443,0,0,1)==0,
          "stream fixture starts a real H2 exchange");
    struct hsock *s=sk(x.fd);
    check(s&&s->h2,"stream fixture negotiated H2");
    if(!s||!s->h2){x_free(&x);return;}
    s->h2s.hdr_sent[0]=1;s->h2s.hold_body=1;
    for(int k=0;k<20&&!s->h2s.req_seen[0];k++)bxfer_pump(&x.c);
    check(s->h2s.req_seen[0],"stream request reached the independent server");
    fixture_headers(s,0);
    if(broken)fail_field="content-security-policy";
    for(int k=0;k<40&&x.c.state!=H1_C_DONE&&x.c.state!=H1_C_ERROR;k++)bxfer_pump(&x.c);
    if(broken){
        check(injected>0,"stream allocation failure was reached");
        check(x.c.state==H1_C_ERROR&&x.c.err==H1_E_NOMEM,
              "stream header-copy failure reports allocation error");
        check(!h1_response_headers_done(&x.c.resp),
              "stream failed copy never declares complete headers");
        check(x.sunk_len==0&&x.c.resp.body_len==0,
              "stream failed copy delivers no body");
        for(int k=0;k<3;k++)bxfer_pump(&x.c);
        check(x.c.state==H1_C_ERROR&&!h1_response_headers_done(&x.c.resp)&&x.sunk_len==0,
              "stream failure stays failed on later pumps");
    }else{
        check(injected==0&&x.c.state==H1_C_DONE,"healthy stream finishes without injected failure");
        check(h1_headers_count(&x.c.resp.hdr,"content-security-policy")==2,
              "healthy stream retains both policy fields");
        check(h1_response_headers_done(&x.c.resp)&&x.sunk_len==16,
              "healthy stream delivers complete headers and exact body length");
    }
    fail_field=0;x_free(&x);
}

static int redirect(void *owner,const char *base,const char *ref,char *out,int cap)
{(void)owner;return bfetch_resolve(base,ref,out,cap)==0;}

static void loader_case(int broken)
{
    reset_world();fail_field=0;injected=0;
    int id=bfetch_start_embedded("https://h2.example/headers","https://h2.example/parent",
        "https://h2.example/parent",redirect,0);
    check(id>=0,"embedded fixture starts through the real bfetch door");
    struct hsock *s=0;
    for(int i=0;i<NSOCK;i++)if(g_sk[i].used&&!g_sk[i].closed&&g_sk[i].h2){s=&g_sk[i];break;}
    check(s!=0,"embedded fixture uses the H2 socket");
    if(id<0||!s){if(id>=0)bfetch_release(id);return;}
    /* Native GET retries once even for allocation failures. Keep both
     * attempts under the same fault so success on a fresh response cannot
     * masquerade as ignoring the first response's incomplete metadata. */
    s->h2s.hdr_sent[0]=s->h2s.hdr_sent[1]=1;s->h2s.hold_body=1;
    for(int k=0;k<20&&!s->h2s.req_seen[0];k++)bfetch_pump();
    check(s->h2s.req_seen[0],"embedded request reached the independent server");
    fixture_headers(s,0);
    if(broken)fail_field="x-frame-options";
    for(int k=0;k<40&&bfetch_state(id)==BF_PENDING;k++){
        bfetch_pump();
        if(s->h2s.req_seen[1]&&!s->h2s.done_sent[1])fixture_headers(s,1);
    }
    int len=-1;const unsigned char *body=bfetch_body(id,&len);
    if(broken){
        check(injected>=1&&injected<=2,"embedded late policy-copy failure is bounded to two attempts");
        check(bfetch_state(id)==BF_FAILED,"embedded failed copy reports fetch failure");
        check(!bfetch_response_policy_known(id),"embedded failed copy never marks policies known");
        check(!bfetch_response_header(id,"content-security-policy"),
              "embedded failed copy exposes no partial policy response");
        check(!body&&len==0,"embedded failed copy exposes no body");
        check(strstr(bfetch_error(id),"memory")!=0,
              "embedded failed copy retains the allocation failure reason");
    }else{
        check(injected==0&&bfetch_state(id)==BF_DONE,"healthy embedded response completes");
        check(bfetch_response_policy_known(id),"healthy embedded policy metadata is known");
        const char *csp=bfetch_response_header(id,"content-security-policy");
        check(csp&&!strcmp(csp,"script-src 'none'\nframe-ancestors 'none'"),
              "healthy embedded response aggregates all CSP fields");
        check(body&&len==16,"healthy embedded response exposes exact body length");
    }
    fail_field=0;bfetch_release(id);
}

int main(void)
{
    stream_case(0);stream_case(1);loader_case(0);loader_case(1);
    reset_world();
    printf("h2-header-copy: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
