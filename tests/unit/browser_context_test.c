/* SPDX-License-Identifier: MIT
 * Exercise the real fetch/XHR, Cookie and CORS implementation through the
 * ordinary fake HTTP transport. No live account or external server is used. */
#include "stream_net.h"

static void router(struct fakesock *s, const char *method, const char *target)
{
    char origin[300] = "null", head[1200];
    const char *p = strstr(s->req, "\r\nOrigin: ");
    if (p) { p += 10; const char *e = strstr(p, "\r\n");
        int n = e ? (int)(e-p) : 0;
        if (n > 0 && n < (int)sizeof origin) { memcpy(origin,p,n); origin[n]=0; } }
    if (!strcmp(target,"/r1") || !strcmp(target,"/r2")) {
        snprintf(head,sizeof head,"HTTP/1.1 302 Found\r\nLocation: https://%s/final\r\n"
                 "Access-Control-Allow-Origin: %s\r\nContent-Length: 0\r\n\r\n",
                 !strcmp(target,"/r1")?"other.example":"third.example",origin);
        rsp_add(s,head); return;
    }
    snprintf(head,sizeof head,"HTTP/1.1 %s\r\nAccess-Control-Allow-Origin: %s\r\n"
             "Access-Control-Allow-Credentials: true\r\n"
             "Access-Control-Allow-Methods: PUT\r\nAccess-Control-Allow-Headers: x-context\r\n"
             "Access-Control-Max-Age: 600\r\n%sContent-Length: %d\r\n\r\n%s",
             !strcmp(method,"OPTIONS")?"204 No Content":"200 OK",origin,
             !strcmp(target,"/set")?"Set-Cookie: keep=1; Secure; SameSite=None; Path=/\r\n"
             "Set-Cookie: refuse=1; Secure; SameSite=Strict; Path=/\r\n":"",
             !strcmp(method,"OPTIONS")?0:2,!strcmp(method,"OPTIONS")?"":"ok");
    rsp_add(s,head);
}
static int options(void)
{ for(int i=0;i<req_count();i++)if(!strncmp(nth_req(i),"OPTIONS ",8))return 1;return 0; }
static void put(const char *path)
{
    char js[500];
    snprintf(js,sizeof js,"var done=false; fetch('https://service.example/%s',"
             "{method:'PUT',headers:{'X-Context':'fixture'},body:'fixture'})"
             ".then(function(r){return r.text()}).then(function(){done=true});",path);
    run(js);settle(100);ckjs("done","preflighted request completes");
}
int main(void)
{
    fs_set_router(router);open_ctx("https://page.example/start");
    run("document.cookie='strict=1; Secure; SameSite=Strict; Path=/'");
    fs_reset();run("fetch('http://page.example/final',{credentials:'include'}).catch(function(){});");
    settle(100);ck(!strstr(nth_req(0),"Cookie:"),"scheme change suppresses same-site cookie");

    fs_reset();run("var setdone=false;fetch('https://other.example/set',{credentials:'include'})"
                  ".then(function(){setdone=true});");settle(100);
    ckjs("setdone","cross-site response remains available with valid CORS");
    close_ctx();open_ctx("https://other.example/");
    ckjs("document.cookie.indexOf('keep=1')>=0","cross-site SameSite None response cookie stored");
    ckjs("document.cookie.indexOf('refuse=1')<0","cross-site Strict response cookie refused");
    close_ctx();open_ctx("https://page.example/start");

    fs_reset();run("var rdone=false;fetch('/r1',{headers:{Authorization:'fixture'}})"
                  ".then(function(r){return r.text()}).then(function(){rdone=true});");settle(100);
    ckjs("rdone","same-origin redirect accepts original-origin CORS reply");
    ck(strstr(nth_req(1),"Origin: https://page.example\r\n")!=0,"first redirect retains initiating origin");
    ck(!strstr(nth_req(1),"Authorization:"),"cross-origin redirect drops author authorization");
    fs_reset();run("var rdone2=false;fetch('https://other.example/r2')"
                  ".then(function(r){return r.text()}).then(function(){rdone2=true});");settle(100);
    ckjs("rdone2","tainted redirect accepts ACAO null");
    ck(strstr(nth_req(1),"Origin: null\r\n")!=0,"second-origin redirect serializes opaque origin");

    fs_reset();put("pf");ck(options(),"first request preflights");
    fs_reset();put("pf");ck(!options(),"same creator and URL reuse successful preflight");
    fs_reset();put("pf-other");ck(options(),"different URL requires fresh preflight");
    close_ctx();open_ctx("https://second.example/");
    fs_reset();put("pf");ck(options(),"different creator requires fresh preflight");

    fs_reset();run("var pendingDone=false;fetch('/final').then(function(){pendingDone=true});");
    JSContext *other=JS_NewContext(rt);
    js_webapi_install(other,"https://replacement.example/");
    JSValue global=JS_GetGlobalObject(other), f=JS_GetPropertyStr(other,global,"fetch");
    ck(JS_IsUndefined(f),"unsupported second realm cannot overwrite active Web APIs");
    JS_FreeValue(other,f);JS_FreeValue(other,global);
    js_webapi_close(other);JS_FreeContext(other);
    settle(100);ckjs("pendingDone","closing an unrelated realm preserves pending owner request");
    ckjs("location.hostname==='second.example'","owner location remains bound to original realm");
    run("for(var i=0;i<3;i++)document.cookie='big'+i+'='+'x'.repeat(3000)+'; Secure; Path=/';"
        "var readFailed=false;try{document.cookie}catch(e){readFailed=true}");
    ckjs("readFailed","document cookie overflow is an explicit failure");
    fs_reset();run("var oversizedFailed=false;fetch('/final').catch(function(){oversizedFailed=true});");
    settle(100);ckjs("oversizedFailed","fetch refuses an incomplete cookie header");
    ck(req_count()==0,"cookie overflow sends no request bytes");
    close_ctx();printf("browser_context: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
