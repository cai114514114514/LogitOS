/* Passive document policy, not a substitute for a scripted frame sandbox.
 * CSP3 source lists are evaluated per response policy: independent policies
 * intersect, duplicate directives use the first occurrence, and unrelated
 * script-src directives cannot disable an otherwise permitted image embed.
 * URLs use the shipping URL parser, never suffix checks on unparsed URLs.
 * This initial consumer refuses CSP sandbox, unsupported URL schemes, and
 * hash-only inline styles (no source text hash is supplied to this API).
 * Redirects must be checked by the loader at EACH destination, before fetch.
 * Sources: W3C CSP3 sections 6.4.2 and 6.7.2, HTML X-Frame-Options processing.
 */
#include "iframe_policy.h"
#ifndef URL_CORE_ONLY
#define URL_CORE_ONLY
#define IP_UNDEF_URL_CORE_ONLY
#endif
#include "js_url.h"
#ifdef IP_UNDEF_URL_CORE_ONLY
#undef URL_CORE_ONLY
#undef IP_UNDEF_URL_CORE_ONLY
#endif
#include <stdlib.h>
#include <string.h>

#define IP_MAX 8192
struct ips { const char *p; size_t n; int found; };
struct ipurl { char *scheme, *host, *port, *path, *origin; };
static int ip_space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f'; }
static int ip_lc(int c) { return c>='A' && c<='Z' ? c+32 : c; }
static int ip_eq(struct ips s,const char *v)
{
    if(s.n!=strlen(v))return 0;
    for(size_t i=0;i<s.n;i++)if(ip_lc((unsigned char)s.p[i])!=ip_lc((unsigned char)v[i]))return 0;
    return 1;
}
static struct ips ip_trim(struct ips s)
{ while(s.n && ip_space(*s.p)){s.p++;s.n--;}while(s.n && ip_space(s.p[s.n-1]))s.n--;return s; }
static struct ips ip_directive(struct ips policy,const char *name)
{
    size_t i=0;
    while(i<policy.n){size_t b=i;while(i<policy.n && policy.p[i]!=';')i++;
        struct ips d=ip_trim((struct ips){policy.p+b,i-b,1});
        size_t k=0;while(k<d.n && !ip_space(d.p[k]))k++;
        if(ip_eq((struct ips){d.p,k,1},name))return ip_trim((struct ips){d.p+k,d.n-k,1});
        if(i<policy.n)i++;
    }
    return (struct ips){0,0,0};
}
static struct ips ip_select(struct ips p,const char *first,const char *second,const char *third)
{
    struct ips r=ip_directive(p,first);
    if(!r.found && second)r=ip_directive(p,second);
    if(!r.found && third)r=ip_directive(p,third);
    return r;
}
static int ip_next_policy(const char *all,size_t *at,struct ips *out)
{
    if(!all || !all[*at])return 0;
    size_t b=*at;while(all[*at] && all[*at]!='\n' && all[*at]!=',')(*at)++;
    *out=ip_trim((struct ips){all+b,*at-b,1});if(all[*at])(*at)++;return 1;
}
static int ip_bounded(const char *s) { return !s || strlen(s)<=IP_MAX; }
static void ip_url_free(struct ipurl *u)
{ free(u->scheme);free(u->host);free(u->port);free(u->path);free(u->origin);memset(u,0,sizeof *u); }
static int ip_url(const char *s,struct ipurl *out)
{
    memset(out,0,sizeof *out);if(!s || strlen(s)>IP_MAX)return 0;
    urlrec *u=url_parse_w(s,-1,0);if(!u)return 0;
    out->scheme=url_get(u,URLC_PROTOCOL);out->host=url_get(u,URLC_HOSTNAME);
    out->port=url_get(u,URLC_PORT);out->path=url_get(u,URLC_PATHNAME);out->origin=url_get(u,URLC_ORIGIN);
    char *user=url_get(u,URLC_USERNAME),*pass=url_get(u,URLC_PASSWORD);
    int ok=out->scheme && out->host && out->port && out->path && out->origin && user && pass &&
        !user[0] && !pass[0] && out->host[0] &&
        (!strcmp(out->scheme,"http:") || !strcmp(out->scheme,"https:"));
    free(user);free(pass);url_free_w(u);if(!ok)ip_url_free(out);return ok;
}
static const char *ip_port(const struct ipurl *u)
{ return u->port[0]?u->port:(!strcmp(u->scheme,"https:")?"443":"80"); }
static int ip_scheme(const char *source,const char *target)
{ return !strcmp(source,target) || (!strcmp(source,"http:") && !strcmp(target,"https:")); }
static int ip_hex(int c)
{ if(c>='0'&&c<='9')return c-'0';c=ip_lc(c);return c>='a'&&c<='f'?c-'a'+10:-1; }
/* Decode a segment at a time. Decoding an entire path would turn encoded '/'
 * into a path separator and accidentally grant a different directory. */
static int ip_segment(const char *a,size_t an,const char *b,size_t bn)
{
    size_t i=0,j=0;
    while(i<an && j<bn){unsigned ca=(unsigned char)a[i++],cb=(unsigned char)b[j++];
        if(ca=='%' && i+1<an && ip_hex(a[i])>=0 && ip_hex(a[i+1])>=0){ca=ip_hex(a[i])*16+ip_hex(a[i+1]);i+=2;}
        if(cb=='%' && j+1<bn && ip_hex(b[j])>=0 && ip_hex(b[j+1])>=0){cb=ip_hex(b[j])*16+ip_hex(b[j+1]);j+=2;}
        if(ca!=cb)return 0;
    }return i==an && j==bn;
}
static int ip_path(const char *source,const char *target)
{
    if(!source[0])return 1;
    size_t a=0,b=0,n=strlen(source),m=strlen(target);int prefix=source[n-1]=='/';
    while(a<n){size_t ae=a,be=b;while(ae<n && source[ae]!='/')ae++;while(be<m && target[be]!='/')be++;
        if(!ip_segment(source+a,ae-a,target+b,be-b))return 0;
        if(ae==n)return be==m;
        if(be==m)return 0;
        a=ae+1;b=be+1;if(a==n && prefix)return 1;
    }return b==m;
}
static int ip_match(struct ips token,const struct ipurl *self,const struct ipurl *dest)
{
    if(ip_eq(token,"'self'")){
        if(strcmp(self->host,dest->host) || !ip_scheme(self->scheme,dest->scheme))return 0;
        return !strcmp(ip_port(self),ip_port(dest)) ||
            (!strcmp(self->scheme,"http:") && !strcmp(dest->scheme,"https:") &&
             !strcmp(ip_port(self),"80") && !strcmp(ip_port(dest),"443"));
    }
    if(ip_eq(token,"*"))return 1; /* ip_url already limits network schemes */
    if(!token.n || token.n>=2048 || token.p[0]=='\'')return 0;
    char s[2048];memcpy(s,token.p,token.n);s[token.n]=0;
    if(s[token.n-1]==':' && !strchr(s,'/')){
        for(size_t i=0;i<token.n;i++)s[i]=(char)ip_lc((unsigned char)s[i]);
        return ip_scheme(s,dest->scheme);
    }
    char scheme[8];strcpy(scheme,self->scheme);char *host=s,*mark=strstr(s,"://");
    if(mark){size_t n=(size_t)(mark-s);if(n>5)return 0;
        for(size_t i=0;i<n;i++)scheme[i]=(char)ip_lc((unsigned char)s[i]);scheme[n]=':';scheme[n+1]=0;host=mark+3;}
    if(!ip_scheme(scheme,dest->scheme))return 0;
    char *path=strchr(host,'/');if(path)*path++=0;
    /* Paths need the leading slash we split away; source expressions with
     * queries/fragments/userinfo or IPv6 literals are conservatively refused. */
    if(strchr(host,'@') || strchr(host,'[') || strchr(host,'?') || strchr(host,'#'))return 0;
    char *port=strchr(host,':');if(port)*port++=0;
    for(char *p=host;*p;p++)*p=(char)ip_lc((unsigned char)*p);
    int hmatch=0;
    if(!strcmp(host,"*"))hmatch=1;
    else if(host[0]=='*' && host[1]=='.'){
        size_t dn=strlen(dest->host),sn=strlen(host+1);
        hmatch=dn>sn && !strcmp(dest->host+dn-sn,host+1);
    }else hmatch=!strcmp(host,dest->host);
    if(!hmatch)return 0;
    if(port && strcmp(port,"*")){
        for(char *p=port;*p;p++)if(*p<'0'||*p>'9')return 0;
        if(strcmp(port,ip_port(dest)) && !( !strcmp(port,"80") && !strcmp(scheme,"http:") &&
            !strcmp(dest->scheme,"https:") && !strcmp(ip_port(dest),"443")))return 0;
    }else if(!port && dest->port[0])return 0;
    if(path){char full[2048];size_t n=strlen(path);if(n+2>sizeof full || strchr(path,'?') || strchr(path,'#'))return 0;
        full[0]='/';memcpy(full+1,path,n+1);if(!ip_path(full,dest->path))return 0;}
    return 1;
}
static int ip_list(struct ips list,const struct ipurl *self,const struct ipurl *dest)
{
    if(!list.found)return 1;
    size_t i=0;while(i<list.n){while(i<list.n && ip_space(list.p[i]))i++;size_t b=i;
        while(i<list.n && !ip_space(list.p[i]))i++;
        if(i>b && ip_match((struct ips){list.p+b,i-b,1},self,dest))return 1;
    }return 0;
}
static int ip_policies(const char *all,const struct ipurl *self,const struct ipurl *dest,
                       const char *a,const char *b,const char *c,int *found)
{
    if(!ip_bounded(all))return 0;
    size_t at=0;struct ips p;
    while(ip_next_policy(all,&at,&p)){
        if(ip_directive(p,"sandbox").found)return 0;
        struct ips l=ip_select(p,a,b,c);if(l.found && found)*found=1;
        if(!ip_list(l,self,dest))return 0;
    }return 1;
}
int iframe_policy_frame(const char *parent_url,const char *child_url,const char *parent_csp,
                        const char *child_csp,const char *xfo)
{
    struct ipurl p,c;if(!ip_url(parent_url,&p))return 0;
    if(!ip_url(child_url,&c)){ip_url_free(&p);return 0;}
    /* frame-ancestors matches the serialized ancestor origin, not the path of
     * its current document. A path in an ancestor source must never confer
     * authority on a particular document within an otherwise denied origin. */
    struct ipurl ancestor=p;ancestor.path="/";
    int ancestors=0,ok=ip_policies(parent_csp,&p,&c,"frame-src","child-src","default-src",0) &&
        ip_policies(child_csp,&c,&ancestor,"frame-ancestors",0,0,&ancestors);
    if(!strcmp(p.scheme,"https:") && !strcmp(c.scheme,"http:"))ok=0;
    /* Enforced frame-ancestors supersedes XFO. Other CSP directives do not. */
    if(ok && !ancestors && xfo && xfo[0]){
        if(!ip_bounded(xfo))ok=0;
        size_t at=0;struct ips v;
        while(ok && ip_next_policy(xfo,&at,&v)){
            if(ip_eq(v,"deny"))ok=0;
            else if(ip_eq(v,"sameorigin"))ok=!strcmp(p.origin,c.origin);
            else if(v.n)ok=0; /* unsupported obsolete directives refuse */
        }
    }
#ifdef IFRAME_POLICY_NO_ENFORCEMENT
    ok=1; /* Test-only refusal control, never a browser build option. */
#endif
    ip_url_free(&p);ip_url_free(&c);return ok;
}
int iframe_policy_resource(const char *document_url,const char *resource_url,const char *csp,int kind)
{
    struct ipurl d,r;if(!ip_url(document_url,&d))return 0;
    if(!ip_url(resource_url,&r)){ip_url_free(&d);return 0;}
    int ok=kind==IF_POLICY_STYLE ? ip_policies(csp,&d,&r,"style-src-elem","style-src","default-src",0) :
        kind==IF_POLICY_IMAGE ? ip_policies(csp,&d,&r,"img-src","default-src",0,0):
        kind==IF_POLICY_CONNECT ? ip_policies(csp,&d,&r,"connect-src","default-src",0,0):0;
    if(!strcmp(d.scheme,"https:") && !strcmp(r.scheme,"http:"))ok=0;
    ip_url_free(&d);ip_url_free(&r);return ok;
}
int iframe_policy_base(const char *document_url,const char *base_url,const char *csp)
{
    struct ipurl d,b;if(!ip_url(document_url,&d))return 0;
    if(!ip_url(base_url,&b)){ip_url_free(&d);return 0;}
    /* base-uri is a document directive, with no default-src fallback. A
     * rejected base element leaves the document URL as the fallback base. */
    int ok=ip_policies(csp,&d,&b,"base-uri",0,0,0);
    ip_url_free(&d);ip_url_free(&b);return ok;
}
static int ip_inline(const char *all,const char *nonce,int attr,int script)
{
    if(!ip_bounded(all))return 0;
    size_t at=0;struct ips p;
    while(ip_next_policy(all,&at,&p)){
        if(ip_directive(p,"sandbox").found)return 0;
        struct ips l=ip_select(p,script?(attr?"script-src-attr":"script-src-elem"):
            (attr?"style-src-attr":"style-src-elem"),script?"script-src":"style-src","default-src");
        if(!l.found)continue;
        size_t i=0;int unsafe=0,nonce_ok=0,has_key=0;
        while(i<l.n){while(i<l.n && ip_space(l.p[i]))i++;size_t b=i;
            while(i<l.n && !ip_space(l.p[i]))i++;struct ips t={l.p+b,i-b,1};
            if(ip_eq(t,"'unsafe-inline'"))unsafe=1;
            if(script && ip_eq(t,"'strict-dynamic'"))has_key=1;
            if(t.n>8 && !memcmp(t.p,"'nonce-",7) && t.p[t.n-1]=='\''){
                has_key=1;if(!attr && nonce && strlen(nonce)==t.n-8 && !memcmp(t.p+7,nonce,t.n-8))nonce_ok=1;
            }
            if(t.n>8 && (!memcmp(t.p,"'sha256-",8) || !memcmp(t.p,"'sha384-",8) || !memcmp(t.p,"'sha512-",8)))has_key=1;
        }
        if(!nonce_ok && !(unsafe && !has_key))return 0;
    }return 1;
}
int iframe_policy_inline_style(const char *csp,const char *nonce) { return ip_inline(csp,nonce,0,0); }
int iframe_policy_style_attribute(const char *csp) { return ip_inline(csp,0,1,0); }
int iframe_policy_inline_script(const char *csp,const char *nonce) { return ip_inline(csp,nonce,0,1); }
int iframe_policy_script_attribute(const char *csp) { return ip_inline(csp,0,1,1); }
static int ip_token(struct ips l,const char *s)
{
    for(size_t i=0;i<l.n;){while(i<l.n&&ip_space(l.p[i]))i++;size_t b=i;
        while(i<l.n&&!ip_space(l.p[i]))i++;if(ip_eq((struct ips){l.p+b,i-b,1},s))return 1;
    }return 0;
}
int iframe_policy_eval(const char *csp)
{
    if(!ip_bounded(csp))return 0;
    size_t at=0;struct ips p;
    while(ip_next_policy(csp,&at,&p)){
        if(ip_directive(p,"sandbox").found)return 0;
        struct ips l=ip_select(p,"script-src","default-src",0);
        if(l.found&&!ip_token(l,"'unsafe-eval'"))return 0;
    }return 1;
}
int iframe_policy_blob_worker(const char *csp)
{
    if(!ip_bounded(csp))return 0;
    size_t at=0;struct ips p;
    while(ip_next_policy(csp,&at,&p)){
        if(ip_directive(p,"sandbox").found)return 0;
        struct ips l=ip_select(p,"worker-src","child-src","script-src");
        if(!l.found)l=ip_directive(p,"default-src");
        /* Conservative subset: require explicit blob: when a source list
         * exists. '*' is a network wildcard, not a Blob capability grant.
         * Same-origin Blob matching of 'self' is not implemented here. */
        if(l.found&&!ip_token(l,"blob:"))return 0;
    }return 1;
}
int iframe_policy_network_worker(const char *document_url,const char *url,const char *csp)
{
    struct ipurl d,r;if(!ip_url(document_url,&d))return 0;
    if(!ip_url(url,&r)){ip_url_free(&d);return 0;}
    /* worker-src does not authorize a classic cross-origin entry. This
     * includes redirects and HTTP-to-HTTPS origin changes. */
    int ok=ip_bounded(csp)&&!strcmp(d.origin,r.origin);size_t at=0;struct ips p;
    while(ok&&ip_next_policy(csp,&at,&p)){
        if(ip_directive(p,"sandbox").found){ok=0;break;}
        struct ips l=ip_select(p,"worker-src","child-src","script-src");
        if(!l.found)l=ip_directive(p,"default-src");
        ok=!l.found||ip_list(l,&d,&r);
    }
    ip_url_free(&d);ip_url_free(&r);return ok;
}
int iframe_policy_worker_import(const char *document_url,const char *url,const char *csp)
{
    struct ipurl d,r;if(!ip_url(document_url,&d))return 0;
    if(!ip_url(url,&r)){ip_url_free(&d);return 0;}
    int ok=ip_bounded(csp);size_t at=0;struct ips p;
    if(!strcmp(d.scheme,"https:")&&!strcmp(r.scheme,"http:"))ok=0;
    while(ok&&ip_next_policy(csp,&at,&p)){
        if(ip_directive(p,"sandbox").found){ok=0;break;}
        /* importScripts has no element, nonce or parser insertion. CSP3's
         * worker script request uses script-src, NOT script-src-elem. */
        struct ips l=ip_select(p,"script-src","default-src",0);
        ok=!l.found||ip_token(l,"'strict-dynamic'")||ip_list(l,&d,&r);
    }
    ip_url_free(&d);ip_url_free(&r);return ok;
}
int iframe_policy_script(const char *document_url,const char *resource_url,
                         const char *csp,const char *nonce,int parser_inserted)
{
    struct ipurl d,r;if(!ip_url(document_url,&d))return 0;
    if(!ip_url(resource_url,&r)){ip_url_free(&d);return 0;}
    int ok=ip_bounded(csp);
    if(!strcmp(d.scheme,"https:")&&!strcmp(r.scheme,"http:"))ok=0;
    size_t at=0;struct ips p;
    while(ok&&ip_next_policy(csp,&at,&p)){
        if(ip_directive(p,"sandbox").found){ok=0;break;}
        struct ips l=ip_select(p,"script-src-elem","script-src","default-src");
        if(!l.found)continue;
        int nonce_ok=0;
        for(size_t i=0;i<l.n;){while(i<l.n&&ip_space(l.p[i]))i++;size_t b=i;
            while(i<l.n&&!ip_space(l.p[i]))i++;struct ips t={l.p+b,i-b,1};
            if(t.n>8&&!memcmp(t.p,"'nonce-",7)&&t.p[t.n-1]=='\''&&nonce&&
                strlen(nonce)==t.n-8&&!memcmp(t.p+7,nonce,t.n-8))nonce_ok=1;
        }
        if(!nonce_ok)ok=ip_token(l,"'strict-dynamic'")?!parser_inserted:ip_list(l,&d,&r);
    }
    ip_url_free(&d);ip_url_free(&r);return ok;
}
