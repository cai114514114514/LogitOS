/* SPDX-License-Identifier: MIT
 * Offline regression fixtures for the 2026-09-10 Cookie audit. These exercise
 * the shared product jar directly; browser context/transport has its own gate.
 * Failure groups let a negative-control build prove exactly which boundary
 * it disabled rather than passing because some unrelated process crashed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cookies.h"
#define NOW ((int64_t)1700000000)
static int checks, fails;
#define CHECK(group, expr) do { checks++; if (!(expr)) { \
    printf("FAIL %s:%d %s\n", group, __LINE__, #expr); fails++; } } while (0)
static struct cookie_ctx context(const char *host, const char *path, int secure, int http)
{ struct cookie_ctx c = {host,path,secure,http}; return c; }
static int put(struct cookie_jar *j, const char *host, const char *path, int tls, const char *line)
{ struct cookie_ctx c = context(host,path,tls,1); return cookie_set(j,&c,line,NOW); }

static void secure_integrity(void)
{
    struct cookie_jar j; cookie_jar_init(&j);
    CHECK("secure", put(&j,"unit.example","/login",1,"sid=old; Secure; Path=/login") == 0);
    CHECK("secure", put(&j,"unit.example","/login",0,"sid=new; Path=/login") == -1);
    CHECK("secure", put(&j,"unit.example","/login",0,"sid=; Path=/login; Max-Age=0") == -1);
    CHECK("secure", j.n == 1 && j.v[0].secure && !strcmp(j.v[0].value,"old"));
    CHECK("secure", put(&j,"sub.unit.example","/login/en",0,"sid=x; Path=/login/en") == -1);
    CHECK("secure", put(&j,"unit.example","/login",0,"sid=x; Domain=unit.example; Path=/login") == -1);
    /* The path relation is deliberately asymmetric: broader and sibling paths
     * are permitted; rejecting both directions would break ordinary sites. */
    CHECK("secure", put(&j,"unit.example","/",0,"sid=wide; Path=/") == 0);
    CHECK("secure", put(&j,"unit.example","/foo",0,"sid=side; Path=/foo") == 0);
    CHECK("secure", put(&j,"other.example","/login",0,"sid=other; Path=/login") == 0);
    CHECK("secure", put(&j,"unit.example","/login",1,"sid=updated; Secure; Path=/login") == 0);
    CHECK("secure", put(&j,"unit.example","/login",1,"sid=; Secure; Path=/login; Max-Age=0") == 0);
    cookie_jar_free(&j); cookie_jar_init(&j);
    CHECK("secure", put(&j,"unit.example","/",1,"expired=old; Secure; Max-Age=1") == 0);
    struct cookie_ctx c = context("unit.example","/",0,1);
    CHECK("secure", cookie_set(&j,&c,"expired=new",NOW+2) == 0);
    cookie_jar_free(&j);
}

static void request_context(void)
{
    struct cookie_ctx c = context("api.unit.example","/",1,1);
    struct cookie_request r = {"www.unit.example",1,0,1,0};
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_SAME_SITE);
    r.site_secure = 0;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE);
    r.top_level_navigation=1;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE_NAV);
    r.safe_method=0;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE_NAV_UNSAFE);
    r.site_host="other.example"; r.site_secure=1;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE_NAV_UNSAFE);
    r.site_host=NULL; r.browser_initiated=1;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_SAME_SITE);
    r.top_level_navigation=0;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE);
    r.browser_initiated=0; r.top_level_navigation=1; r.safe_method=1;
    CHECK("context", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE_NAV);
    CHECK("context", cookie_request_kind(&c,NULL) == CK_REQ_CROSS_SITE);
    r.site_host="a.github.io"; r.top_level_navigation=0; c.host="b.github.io";
    CHECK("psl", cookie_request_kind(&c,&r) == CK_REQ_CROSS_SITE);
}

static void creation_context(void)
{
    const char *line[] = {"u=1", "l=1; SameSite=Lax", "s=1; SameSite=Strict", "n=1; SameSite=None; Secure"};
    struct cookie_ctx c = context("unit.example","/",1,1);
    for (int kind=0; kind<=3; kind++) {
        for (int http=0; http<=1; http++) {
            for (int attr=0; attr<4; attr++) {
                struct cookie_jar j; cookie_jar_init(&j); c.http_api=http;
                int allowed = attr==3 || kind==CK_REQ_SAME_SITE ||
                              (http && (kind==CK_REQ_CROSS_SITE_NAV || kind==CK_REQ_CROSS_SITE_NAV_UNSAFE));
                int result=cookie_set_ex(&j,&c,kind,line[attr],NOW);
                CHECK("creation", result == (allowed ? 0 : -1));
                cookie_jar_free(&j);
            }
        }
    }
    struct cookie_jar j; cookie_jar_init(&j); c.http_api=1;
    CHECK("creation", cookie_set_ex(&j,&c,CK_REQ_SAME_SITE,"strict=old; SameSite=Strict",NOW)==0);
    CHECK("creation", cookie_set_ex(&j,&c,CK_REQ_CROSS_SITE,"strict=; SameSite=Strict; Max-Age=0",NOW)==-1);
    CHECK("creation", j.n==1);
    CHECK("creation", cookie_set_ex(&j,&c,CK_REQ_CROSS_SITE_NAV_UNSAFE,"lax=1; SameSite=Lax",NOW)==0);
    char buf[64];
    CHECK("creation", cookie_header_ex(&j,&c,CK_REQ_CROSS_SITE_NAV_UNSAFE,NOW,buf,sizeof buf)==0);
    cookie_jar_free(&j);
}

static void suffix_rules(void)
{
    CHECK("psl", cookie_domain_is_public_suffix("github.io"));
    CHECK("psl", cookie_domain_is_public_suffix("blogspot.com"));
    CHECK("psl", cookie_domain_is_public_suffix("s3.amazonaws.com"));
    CHECK("psl", cookie_domain_is_public_suffix("k12.ak.us"));
    CHECK("psl", cookie_domain_is_public_suffix("unit.ck"));
    CHECK("psl", !cookie_domain_is_public_suffix("www.ck"));
    CHECK("psl", cookie_same_site("a.www.ck","b.www.ck"));
    CHECK("psl", !cookie_same_site("a.unit.ck","b.unit.ck"));
    CHECK("psl", cookie_domain_is_public_suffix("unit.kawasaki.jp"));
    CHECK("psl", !cookie_domain_is_public_suffix("city.kawasaki.jp"));
    CHECK("psl", cookie_same_site("a.city.kawasaki.jp","b.city.kawasaki.jp"));
    CHECK("psl", !cookie_domain_is_public_suffix("org.de"));
    CHECK("psl", !cookie_same_site("a.github.io","b.github.io"));
    CHECK("psl", cookie_same_site("A.UNIT.EXAMPLE.","b.unit.example"));
    CHECK("psl", cookie_domain_is_public_suffix("xn--55qx5d.cn"));
    struct cookie_jar j; cookie_jar_init(&j);
    CHECK("psl", put(&j,"a.github.io","/",1,"p=1; Domain=github.io")==-1);
    CHECK("psl", put(&j,"a.unit.ck","/",1,"p=1; Domain=unit.ck")==-1);
    CHECK("psl", put(&j,"a.city.kawasaki.jp","/",1,"p=1; Domain=city.kawasaki.jp")==0);
    CHECK("psl", put(&j,"a.org.de","/",1,"p=1; Domain=org.de")==0);
    cookie_jar_free(&j);
}

static void prefix_parsing(void)
{
    struct cookie_jar j; cookie_jar_init(&j);
    CHECK("prefix", put(&j,"unit.example","/",1,"__Secure-=x")==-1);
    CHECK("prefix", put(&j,"unit.example","/",1,"__Secure-=x; Secure")==0);
    CHECK("prefix", put(&j,"unit.example","/",1,"__Host-=x; Secure")==-1);
    CHECK("prefix", put(&j,"unit.example","/",1,"__Host-=x; Secure; Path=/")==0);
    CHECK("prefix", put(&j,"unit.example","/",1,"__HOST-name=x; Secure")==-1);
    CHECK("prefix", put(&j,"unit.example","/",1,"__Host-name=x; Secure; Path=/; Path=relative")==-1);
    CHECK("prefix", put(&j,"unit.example","/",1,"__Host-name=x; Secure; Path=/; Domain=unit.example")==-1);
    CHECK("prefix", put(&j,"localhost","/",1,"__Host-name=x; Secure; Path=/; Domain=localhost")==-1);
    cookie_jar_free(&j);
}

static void attributes_expiry(void)
{
    struct cookie_jar j; cookie_jar_init(&j);
    CHECK("parsing", put(&j,"unit.example","/a/b",1,"p=1; Path=/x; Path=relative")==0);
    CHECK("parsing", !strcmp(j.v[0].path,"/a"));
    CHECK("parsing", put(&j,"unit.example","/",1,"s=1; SameSite=Strict; SameSite=invalid")==0);
    CHECK("parsing", j.v[1].samesite==CK_SS_UNSET);
    CHECK("parsing", put(&j,"unit.example","/",1,"plus=1; Max-Age=+20")==0);
    CHECK("parsing", !j.v[2].persistent);
    CHECK("parsing", put(&j,"unit.example","/",1,"max=1; Max-Age=999999999999999999999999999")==0);
    CHECK("parsing", j.v[3].expires==NOW+CK_MAX_AGE_SECONDS);
    CHECK("parsing", put(&j,"unit.example","/",1,"date=1; Expires=Fri, 31 Dec 9999 23:59:59 GMT")==0);
    CHECK("parsing", j.v[4].expires==NOW+CK_MAX_AGE_SECONDS);
    CHECK("parsing", put(&j,"unit.example","/",1,"dot=1; Domain=unit.example.")==-1);
    CHECK("parsing", put(&j,"unit.example","/",1,"ctl=1; Path=/ok\r\n")==-1);
    char buf[CK_PAIR_MAX+1024+64];
    memset(buf,'a',sizeof buf); buf[200]='='; buf[CK_PAIR_MAX+1]=0;
    CHECK("parsing", put(&j,"unit.example","/",1,buf)==0);
    buf[CK_PAIR_MAX+1]='a'; buf[CK_PAIR_MAX+2]=0;
    CHECK("parsing", put(&j,"unit.example","/",1,buf)==-1);
    strcpy(buf,"path=1; Path=/");
    size_t pos=strlen(buf); memset(buf+pos,'x',1023); buf[pos+1023]=0;
    CHECK("parsing", put(&j,"unit.example","/",1,buf)==0);
    CHECK("parsing", strlen(j.v[j.n-1].path)==1024);
    strcat(buf,"x");
    CHECK("parsing", put(&j,"unit.example","/a/b",1,buf)==0);
    CHECK("parsing", !strcmp(j.v[j.n-1].path,"/a"));
    cookie_jar_free(&j);
}

static void header_overflow(void)
{
    struct cookie_jar j; cookie_jar_init(&j);
    CHECK("overflow", put(&j,"unit.example","/",0,"a=1234")==0);
    CHECK("overflow", put(&j,"unit.example","/",0,"b=5678")==0);
    struct cookie_ctx c=context("unit.example","/",0,1);
    struct cookie_header_diagnostics d;
    char small[8], full[15];
    CHECK("overflow", cookie_header_with_diagnostics(&j,&c,0,NOW+1,small,sizeof small,&d)==CK_E_NOFIT);
    CHECK("overflow", small[0]==0 && d.required_bytes==15 && d.eligible_count==2);
    CHECK("overflow", j.v[0].accessed==NOW && j.v[1].accessed==NOW);
    CHECK("overflow", cookie_header_with_diagnostics(&j,&c,0,NOW+2,full,sizeof full,&d)==14);
    CHECK("overflow", !strcmp(full,"a=1234; b=5678") && j.v[0].accessed==NOW+2);
    CHECK("overflow", cookie_header_with_diagnostics(&j,&c,0,NOW,full,14,&d)==CK_E_NOFIT);
    CHECK("overflow", full[0]==0 && d.required_bytes==15);
    cookie_jar_free(&j); cookie_jar_init(&j);
    char *line=malloc(4100); memset(line,'x',4097); line[1]='='; line[4097]=0;
    for (int i=0;i<3;i++) { line[0]=(char)('a'+i); CHECK("overflow", put(&j,"unit.example","/",0,line)==0); }
    char *out=malloc(CK_HEADER_MAX);
    CHECK("overflow", cookie_header_with_diagnostics(&j,&c,0,NOW,out,CK_HEADER_MAX,&d)==CK_E_NOFIT);
    CHECK("overflow", !out[0] && d.required_bytes==12296 && d.eligible_count==3);
    free(out);free(line);cookie_jar_free(&j);
}

/* Capacity cannot be another way for an ordinary new cookie to evict the
 * protected state that a same-name overwrite was correctly refused to touch. */
static void eviction_admission(void)
{
    struct cookie_jar j; cookie_jar_init(&j);cookie_jar_limits(&j,2,2);
    CHECK("eviction", put(&j,"unit.example","/",1,"a=1; Secure; HttpOnly")==0);
    CHECK("eviction", put(&j,"unit.example","/",1,"b=1; Secure")==0);
    struct cookie_ctx c=context("unit.example","/",0,0);
    CHECK("eviction", cookie_set(&j,&c,"ordinary=1",NOW+1)==0);
    CHECK("eviction", j.n==2 && j.v[0].secure && j.v[1].secure);
    c.secure=1;
    CHECK("eviction", cookie_set(&j,&c,"replacement=1; Secure",NOW+2)==0);
    CHECK("eviction", j.n==2 && !strcmp(j.v[0].name,"a"));
    cookie_jar_free(&j);cookie_jar_init(&j);cookie_jar_limits(&j,1,1);
    CHECK("eviction", put(&j,"unit.example","/",1,"a=1; HttpOnly")==0);
    CHECK("eviction", cookie_set(&j,&c,"ordinary=1",NOW+1)==0);
    CHECK("eviction", j.n==1 && j.v[0].http_only);
    cookie_jar_free(&j);
}

static void restore_validation(void)
{
    struct cookie_jar j; cookie_jar_init(&j);
    struct cookie c = {"sid","value","unit.example","/",NOW+60,NOW-10,NOW-5,1,1,1,1,CK_SS_LAX};
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==0);
    CHECK("restore", j.n==1 && j.v[0].name!=c.name && j.v[0].created==NOW-10);
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.name="second";c.expires=NOW;
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.expires=NOW+60;c.host_only=0;c.domain="github.io";
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.domain="unit.example";c.samesite=CK_SS_NONE;c.secure=0;
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.samesite=CK_SS_LAX;c.name="__Host-";c.secure=1;
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.host_only=1;c.path="/sub";
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.path="/";c.domain="UNIT.example";
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.domain="unit.example";c.name="second";c.persistent=0;
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.persistent=1;c.secure=2;
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    c.secure=1;c.accessed=NOW+1;
    CHECK("restore", cookie_restore_entry(&j,&c,NOW)==-1);
    CHECK("restore", j.n==1);
    cookie_jar_free(&j);
}

int main(void)
{
    secure_integrity();request_context();creation_context();suffix_rules();
    prefix_parsing();attributes_expiry();header_overflow();eviction_admission();restore_validation();
    printf("cookie hardening: %d checks, %d failures\n",checks,fails);
    return fails ? 1 : 0;
}
