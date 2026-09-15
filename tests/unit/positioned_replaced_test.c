/* A positioned/flex/grid allocator lays out the allocated node itself rather
 * than encountering it as a child in flow. Replaced nodes have no page
 * children: that self path must emit their display-list item. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void kfree(void *p){ free(p); }
int text_measure(const char *s,int n,int px,int mono)
{ (void)s;(void)mono;return n*(px/2); }
int res_fetch(const char *u,uint8_t **b,int *n)
{ (void)u;(void)b;(void)n;return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p,int n,struct image *o)
{ (void)p;(void)n;(void)o;return -1; }

static int checks,fails;
#define CHECK(c,m) do { checks++; if(!(c)){ printf("FAIL: %s\n",m);fails++; } } while(0)

static struct node *find_id(struct node *n,const char *id)
{
    if(n->type==N_ELEM) {
        const char *v=dom_attr(n,"id");if(v&&!strcmp(v,id))return n;
    }
    for(struct node *c=n->first_child;c;c=c->next) {
        struct node *r=find_id(c,id);if(r)return r;
    }
    return 0;
}

int main(void)
{
    const char *html=
        "<html><head><style>body{margin:0}"
        "#stage{position:relative;width:640px;height:360px}"
        "#movie{position:absolute;display:block;left:0;top:0;"
        "width:100%;height:100%}"
        "</style></head><body><div id=stage><video id=movie></video></div>"
        "</body></html>";
    struct node *root=dom_parse(html,(int)strlen(html));
    CHECK(root!=0,"document parses");
    css_apply(root,"body{margin:0}#stage{position:relative;width:640px;height:360px}"
                   "#movie{position:absolute;display:block;left:0;top:0;"
                   "width:100%;height:100%}",
              (int)strlen("body{margin:0}#stage{position:relative;width:640px;height:360px}"
                          "#movie{position:absolute;display:block;left:0;top:0;"
                          "width:100%;height:100%}"));
    layout_page(root,800);
    struct node *movie=find_id(root,"movie");
    CHECK(movie!=0,"video is in the DOM");
    int x=0,y=0,w=0,h=0;
    CHECK(movie&&layout_node_box(movie,&x,&y,&w,&h),"video has a layout box");
    CHECK(x==0&&y==0&&w==640&&h==360,"video box is the positioned 640x360 stage");
    const struct item *it=layout_items();int found=0;
    for(int i=0;movie&&i<layout_count();i++)
        if(it[i].node==movie&&it[i].type==IT_VIDEO&&
           it[i].x==0&&it[i].y==0&&it[i].w==640&&it[i].h==360)found=1;
    CHECK(found,"positioned video emits a 640x360 IT_VIDEO paint item");
    printf("positioned_replaced_test: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
