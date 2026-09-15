#define main modal_paint_original_main
#include "modal_paint_test.c"
#undef main
int main(void) {
 const char *html="<!doctype html><button id=o>OUTSIDE</button><div id=p popover><span>POPOVER</span><button id=i>INSIDE</button></div><div id=page>PAGE</div>";
 const char *css="body{margin:0;font-size:16px;line-height:20px}#p{width:180px;height:100px;border:0;padding:0;margin:0}#page{position:relative;z-index:2147483647;height:30px}#o{display:block}";
 struct node *root=dom_parse(html,strlen(html));css_viewport(500,400);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));
 struct node *p=dom_get_element_by_id(root->doc,"p"),*o=dom_get_element_by_id(root->doc,"o"),*page=dom_get_element_by_id(root->doc,"page");
 layout_page(root,500);int x,y,w,h;CHECK(!layout_node_box(p,&x,&y,&w,&h),"closed popover has no ordinary layout box");
 CHECK(top_layer_push_popover(p,o),"native popover enters shared paint stack");layout_page(root,500);
 layout_node_box(page,&x,&y,&w,&h);CHECK(y<100,"popover is excluded from ordinary document flow");
 layout_node_box(p,&x,&y,&w,&h);CHECK(x==160&&y==150&&w==180&&h==100,"popover uses viewport top layer geometry");
 paint_nops=0;browser_paint_scroll(0,0,500,400,0,0);int cm,cp;int mi=text_index("POPOVER",&cm),pi=text_index("PAGE",&cp);
 CHECK(cm==1&&mi>pi&&pi>=0,"popover paints once above maximum page z index");
 int backdrop=0;for(int k=0;k<paint_nops;k++)if(paint_ops[k].kind==OP_BLIT&&paint_ops[k].solid&&paint_ops[k].w==500&&paint_ops[k].h==400&&paint_ops[k].alpha==100)backdrop++;
 CHECK(backdrop==0,"popover paints no modal backdrop");struct node *hit=0;char href[40];
 browser_hittest_node_scroll(x+2,y+2,80,120,&hit,href,sizeof href);CHECK(top_layer_owner(hit)==p,"popover hit geometry stays fixed while page scrolls");
 browser_hittest_node_scroll(2,2,0,0,&hit,href,sizeof href);CHECK(hit==o,"popover leaves background hit testing enabled");
 top_layer_remove(p);layout_page(root,500);CHECK(!layout_node_box(p,&x,&y,&w,&h),"hidden popover disappears from layout after close");
 top_layer_reset();focus_reset();layout_free();dom_free(root);puts(fail?"popover paint failed":"popover paint passed");return fail?1:0;
}
