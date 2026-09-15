/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_DOWNLOAD_NAME_H
#define LOGIT_DOWNLOAD_NAME_H
#include <string.h>
/* One destination and one naming rule for browser and CLI downloads. A server
 * suggests a basename, never a path. filename* (UTF-8 RFC 8187) wins over the
 * ASCII filename fallback; the caller may then add a collision suffix. */
#define LOGIT_DOWNLOAD_DIR "/download"
static int dl_lower(int c){return c>='A'&&c<='Z'?c+32:c;}
static int dl_equal(const char *a,int n,const char *b)
{int i=0;for(;i<n&&b[i];i++)if(dl_lower((unsigned char)a[i])!=dl_lower((unsigned char)b[i]))return 0;return i==n&&!b[i];}
static int dl_hex(int c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static int dl_attachment(const char *cd)
{if(!cd)return 0;while(*cd==' '||*cd=='\t')cd++;int n=(int)strcspn(cd,"; \t");return dl_equal(cd,n,"attachment");}
static int dl_parameter(const char *cd,const char *key,char *out,int max)
{
    if(!cd||max<2)return 0;
    const char *p=strchr(cd,';');
    while(p&&*p){p++;while(*p==' '||*p=='\t')p++;const char *name=p;
        while(*p&&*p!='='&&*p!=';')p++;const char *end=p;while(end>name&&(end[-1]==' '||end[-1]=='\t'))end--;
        if(*p!='='){p=strchr(p,';');continue;}p++;while(*p==' '||*p=='\t')p++;
        int wanted=dl_equal(name,(int)(end-name),key),n=0,quoted=*p=='"',overflow=0;if(quoted)p++;
        while(*p&&(quoted?*p!='"':*p!=';')){char c=*p++;if(quoted&&c=='\\'&&*p)c=*p++;
            if(wanted){if(n<max-1)out[n++]=c;else overflow=1;}}
        if(quoted){if(*p!='"')return 0;p++;}
        if(wanted){while(n&&(out[n-1]==' '||out[n-1]=='\t'))n--;out[n]=0;return !overflow&&n>0;}
        p=strchr(p,';');
    }
    return 0;
}
static int dl_filename(const char *url,const char *cd,const char *suggest,char *out,int max)
{
    if(max<2)return 0;
    char value[768],decoded[768];const char *name=NULL;int decode=0;
    if(dl_parameter(cd,"filename*",value,sizeof value)){
        char *lang=strchr(value,'\'');
        if(lang&&dl_equal(value,(int)(lang-value),"utf-8")){
            char *encoded=strchr(lang+1,'\'');
            if(encoded&&encoded[1]){name=encoded+1;decode=1;}
        }
    }
    if(!name&&dl_parameter(cd,"filename",value,sizeof value))name=value;
    if(!name&&suggest&&*suggest)name=suggest;
    if(!name){
        const char *p=url?url:"",*sch=strstr(p,"://");if(sch){p=strchr(sch+3,'/');if(!p)p="";}
        const char *end=p+strcspn(p,"?#"),*start=p;for(const char *q=p;q<end;q++)if(*q=='/')start=q+1;
        size_t n=(size_t)(end-start);if(n>=sizeof value)n=sizeof value-1;memcpy(value,start,n);value[n]=0;name=value;decode=1;
    }
    int n=0;
    for(int i=0;name[i]&&n<(int)sizeof decoded-1;i++){
        unsigned char c=(unsigned char)name[i];
        if(decode&&c=='%'&&name[i+1]&&name[i+2]){int a=dl_hex(name[i+1]),b=dl_hex(name[i+2]);if(a>=0&&b>=0){c=(unsigned char)(a*16+b);i+=2;}}
        if(c<32||c==127||c=='/'||c=='\\'||c==':')c='_';decoded[n++]=(char)c;
    }
    decoded[n]=0;int start=0;while(decoded[start]=='.'||decoded[start]==' ')start++;
    while(n>start&&(decoded[n-1]=='.'||decoded[n-1]==' '))n--;
    int take=n-start;if(take>=max)take=max-1;
    /* Do not leave a truncated UTF-8 code point at the basename boundary. */
    if(start+take<n)while(take>0&&((unsigned char)decoded[start+take]&0xc0)==0x80)take--;
    if(take<=0){const char *fallback="download";take=(int)strlen(fallback);if(take>=max)take=max-1;memcpy(out,fallback,(size_t)take);}
    else memcpy(out,decoded+start,(size_t)take);
    out[take]=0;return take;
}
#endif
