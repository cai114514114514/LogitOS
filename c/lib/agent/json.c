/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.h"
#include "task.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static void ws(const char *s,size_t n,size_t *p)
{while(*p<n&&(s[*p]==' '||s[*p]=='\r'||s[*p]=='\n'||s[*p]=='\t'))++*p;}
static int hex(char c)
{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static int value(struct ag_json *j,size_t *p,unsigned depth)
{
    if(depth>32||j->count==4096)return -1;
    ws(j->text,j->length,p);if(*p>=j->length)return -1;
    if(j->count==j->cap){int cap=j->cap?j->cap*2:64;void *t=realloc(j->tokens,(size_t)cap*sizeof *j->tokens);if(!t)return -1;j->tokens=t;j->cap=cap;}
    int id=j->count++; j->tokens[id]=(struct ag_jtoken){(int)*p,0,-1,-1,j->text[*p]};
    char c=j->text[(*p)++];int last=-1;
    if(c=='{'||c=='['){
        char close=c=='{'?'}':']';ws(j->text,j->length,p);
        if(*p<j->length&&j->text[*p]==close){++*p;goto done;}
        for(;;){
            if(c=='{'&&(*p>=j->length||j->text[*p]!='"'))return -1;
            int child=value(j,p,depth+1);if(child<0)return -1;
            if(last<0)j->tokens[id].child=child;else j->tokens[last].next=child;last=child;
            if(c=='{'){
                ws(j->text,j->length,p);if(*p>=j->length||j->text[(*p)++]!=':')return -1;
                child=value(j,p,depth+1);if(child<0)return -1;j->tokens[last].next=child;last=child;
            }
            ws(j->text,j->length,p);if(*p>=j->length)return -1;
            char delim=j->text[(*p)++];if(delim==close)break;if(delim!=',')return -1;
            ws(j->text,j->length,p);
        }
    }else if(c=='"'){
        int closed=0;
        while(*p<j->length){unsigned char x=j->text[(*p)++];if(x=='"'){closed=1;break;}if(x<32)return -1;
            if(x=='\\'){if(*p>=j->length)return -1;x=j->text[(*p)++];
                if(x=='u'){for(int k=0;k<4;k++)if(*p>=j->length||hex(j->text[(*p)++])<0)return -1;}
                else if(!strchr("\"\\/bfnrt",x))return -1;}}
        if(!closed)return -1;
    }else{
        size_t start=*p-1;
        while(*p<j->length&&!strchr(" \t\r\n,]}",j->text[*p]))++*p;
        size_t n=*p-start;const char *s=j->text+start;
        if((n==4&&!memcmp(s,"true",4))||(n==5&&!memcmp(s,"false",5))||(n==4&&!memcmp(s,"null",4)))goto done;
        size_t k=0;if(k<n&&s[k]=='-')k++;
        if(k==n)return -1;
        if(s[k]=='0')k++;else {if(s[k]<'1'||s[k]>'9')return -1;while(k<n&&s[k]>='0'&&s[k]<='9')k++;}
        if(k<n&&s[k]=='.'){k++;size_t a=k;while(k<n&&s[k]>='0'&&s[k]<='9')k++;if(k==a)return -1;}
        if(k<n&&(s[k]=='e'||s[k]=='E')){k++;if(k<n&&(s[k]=='+'||s[k]=='-'))k++;size_t a=k;while(k<n&&s[k]>='0'&&s[k]<='9')k++;if(k==a)return -1;}
        if(k!=n)return -1;
    }
done:j->tokens[id].end=(int)*p;return id;
}
int ag_json_parse(struct ag_json *j,const char *s,size_t n)
{memset(j,0,sizeof *j);if(n>8*1024*1024||!s||!ag_utf8(s,(uint32_t)n))return -1;j->text=s;j->length=n;size_t p=0;int r=value(j,&p,0);ws(s,n,&p);if(r<0||p!=n){ag_json_free(j);return -1;}return 0;}
void ag_json_free(struct ag_json *j){free(j->tokens);j->tokens=0;j->count=j->cap=0;}
int ag_json_at(const struct ag_json *j,int id,unsigned n)
{if(id<0||id>=j->count||j->tokens[id].type!='[')return -1;int k=j->tokens[id].child;while(n--&&k>=0)k=j->tokens[k].next;return k;}
char *ag_json_string(const struct ag_json *j,int id)
{
    if(id<0||id>=j->count||j->tokens[id].type!='"')return 0;
    int a=j->tokens[id].start+1,b=j->tokens[id].end-1,o=0;
    char *s=malloc((size_t)(b-a)+1);if(!s)return 0;
    while(a<b){unsigned char c=j->text[a++];if(c!='\\'){s[o++]=c;continue;}c=j->text[a++];
        if(c=='u'){unsigned cp=0;for(int k=0;k<4;k++)cp=(cp<<4)|(unsigned)hex(j->text[a++]);
            if(cp>=0xd800&&cp<=0xdbff){if(a+6>b||j->text[a++]!='\\'||j->text[a++]!='u')goto bad;
                unsigned low=0;for(int k=0;k<4;k++)low=(low<<4)|(unsigned)hex(j->text[a++]);
                if(low<0xdc00||low>0xdfff)goto bad;cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}
            else if(cp>=0xdc00&&cp<=0xdfff)goto bad;
            if(!cp)goto bad;
            if(cp<128)s[o++]=(char)cp;
            else if(cp<2048){s[o++]=0xc0|(cp>>6);s[o++]=0x80|(cp&63);}
            else if(cp<65536){s[o++]=0xe0|(cp>>12);s[o++]=0x80|((cp>>6)&63);s[o++]=0x80|(cp&63);}
            else{s[o++]=0xf0|(cp>>18);s[o++]=0x80|((cp>>12)&63);s[o++]=0x80|((cp>>6)&63);s[o++]=0x80|(cp&63);}
        }else {const char *keys="bfnrt",*vals="\b\f\n\r\t";const char *k=strchr(keys,c);s[o++]=k?vals[k-keys]:c;}}
    s[o]=0;return s;
bad:free(s);return 0;
}
int ag_json_get(const struct ag_json *j,int id,const char *key)
{
    if(id<0||id>=j->count||j->tokens[id].type!='{')return -1;
    int found=-1;
    for(int k=j->tokens[id].child;k>=0;){int v=j->tokens[k].next;if(v<0)return -1;
        char *s=ag_json_string(j,k);if(!s)return -1;int eq=!strcmp(s,key);free(s);
        if(eq){if(found>=0)return -1;found=v;}k=j->tokens[v].next;}
    return found;
}
char *ag_json_quote(const char *s)
{
    size_t n=strlen(s);if(n>2*1024*1024)return 0;char *out=malloc(n*6+3);if(!out)return 0;
    size_t k=0;out[k++]='"';const char *hexes="0123456789abcdef";
    for(size_t i=0;i<n;i++){unsigned char c=s[i];if(c=='"'||c=='\\'){out[k++]='\\';out[k++]=c;}
        else if(c<32){out[k++]='\\';out[k++]='u';out[k++]='0';out[k++]='0';out[k++]=hexes[c>>4];out[k++]=hexes[c&15];}
        else out[k++]=c;}
    out[k++]='"';out[k]=0;return out;
}
