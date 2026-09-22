/* SPDX-License-Identifier: MIT */
#include "document.h"
int st_boundary(const char *s,int n,int p)
{return p>=0&&p<=n&&(p==n||((unsigned char)s[p]&0xc0)!=0x80);}
int st_utf8(const char *s,int n)
{
    for(int i=0;i<n;){unsigned c=(unsigned char)s[i++],v;int more;
        if(!c)return 0;if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){v=c&31;more=1;}
        else if(c>=0xe0&&c<=0xef){v=c&15;more=2;}
        else if(c>=0xf0&&c<=0xf4){v=c&7;more=3;}else return 0;
        int bytes=more;if(more>n-i)return 0;
        while(more--){c=(unsigned char)s[i++];if((c&0xc0)!=0x80)return 0;v=(v<<6)|(c&63);}
        if((bytes==2&&v<0x800)||(bytes==3&&v<0x10000)||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
    }return 1;
}
int st_prev(const char *s,int p)
{if(p){p--;while(p>0&&((unsigned char)s[p]&0xc0)==0x80)p--;}return p;}
int st_next(const char *s,int p,int n)
{if(p<n){p++;while(p<n&&((unsigned char)s[p]&0xc0)==0x80)p++;}return p;}
int st_encode(unsigned cp,char s[4])
{
    if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||!cp)return 0;
    if(cp<128){s[0]=(char)cp;return 1;}
    if(cp<0x800){s[0]=(char)(0xc0|(cp>>6));s[1]=(char)(0x80|(cp&63));return 2;}
    if(cp<0x10000){s[0]=(char)(0xe0|(cp>>12));s[1]=(char)(0x80|((cp>>6)&63));s[2]=(char)(0x80|(cp&63));return 3;}
    s[0]=(char)(0xf0|(cp>>18));s[1]=(char)(0x80|((cp>>12)&63));s[2]=(char)(0x80|((cp>>6)&63));s[3]=(char)(0x80|(cp&63));return 4;
}
uint32_t st_hash(const void *data,size_t n)
{const unsigned char *p=data;uint32_t h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
int st_reserve(StDocument *d,int length)
{
    if(length<0||length>ST_DOCUMENT_LIMIT)return -1;
    if(length+1<=d->capacity)return 0;
    int cap=d->capacity?d->capacity:256;while(cap<length+1)cap=cap>ST_DOCUMENT_LIMIT/2?ST_DOCUMENT_LIMIT+1:cap*2;
    char *p=realloc(d->text,(size_t)cap);if(!p)return -1;d->text=p;d->capacity=cap;return 0;
}
int st_init(StDocument *d,const char *path,const char *text,int n,int exists)
{
    if(strlen(path)>=sizeof d->path||n<0||n>ST_DOCUMENT_LIMIT||!st_utf8(text,n))return -1;
    memset(d,0,sizeof *d);if(st_reserve(d,n)<0)return -1;
    d->base=malloc((size_t)n+1);if(!d->base){free(d->text);memset(d,0,sizeof *d);return -1;}
    memcpy(d->text,text,(size_t)n);d->text[n]=0;memcpy(d->base,d->text,(size_t)n+1);
    strcpy(d->path,path);d->length=d->base_length=n;d->revision=1;d->exists=exists;return 0;
}
void st_edit_free(StEdit *e){free(e->old_text);free(e->new_text);memset(e,0,sizeof *e);}
void st_dispose(StDocument *d)
{for(int i=0;i<d->undo_count;i++)st_edit_free(&d->edits[i]);free(d->text);free(d->base);memset(d,0,sizeof *d);}
int st_dirty(const StDocument *d)
{return !d->exists||d->length!=d->base_length||memcmp(d->text,d->base,(size_t)d->length);}
int st_apply(StDocument *d,int lo,int hi,const char *text,int n,int remember)
{
    if(lo>hi||!st_boundary(d->text,d->length,lo)||!st_boundary(d->text,d->length,hi)||n<0||!st_utf8(text,n))return -1;
    if(n>ST_DOCUMENT_LIMIT-(d->length-(hi-lo)))return -1;
    StEdit edit={.at=lo,.removed=hi-lo,.inserted=n,.caret=d->caret,.anchor=d->anchor};
    if(remember){
        /* Reserve history and text BEFORE changing a byte. Allocation failure
         * leaves content, selection and undo history usable. Large single edits
         * are retained even when they exceed the usual rolling history budget. */
        edit.old_text=malloc((size_t)edit.removed+1);edit.new_text=malloc((size_t)n+1);
        if(!edit.old_text||!edit.new_text){st_edit_free(&edit);return -1;}
        memcpy(edit.old_text,d->text+lo,(size_t)edit.removed);memcpy(edit.new_text,text,(size_t)n);
    }
    if(st_reserve(d,d->length-(hi-lo)+n)<0){st_edit_free(&edit);return -1;}
    /* `text` may be a selection in d->text. History owns a stable copy even
     * when reserve reallocates, so recorded edits always use that copy. */
    const char *insert=remember?edit.new_text:text;
    memmove(d->text+lo+n,d->text+hi,(size_t)(d->length-hi)+1);
    if(n)memcpy(d->text+lo,insert,(size_t)n);
    d->length+=n-(hi-lo);d->caret=d->anchor=lo+n;d->revision++;
    if(remember){
        while(d->undo_count>d->undo_cursor){StEdit *e=&d->edits[--d->undo_count];d->history_bytes-=e->removed+e->inserted;st_edit_free(e);}
        while(d->undo_count&&(d->undo_count==ST_UNDO_STEPS||d->history_bytes+edit.removed+n>ST_HISTORY_LIMIT)){
            d->history_bytes-=d->edits[0].removed+d->edits[0].inserted;st_edit_free(&d->edits[0]);
            memmove(d->edits,d->edits+1,(size_t)(--d->undo_count)*sizeof(StEdit));d->undo_cursor--;}
        d->edits[d->undo_count++]=edit;d->undo_cursor=d->undo_count;d->history_bytes+=edit.removed+n;
    }return 0;
}
int st_replace(StDocument *d,const char *s,int n)
{int lo=d->caret<d->anchor?d->caret:d->anchor,hi=d->caret>d->anchor?d->caret:d->anchor;return st_apply(d,lo,hi,s,n,1);}
int st_undo(StDocument *d,int redo)
{
    if(redo?d->undo_cursor>=d->undo_count:!d->undo_cursor)return 0;
    StEdit *e=&d->edits[redo?d->undo_cursor:d->undo_cursor-1];
    int n=redo?e->inserted:e->removed,removed=redo?e->removed:e->inserted;
    if(st_apply(d,e->at,e->at+removed,redo?e->new_text:e->old_text,n,0)<0)return -1;
    if(redo)d->undo_cursor++;else {d->undo_cursor--;d->caret=e->caret;d->anchor=e->anchor;}return 1;
}
int st_find(StDocument *d,const char *query,int from)
{
    size_t n=strlen(query);if(!n||n>(size_t)d->length)return -1;
    if(from<0||from>d->length)from=0;
    for(int pass=0;pass<2;pass++){
        int first=pass?0:from,end=pass?from:d->length;
        for(int i=first;i+(int)n<=end;i++)if(st_boundary(d->text,d->length,i)&&!memcmp(d->text+i,query,n))return i;
    }return -1;
}
