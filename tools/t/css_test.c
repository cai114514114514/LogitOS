#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "css.h"
void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void*p){free(p);}
static int fail;
static struct node *find(struct node*n,const char*tag){ if(n->type==N_ELEM&&!strcmp(n->tag,tag))return n; for(struct node*c=n->first_child;c;c=c->next){struct node*r=find(c,tag);if(r)return r;} return 0; }
#define CK(c,m) do{if(!(c)){printf("FAIL %s\n",m);fail=1;}}while(0)
int main(void){
    const char*h="<html><body><h1 id=t>Hi</h1><p class='a'>x<a href=q>z</a></p></body></html>";
    struct node*root=dom_parse(h,strlen(h));
    const char*css="h1{color:#f00;font-size:32px} body h1{color:#00ff00} #t{color:#0000ff;margin:10px} .a{font-weight:bold}";
    css_apply(root,css,strlen(css));
    struct cstyle*h1=((struct node*)find(root,"h1"))->style;
    CK(h1->font_px==32,"h1 font 32");
    CK(h1->color==0x0000ff,"h1 color = #00f (#t id beats type rules)");
    CK(h1->ml==10&&h1->mr==10&&h1->mt==10&&h1->mb==10,"h1 margin 10 (#t)");
    CK(h1->display==DISP_BLOCK,"h1 display block (UA)");
    struct cstyle*p=((struct node*)find(root,"p"))->style;
    CK(p->display==DISP_BLOCK,"p block (UA)");
    struct cstyle*a=((struct node*)find(root,"a"))->style;
    CK(a->underline==1,"a underline (UA)");
    CK(a->color==0x1a0dab,"a color blue (UA)");
    /* class .a applies bold to <p>, and bold inherits to its text/children */
    CK(p->bold==1,"p.a bold");
    CK(a->bold==1,"bold inherits to <a> inside p");
    printf(fail?"SOME FAILED\n":"ALL PASS\n"); return fail;
}
