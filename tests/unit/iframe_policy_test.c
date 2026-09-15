#include <stdio.h>
#include <string.h>
#include "iframe_policy.h"
static int checks,failed;
static void ck(int value,int want,const char *name)
{ checks++;if(value!=want){failed++;printf("FAIL: %s\n",name);} }
#define FRAME(p,c,x,w,n) ck(iframe_policy_frame("https://parent.example/page","https://child.example/view",p,c,x),w,n)
#define IMAGE(c,u,w,n) ck(iframe_policy_resource("https://child.example/page",u,c,IF_POLICY_IMAGE),w,n)
#define STYLE(c,u,w,n) ck(iframe_policy_resource("https://child.example/page",u,c,IF_POLICY_STYLE),w,n)
int main(int argc,char **argv)
{
    (void)argv;
    FRAME(0,0,"DENY",0,"deny XFO");
    FRAME("frame-src 'none'",0,0,0,"deny parent source");
    FRAME(0,"frame-ancestors 'none'",0,0,"deny child ancestors");
    if(argc>1)goto done;
    FRAME(0,0,0,1,"unrestricted ordinary frame");
    FRAME("script-src 'none'","script-src 'none'",0,1,"unrelated script policy does not block inert document");
    FRAME("frame-src https://child.example",0,0,1,"explicit parent host");
    FRAME("FRAME-SRC https://child.example",0,0,1,"directive name insensitive");
    FRAME("default-src 'none'; frame-src https://child.example",0,0,1,"frame-src overrides fallback");
    FRAME("child-src https://child.example",0,0,1,"child-src fallback");
    FRAME("default-src 'self'",0,0,0,"default-src fallback");
    FRAME("frame-src https://child.example\nframe-src 'none'",0,0,0,"separate policies intersect");
    FRAME("frame-src https://child.example, frame-src 'none'",0,0,0,"comma policies intersect");
    FRAME("frame-src 'none';frame-src *",0,0,0,"duplicate directive first wins");
    FRAME("frame-src *;frame-src 'none'",0,0,1,"first permissive duplicate remains authoritative");
    FRAME(0,"frame-ancestors https://parent.example","DENY",1,"enforced ancestor policy supersedes XFO");
    FRAME(0,"default-src 'none'",0,1,"default-src is not an ancestor policy");
    FRAME(0,"frame-ancestors 'self'",0,0,"cross-origin ancestor self refused");
    FRAME(0,"frame-ancestors https://parent.example\nframe-ancestors 'none'",0,0,"ancestor policies intersect");
    FRAME(0,"frame-ancestors https://parent.example/page",0,0,"ancestor compares origin without document path");
    FRAME(0,"frame-ancestors https://parent.example/",0,1,"ancestor origin root path");
    ck(iframe_policy_frame("https://parent.example/a","http://child.example/b",0,0,0),0,"mixed frame document refused");
    ck(iframe_policy_frame("http://parent.example/a","https://child.example/b",0,0,0),1,"secure frame upgrade allowed");
    FRAME(0,0,"SAMEORIGIN",0,"XFO different origin");
    ck(iframe_policy_frame("https://same.example/a","https://same.example/b",0,0,"sameorigin\nSAMEORIGIN"),1,"XFO repeated same origin");
    ck(iframe_policy_frame("https://same.example/a","https://same.example/b",0,0,"sameorigin,DENY"),0,"conflicting XFO deny");
    FRAME(0,0,"ALLOW-FROM https://parent.example",0,"unsupported obsolete XFO refused");
    FRAME("sandbox allow-same-origin",0,0,0,"parent sandbox not fabricated");
    FRAME(0,"sandbox allow-same-origin",0,0,"child sandbox not fabricated");
    IMAGE("img-src *.example","https://asset.example/a.png",1,"wildcard subdomain");
    IMAGE("img-src *.example","https://example/a.png",0,"wildcard excludes bare suffix");
    IMAGE("img-src *.example","https://asset.example.test/a.png",0,"wildcard host boundary");
    IMAGE("img-src https://asset.example","https://asset.example:9443/a.png",0,"implicit port must be default");
    IMAGE("img-src https://asset.example:*","https://asset.example:9443/a.png",1,"explicit wildcard port");
    IMAGE("img-src https://asset.example:9443","https://asset.example:9443/a.png",1,"explicit port");
    IMAGE("img-src https:","https://asset.example/a.png",1,"scheme source");
    IMAGE("img-src http:","https://asset.example/a.png",1,"secure scheme upgrade");
    IMAGE("img-src https://asset.example/assets/","https://asset.example/assets/a.png",1,"directory source");
    IMAGE("img-src https://asset.example/assets/","https://asset.example/assets-extra/a.png",0,"directory boundary");
    IMAGE("img-src https://asset.example/a.png","https://asset.example/a.png?size=2#end",1,"query and fragment excluded from path check");
    IMAGE("img-src https://asset.example/a.png","https://asset.example/a.png/more",0,"exact path has no suffix grant");
    IMAGE("img-src https://asset.example/a%20b/","https://asset.example/a%20b/image",1,"escaped path segment");
    IMAGE("img-src https://asset.example/a%2Fb/","https://asset.example/a/b/image",0,"encoded slash is not directory separator");
    IMAGE("img-src 'self'","https://child.example/image",1,"self resource");
    IMAGE("img-src 'self'","https://child.example:9443/image",0,"self port differs");
    IMAGE("img-src 'self'","https://child.example.test/image",0,"self host boundary");
    IMAGE("img-src 'none'","https://child.example/image",0,"image directive none");
    IMAGE("default-src 'none'","https://child.example/image",0,"image fallback none");
    IMAGE("img-src *","http://child.example/image",0,"mixed resource not fetched");
    IMAGE(0,"data:image/png;base64,AA==",0,"unsupported resource scheme");
    IMAGE(0,"https://user:secret@asset.example/image",0,"userinfo URL refused");
    STYLE("style-src 'self'","https://child.example/main.css",1,"style self");
    STYLE("style-src 'self';style-src-elem https://asset.example","https://asset.example/main.css",1,"style element source overrides generic");
    STYLE("style-src-elem 'none';style-src *","https://child.example/main.css",0,"style element none");
    ck(iframe_policy_inline_style(0,0),1,"unrestricted inline style");
    ck(iframe_policy_inline_style("script-src 'none'",0),1,"unrelated inline script directive");
    ck(iframe_policy_inline_style("default-src 'none'",0),0,"inline default none");
    ck(iframe_policy_inline_style("style-src 'unsafe-inline'",0),1,"unsafe inline style");
    ck(iframe_policy_inline_style("style-src 'nonce-YWJj'","YWJj"),1,"exact nonce");
    ck(iframe_policy_inline_style("style-src 'nonce-YWJj'","ywjj"),0,"nonce case sensitive");
    ck(iframe_policy_inline_style("style-src 'nonce-YWJj' 'unsafe-inline'",0),0,"nonce source suppresses unsafe inline");
    ck(iframe_policy_inline_style("style-src 'sha256-localhash=' 'unsafe-inline'",0),0,"hash not replaced with fabricated digest");
    ck(iframe_policy_inline_style("style-src 'unsafe-inline'\nstyle-src 'none'",0),0,"inline policies intersect");
    ck(iframe_policy_style_attribute("style-src-elem 'none';style-src-attr 'unsafe-inline'"),1,"style attributes have own directive");
    ck(iframe_policy_style_attribute("style-src-attr 'none';style-src 'unsafe-inline'"),0,"style attribute none overrides generic");
    ck(iframe_policy_style_attribute("style-src 'nonce-YWJj'"),0,"style attribute cannot use nonce");
    ck(iframe_policy_base("https://child.example/view","https://asset.example/",0),1,"base with no directive");
    ck(iframe_policy_base("https://child.example/view","https://asset.example/","default-src 'none'"),1,"base has no default-src fallback");
    ck(iframe_policy_base("https://child.example/view","https://asset.example/","base-uri 'none'"),0,"base none refuses replacement");
    ck(iframe_policy_base("https://child.example/view","https://asset.example/","base-uri 'self'"),0,"base self refuses different origin");
    ck(iframe_policy_base("https://child.example/view","https://child.example/assets/","base-uri 'self'"),1,"base self permits same origin path");
    ck(iframe_policy_base("https://child.example/view","https://asset.example/","base-uri https://asset.example"),1,"base explicit source");
    ck(iframe_policy_base("https://child.example/view","https://asset.example/","base-uri *\nbase-uri 'none'"),0,"base policies intersect");
    ck(iframe_policy_base("https://child.example/view","data:text/html,","base-uri *"),0,"base nonnetwork scheme refused");
    ck(iframe_policy_frame(0,"https://child.example",0,0,0),0,"missing document owner");
    { char huge[9000];memset(huge,' ',sizeof huge);huge[sizeof huge-1]=0;
      FRAME(huge,0,0,0,"policy overflow refused"); }
done:
    printf("iframe-policy: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
