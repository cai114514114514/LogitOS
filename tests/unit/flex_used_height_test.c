/* Shipping DOM/CSS/layout/painter regression for used row heights. A larger
 * background is insufficient: the absolute child's retained clip and actual
 * text paint must both receive the stretched size before contents are laid out. */
#define main modal_paint_original_main
#include "modal_paint_test.c"
#undef main
static struct node *doc;
static int checks, failures;
#undef CHECK
#define CHECK(c,m) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n",m); } } while(0)
static struct node *node(const char *id){return dom_get_element_by_id(doc->doc,id);}
static int dim(const char *id,int k){int v[4]={-1,-1,-1,-1};CHECK(layout_node_box(node(id),v,v+1,v+2,v+3),"fixture box exists");return v[k];}
static void page2(const char *html,const char *css,int w,int h){
 if(doc){layout_free();dom_free(doc);}
 doc=dom_parse(html,(int)strlen(html));css_viewport(w,h);
 css_apply(doc,css,(int)strlen(css));css_extra_apply(doc,css,(int)strlen(css));layout_page(doc,w);
}
static const char *nested_html="<body><div id=app><div id=row><div id=column><div id=absolute><div id=welcome>VISIBLE</div><textarea>INPUT</textarea></div></div></div></div>";
static const char *nested_css="body{margin:0;font-size:14px;line-height:20px}#app{display:flex;width:865px;height:562px}#row{display:flex;flex-direction:row;flex-grow:1;min-width:0}#column{flex-direction:column;flex:1;max-width:100%;display:flex;position:relative;overflow:hidden}#absolute{display:flex;position:absolute;top:0;bottom:0;left:0;right:0;flex-direction:column}#welcome{height:60px;margin-top:60px}textarea{height:36px}";
static void nested(void){
 const char *extra[]={"","#column{overflow:visible}","#column{height:562px}"};
 for(int j=0;j<3;j++){
  char css[2048];snprintf(css,sizeof css,"%s%s",nested_css,extra[j]);page2(nested_html,css,865,562);
  CHECK(dim("column",3)==562,"nested row stretch reaches column contents");
  CHECK(dim("absolute",3)==562,"absolute opposing insets use stretched containing height");
  const struct item *it=layout_items();int retained=0,clip=-1;
  for(int i=0;i<layout_count();i++)if(it[i].type==IT_TEXT&&it[i].len==7&&!memcmp(it[i].text,"VISIBLE",7)){retained++;clip=it[i].has_clip?it[i].clip_h:-1;}
  CHECK(retained==1,"trial layout leaves exactly one retained text item");
  CHECK(j==1?clip==-1:clip==562,"retained text clip matches used height");
  paint_nops=0;browser_paint(0,0,865,562,0);int count=0;text_index("VISIBLE",&count);
  CHECK(count==1,"stretched clipped welcome is actually painted");
  if(j!=2)CHECK(!((struct cstyle*)node("column")->style)->has_h,"row used height leaves computed auto intact");
  layout_page(doc,865);CHECK(dim("absolute",3)==562,"repeat layout preserves used height");
 }
}
static void sidebar(void){
 page2("<body><div id=app><div id=side><div id=head>HEAD</div><div id=scroll><div id=long>FIRST</div><div id=end>LAST</div></div><div id=foot>ACCOUNT</div></div><div id=main>MAIN</div></div>",
  "body{margin:0;font-size:14px;line-height:20px}#app{display:flex;width:865px;height:562px}#side{display:flex;flex-direction:column;width:260px}#head{height:60px;flex:none}#foot{height:44px;flex:none}#scroll{flex:1;overflow:auto}#long{height:900px;background:#eee}#end{height:20px}#main{flex:1}",865,562);
 CHECK(dim("side",3)==562,"sidebar stays within outer row viewport");
 CHECK(dim("scroll",3)==458,"sidebar scrollport receives remaining height");
 CHECK(dim("foot",1)==518&&dim("foot",3)==44,"sidebar account footer remains visible");
 CHECK(dim("long",3)==900,"sidebar overflow content retains natural size");
 int clip=-1;const struct item *it=layout_items();for(int i=0;i<layout_count();i++)if(it[i].node==node("long")&&it[i].type==IT_RECT)clip=it[i].has_clip?it[i].clip_h:-1;
 CHECK(clip==458,"sidebar content is clipped to its own scrollport");
 paint_nops=0;browser_paint(0,0,865,562,0);int count=0;text_index("ACCOUNT",&count);CHECK(count==1,"sidebar account footer is painted");
}
static void semantics(void){
 const char *html="<body><div id=row><div id=a><div id=pct></div></div></div>";
 const char *rules[]={"", "#a{align-self:flex-start}","#a{margin-top:auto}","#a{max-height:80px}","#row{height:0}","#a{padding:10px;border:2px solid}","#row{flex-direction:row-reverse}"};
 for(int i=0;i<7;i++){
  char css[1024];snprintf(css,sizeof css,"body{margin:0;font-size:14px}#row{display:flex;width:240px;height:200px}#a{width:80px;overflow:hidden}#pct{height:50%%}%s",rules[i]);page2(html,css,300,300);
  if(i==1||i==2){CHECK(dim("pct",3)==0,"non-stretched auto cross size remains indefinite");CHECK(dim("a",3)<200,"alignment or auto margin prevents stretch");}
  else{int ah=i==3?80:i==4?0:200;int ph=i==5?88:ah/2;CHECK(dim("a",3)==ah,"used stretch honors min max and zero sizes");CHECK(dim("pct",3)==ph,"percent child uses final content size without doubled padding");}
  CHECK(!((struct cstyle*)node("a")->style)->has_h,"stretch scope restores auto computed height");
 }
}
int main(void){css_init();nested();sidebar();semantics();layout_free();dom_free(doc);printf("flex-used-height: %d checks, %d failures\n",checks,failures);return failures?1:0;}
