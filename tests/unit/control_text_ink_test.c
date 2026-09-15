/* Finite ordinary fixed native metrics; real raster/API parity is a separate
 * kernel gate. The expected origin is independently found by enumeration. */
#define main unused_modal_main
#include "modal_paint_test.c"
#undef main
#include "forms.h"
#include "control_text_metrics.h"
static int checks,failures,scale=100,queries,status=1,last_run_limit=0,query_contract_bad=0;
static const char *name;
#define EXPECT(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s: %s\n",name,m);}}while(0)
static int S(int x){return x>=0?x*scale/100:-((-x*scale+99)/100);}
static struct logit_text_metrics oracle(const char *s,int n)
{
 struct logit_text_metrics m={0};m.scale_percent=scale;m.baseline=16*scale/100;m.ascent=m.baseline;m.descent=-4*scale/100;
 int h=0,g=0,cjk=0,period=0,dash=0;
 for(int i=0;i<n;i++){unsigned char c=s[i];if(c=='g')g=1;else if(c=='.')period=1;else if(c=='-')dash=1;else if(c>=128)cjk=1;else if(c!=' '&&c!='\t')h=1;}
 int j=scale==100?0:scale==150?1:2,ht[]={5,8,11},hb[]={16,24,32},gt[]={8,12,16},gb[]={20,30,40},ct[]={4,6,8},cb[]={18,27,36};
 int top=999,bottom=-999;
 if(h){top=ht[j];bottom=hb[j];}if(g){if(gt[j]<top)top=gt[j];if(gb[j]>bottom)bottom=gb[j];}if(cjk){if(ct[j]<top)top=ct[j];if(cb[j]>bottom)bottom=cb[j];}
 if(period){int t=14*scale/100,b=16*scale/100;if(t<top)top=t;if(b>bottom)bottom=b;}
 if(dash){int t=10*scale/100,b=12*scale/100;if(t<top)top=t;if(b>bottom)bottom=b;}
 m.has_ink=h||g||cjk||period||dash;if(m.has_ink){m.ink_top=top;m.ink_bottom=bottom;m.ink_right=n*7*scale/100;}
 return m;
}
int text_run_metrics_px(const char *s,int n,int px,int face,struct logit_text_metrics *m)
{
 queries++;if(n>last_run_limit)last_run_limit=n;
 if(px!=14||face!=0)query_contract_bad=1;
 if(status!=1)return status;
 *m=oracle(s,n);return 1;
}
static int best_y(int cy,int h,int top,int bottom)
{
 int best=cy-200,error=100000;
 for(int y=cy-200;y<=cy+200;y++){int e=2*S(y)+top+bottom-S(cy)-S(cy+h);if(e<0)e=-e;if(e<error){error=e;best=y;}}
 return best;
}
static void arithmetic(void)
{
 name="signed-scale-centering";
 const int tops[]={-5,-1,0,4,8},heights[]={9,15,22},origins[]={-17,-1,0,1,23};
 for(int z=0;z<3;z++){scale=100+z*50;for(int a=0;a<5;a++)for(int b=0;b<3;b++)for(int c=0;c<5;c++){
  int cy=origins[c],h=20;struct logit_text_metrics m={0};m.scale_percent=scale;m.has_ink=1;m.ink_top=tops[a];m.ink_bottom=tops[a]+heights[b];
  struct control_text_vertical v=control_text_center_ink(cy,h,14,&m);int want=best_y(cy,h,m.ink_top,m.ink_bottom);
  EXPECT(v.draw_y==want,"signed integer origin minimizes device center error");
  int top=S(v.draw_y)+m.ink_top,bottom=S(v.draw_y)+m.ink_bottom;
  if(top>=S(cy)&&bottom<=S(cy+h))EXPECT(S(v.selection_y)<=top&&S(v.selection_y+v.selection_h)>=bottom,"outward selection covers complete fitting ink");
  EXPECT(v.selection_y==v.caret_y&&v.selection_h==v.caret_h,"caret and selection share visible vertical extent");
 }}
}
static void sample(const char *label,const char *markup,const char *text,int value,int selected,int supported)
{
 name=label;status=supported;queries=0;last_run_limit=0;query_contract_bad=0;
 const char *css="body{margin:21px;font-size:14px}input,select,textarea,button{display:block;width:160px;height:16px;box-sizing:border-box;padding:0;border:0}";
 struct node *root=dom_parse(markup,strlen(markup));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 struct node *n=dom_get_element_by_id(root->doc,"f");int x=0,y=0,w=0,h=0;layout_node_box(n,&x,&y,&w,&h);
 if(value)fc_set_value(n,text,strlen(text));if(selected){focus_set(n);fc_set_selection(n,0,strlen(text));}
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);
 struct logit_text_metrics m=oracle(text,strlen(text));int expected=supported==1&&m.has_ink?best_y(y,h,m.ink_top,m.ink_bottom):y+1;
 int runs=0,bytes=0,selection=0,caret=0,cx=0,cy=0,cw=0,ch=0;
 for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if(p->kind==OP_CLIP){cx=p->x;cy=p->y;cw=p->w;ch=p->h;}
  if(p->kind==OP_TEXT){runs++;bytes+=p->len;EXPECT(p->y==expected,"native draw uses expected complete-run ink origin");EXPECT(p->px==14,"draw preserves font size");EXPECT(cy==y&&ch==h,"content clip stays unchanged");if(supported==1&&m.has_ink)EXPECT(S(p->y)+m.ink_top>=S(cy)&&S(p->y)+m.ink_bottom<=S(cy+ch),"ordinary ink fits existing device clip");}
  if(p->kind==OP_RECT&&p->color==0xb4d5fe){selection++;if(m.has_ink&&supported==1)EXPECT(S(p->y)<=S(expected)+m.ink_top&&S(p->y+p->h)>=S(expected)+m.ink_bottom,"selection covers positioned ink");}
  if(p->kind==OP_RECT&&p->w==1){caret++;if(!strcmp(text,".")||!strcmp(text,"-"))EXPECT(p->h>=14,"punctuation retains ordinary caret height");if(m.has_ink&&supported==1)EXPECT(S(p->y)<=S(expected)+m.ink_top&&S(p->y+p->h)>=S(expected)+m.ink_bottom,"caret covers positioned ink");}
 }
 int nbytes=strlen(text),want_runs=(nbytes+255)/256;
 EXPECT(bytes==nbytes&&runs==want_runs,"all finite text chunks draw once");
 EXPECT(queries==(supported==1?want_runs:(nbytes?1:0)),"query count follows actual emitted run boundaries");
 EXPECT(last_run_limit<=256,"queries preserve bounded native UTF8 run size");
 EXPECT(!query_contract_bad,"query preserves logical font size and actual native face");
 if(selected){EXPECT(caret==1,"one focused caret remains visible");if(nbytes)EXPECT(selection==1,"one selected short value remains visible");}
 fc_reset();focus_reset();layout_free();dom_free(root);
}
static void separate_paths(void)
{
 name="textarea-button-separate";queries=0;status=1;
 const char *html="<textarea id=t>Hg\nHg</textarea><button id=b>Child</button>";
 const char *css="body{font-size:14px}textarea,button{display:block;padding:0;border:0;width:160px;height:40px}";
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);int hg=0,child=0;
 for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if(p->kind!=OP_TEXT)continue;if(p->len==2&&!memcmp(p->text,"Hg",2))hg++;if(p->len==5&&!memcmp(p->text,"Child",5))child++;}
 EXPECT(queries==0,"textarea and child-content button do not query single-line ink");EXPECT(hg==2,"textarea keeps two ordinary lines");EXPECT(child==1,"button child label not duplicated");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
int main(void)
{
 arithmetic();
 for(int j=0;j<3;j++){scale=100+j*50;
  sample("placeholder-Hg","<input id=f placeholder=Hg>","Hg",0,0,1);
  sample("value-Hg","<input id=f>","Hg",1,1,1);
  sample("placeholder-ASCII","<input id=f placeholder=H>","H",0,0,1);
  sample("placeholder-CJK","<input id=f placeholder=字>","字",0,0,1);
  sample("select-Hg","<select id=f><option>Hg</option></select>","Hg",0,0,1);
  sample("native-input-button-Hg","<input id=f type=submit value=Hg>","Hg",0,0,1);
  sample("period-value","<input id=f>",".",1,1,1);
  sample("dash-value","<input id=f>","-",1,1,1);
  sample("empty-fallback","<input id=f>","",1,1,1);
  sample("spaces-fallback","<input id=f>","   ",1,1,1);
  sample("unsupported-fallback","<input id=f placeholder=Hg>","Hg",0,0,0);
  sample("unavailable-fallback","<input id=f placeholder=Hg>","Hg",0,0,-1);
 }
 scale=100;char text[1026];memset(text,'H',1024);text[1024]='g';text[1025]=0;sample("all-five-native-runs","<input id=f>",text,1,0,1);
 separate_paths();
 printf("control-text-ink: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
