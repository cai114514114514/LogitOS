/* SPDX-License-Identifier: MIT */
#include "internal.h"

int st_engine_insert(StEngine *e,const char *text,int bytes)
{
    StDocument *d=st_current(e);if(!d)return -1;
    if(st_replace(d,text,bytes)<0){st_engine_notice(e,"Edit rejected: invalid UTF-8, document limit or allocation failure");return -1;}
    st_changed(e);return 0;
}
int st_engine_undo(StEngine *e,int redo)
{StDocument *d=st_current(e);if(!d)return -1;int r=st_undo(d,redo);if(r>0)st_changed(e);return r;}
int st_engine_select(StEngine *e,int caret,int anchor)
{
    StDocument *d=st_current(e);if(!d||!st_boundary(d->text,d->length,caret)||!st_boundary(d->text,d->length,anchor))return -1;
    d->caret=caret;d->anchor=anchor;st_engine_dismiss_completion(e);return 0;
}
void st_engine_viewport(StEngine *e,int top,int left)
{StDocument *d=st_current(e);if(d){int last=0;for(int i=0;i<d->length;i++)if(d->text[i]=='\n')last++;d->top=top<0?0:top>last?last:top;d->left=left<0?0:left;}}
int st_engine_delete(StEngine *e,int forward)
{
    StDocument *d=st_current(e);if(!d)return -1;
    int anchor=d->anchor;
    if(anchor==d->caret)d->anchor=forward?st_next(d->text,d->caret,d->length):st_prev(d->text,d->caret);
    int r=st_engine_insert(e,"",0);if(r<0)d->anchor=anchor;return r;
}
int st_engine_newline(StEngine *e)
{
    StDocument *d=st_current(e);if(!d)return -1;
    int lo=d->caret<d->anchor?d->caret:d->anchor,start=lo;
    while(start&&d->text[start-1]!='\n')start--;
    char indent[260];int n=1;indent[0]='\n';
    for(int i=start;i<lo&&d->text[i]==' '&&n<252;i++)indent[n++]=' ';
    if(lo>start&&d->text[lo-1]==':')for(int i=0;i<4;i++)indent[n++]=' ';
    return st_engine_insert(e,indent,n);
}
int st_engine_find(StEngine *e,const char *query)
{
    StDocument *d=st_current(e);if(!d)return -1;int p=st_find(d,query,d->caret);
    if(p<0){st_engine_notice(e,"Text not found");return -1;}
    return st_engine_select(e,p+(int)strlen(query),p);
}
int st_engine_replace_found(StEngine *e,const char *query,const char *replacement)
{
    StDocument *d=st_current(e);if(!d)return -1;
    int lo=d->caret<d->anchor?d->caret:d->anchor,hi=d->caret>d->anchor?d->caret:d->anchor;
    if(hi-lo==(int)strlen(query)&&hi>lo&&!memcmp(d->text+lo,query,(size_t)(hi-lo)))
        if(st_engine_insert(e,replacement,(int)strlen(replacement))<0)return -1;
    return st_engine_find(e,query);
}
void st_engine_move(StEngine *e,enum StMove move,int rows,int extend)
{
    StDocument *d=st_current(e);if(!d)return;
    int row=st_caret_line(d),p=d->caret,start=st_line_start(d,row),col=0;
    for(int i=start;i<p;i=st_next(d->text,i,d->length))col++;
    if(move==ST_LEFT)p=st_prev(d->text,p);
    else if(move==ST_RIGHT)p=st_next(d->text,p,d->length);
    else if(move==ST_HOME)p=start;
    else if(move==ST_END)p=st_line_end(d,p);
    else if(move==ST_FIRST)p=0;
    else if(move==ST_LAST)p=d->length;
    else {if(rows<1)rows=1;row+=move==ST_UP?-rows:rows;if(row<0)row=0;p=st_line_start(d,row);
        while(col--&&p<d->length&&d->text[p]!='\n')p=st_next(d->text,p,d->length);}
    st_engine_select(e,p,extend?d->anchor:p);
}
