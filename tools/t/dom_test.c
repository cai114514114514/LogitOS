#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void*p){free(p);}
static int fail;
static struct node *find(struct node *n, const char *tag){
    if(n->type==N_ELEM && !strcmp(n->tag,tag)) return n;
    for(struct node*c=n->first_child;c;c=c->next){struct node*r=find(c,tag); if(r)return r;}
    return 0;
}
static const char *firsttext(struct node *n){
    for(struct node*c=n->first_child;c;c=c->next){ if(c->type==N_TEXT)return c->text; }
    return "";
}
#define CK(cond,msg) do{ if(!(cond)){printf("FAIL %s\n",msg);fail=1;} }while(0)
int main(void){
    const char*h="<html><body><h1 id=t>Hi&amp;You</h1><p class='a b'>x<a href=/y>z</a></p><img src=p.png><br></body></html>";
    struct node*root=dom_parse(h,strlen(h));
    CK(root,"root");
    struct node*body=find(root,"body"); CK(body,"body");
    struct node*h1=find(root,"h1"); CK(h1,"h1");
    CK(h1&&!strcmp(dom_attr(h1,"id")?:"","t"),"h1#t");
    CK(h1&&!strcmp(firsttext(h1),"Hi&You"),"h1 text entity (&amp;->&)");
    struct node*pp=find(root,"p"); CK(pp,"p");
    CK(pp&&!strcmp(dom_attr(pp,"class")?:"","a b"),"p.class");
    struct node*a=find(root,"a"); CK(a,"a");
    CK(a&&!strcmp(dom_attr(a,"href")?:"","/y"),"a href");
    CK(a&&!strcmp(firsttext(a),"z"),"a text");
    struct node*img=find(root,"img"); CK(img,"img");
    CK(img&&!strcmp(dom_attr(img,"src")?:"","p.png"),"img src");
    CK(img&&!img->first_child,"img is void (no children)");
    printf(fail?"SOME FAILED\n":"ALL PASS\n"); return fail;
}
