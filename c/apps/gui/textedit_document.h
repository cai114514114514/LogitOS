/* SPDX-License-Identifier: MIT */
#ifndef TEXTEDIT_DOCUMENT_H
#define TEXTEDIT_DOCUMENT_H
#include <string.h>
/* Byte offsets always remain UTF-8 boundaries. Replacement checks capacity
 * before touching any byte, including a selected range at the 1 MiB limit. */
static int ted_prev(const char *s,int p)
{if(p>0){p--;while(p>0&&((unsigned char)s[p]&0xc0)==0x80)p--;}return p;}
static int ted_next(const char *s,int p,int n)
{if(p<n){p++;while(p<n&&((unsigned char)s[p]&0xc0)==0x80)p++;}return p;}
static int ted_replace(char *s,int *n,int cap,int *caret,int *anchor,const char *insert,int count)
{
    int lo=*caret<*anchor?*caret:*anchor,hi=*caret>*anchor?*caret:*anchor;
    if(lo<0||hi>*n||count<0||count>cap-(*n-(hi-lo)))return -1;
    memmove(s+lo+count,s+hi,(size_t)(*n-hi+1));
    if(count)memcpy(s+lo,insert,(size_t)count);
    *n+=count-(hi-lo);*caret=*anchor=lo+count;return 0;
}
/* A bounded changed interval is exact: outside it both versions are byte
 * identical. It deliberately makes no semantic claim about moved paragraphs. */
static void ted_difference(const char *a,int an,const char *b,int bn,int *prefix,int *aend,int *bend)
{
    int p=0,s=0;while(p<an&&p<bn&&a[p]==b[p])p++;
    while(p>0&&((p<an&&((unsigned char)a[p]&0xc0)==0x80)||(p<bn&&((unsigned char)b[p]&0xc0)==0x80)))p--;
    while(s<an-p&&s<bn-p&&a[an-1-s]==b[bn-1-s])s++;
    while(s>0&&((an-s<an&&((unsigned char)a[an-s]&0xc0)==0x80)||(bn-s<bn&&((unsigned char)b[bn-s]&0xc0)==0x80)))s--;
    *prefix=p;*aend=an-s;*bend=bn-s;
}
#endif
