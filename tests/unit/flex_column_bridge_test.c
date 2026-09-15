/* DOM -> computed CSS -> real display-list sizing. The pure flex solver's
 * column tests cannot detect a caller that never invokes it for columns.
 * Deepseek's saved menu uses this fixed/column + flex:1/overflow:auto shape;
 * use generic names and deterministic content, without fetching its scripts. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static int ypos(const char *id){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"position exists");return y;}
static void col(const char *rule,const char *children){
 char html[8192];snprintf(html,sizeof html,"<style>body{margin:0}#col{display:flex;flex-direction:column;width:240px;height:200px}%s</style><div id=col>%s</div>",rule,children);page(html,400);
}
int main(void){
 css_init();css_viewport(400,600);
 col("#head{flex:none;height:40px}#body{flex:1;overflow-y:auto}","<div id=head>HEAD</div><div id=body><div id=long style='height:400px;background:red'></div></div>");
 EQ(height("body"),160,"column scrollport consumes remaining main size");EQ(ypos("body"),40,"scrollport follows fixed header");EQ(height("long"),400,"shrinking scrollport preserves overflowing content");
 int clipped=0;const struct item *it=layout_items();for(int i=0;i<layout_count();i++)if(it[i].node==ID("long")&&it[i].type==IT_RECT)clipped=it[i].has_clip&&it[i].clip_h==160;
 CHECK(clipped,"paint clipping follows resolved column scrollport");
 col("#a,#b{height:160px;min-height:0}","<div id=a></div><div id=b></div>");
 EQ(height("a"),100,"column shrink resolves first item");EQ(height("b"),100,"column shrink resolves second item");EQ(ypos("b"),100,"shrunk geometry moves following item");
 col("#a{flex-grow:1;flex-shrink:1;flex-basis:0%;min-height:0}#b{flex-grow:3;flex-shrink:1;flex-basis:0%;min-height:0}","<div id=a><div style='height:100px'></div></div><div id=b><div style='height:20px'></div></div>");
 EQ(height("a"),50,"column basis zero excludes natural content from growth");EQ(height("b"),150,"column growth uses authored ratio");
 col("#a{flex:1;min-height:80px}#b{flex:1;max-height:60px}","<div id=a></div><div id=b></div>");
 EQ(height("a"),140,"column redistributes after max-height freeze");EQ(height("b"),60,"column max-height constrains growth");
 col("#a{height:140px;flex-shrink:0}#b{height:140px;min-height:0}","<div id=a></div><div id=b></div>");
 EQ(height("a"),140,"shrink zero keeps main size");EQ(height("b"),60,"other column item absorbs remaining shrink");
 col("#a{flex:1;min-height:0;padding:10px;border:2px solid}#pct{height:50%}","<div id=a><div id=pct></div></div>");
 EQ(height("a"),200,"used column border box includes padding once");EQ(height("pct"),88,"percentage descendant uses post-flex content height");
 CHECK(!((struct cstyle*)ID("a")->style)->has_h,"used flex height does not overwrite computed auto height");
 layout_page(g_root,400);EQ(height("pct"),88,"repeated layout recomputes the same percentage basis");
 col("#col{flex-direction:column-reverse}#a,#b{height:40px;flex:none}","<div id=a></div><div id=b></div>");
 EQ(ypos("a"),160,"column reverse starts at main end");EQ(ypos("b"),120,"column reverse preserves unused top space");
 col("#col{gap:10px}#a,#b{flex:1;min-height:0}","<div id=a></div><div id=b></div>");
 EQ(height("a"),95,"main gap excluded before growth");EQ(ypos("b"),105,"main gap retained between flexed items");
 col("#a{height:40px;flex:none;margin-top:auto}","<div id=a></div>");EQ(ypos("a"),160,"main auto margin consumes unused space");
 col("#col{height:auto}#a,#b{height:30px}","<div id=a></div><div id=b></div>");EQ(height("col"),60,"indefinite column remains natural stack");
 col("#a{flex:1;min-height:0;position:relative;overflow:hidden}#abs{position:absolute;bottom:0;height:25%;width:20px;background:blue}","<div id=a><div id=abs></div></div>");
 EQ(height("abs"),50,"absolute child uses final flex containing block height");EQ(ypos("abs"),150,"absolute bottom inset uses final flex slot");
 int rects=0;it=layout_items();for(int i=0;i<layout_count();i++)if(it[i].node==ID("abs")&&it[i].type==IT_RECT)rects++;
 EQ(rects,1,"column trial discards temporary positioned paint items");
 col("#a,#b{flex:1;overflow:visible}","<div id=a><div style='height:150px'></div></div><div id=b><div style='height:150px'></div></div>");
 EQ(height("a"),150,"visible overflow retains automatic minimum height");EQ(height("b"),150,"content minima may overflow definite column");
 col("#col{height:0}#a{flex:1;min-height:0;overflow:hidden}","<div id=a><div style='height:60px'></div></div>");
 EQ(height("a"),0,"zero definite height is not an indefinite flex basis");
 col("#a{flex:1;min-height:0;display:flex;flex-direction:column}#head{height:40px;flex:none}#body{flex:1;overflow:auto}","<div id=a><div id=head></div><div id=body><div style='height:400px'></div></div></div>");
 EQ(height("a"),200,"nested flex column receives definite post-flex size");EQ(height("body"),160,"nested flex body resolves its own remaining space");
 printf("flex-column-bridge: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
