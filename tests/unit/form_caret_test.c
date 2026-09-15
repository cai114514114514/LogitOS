/* Reuse the real parser/layout/painter fixture; its old byte-width stand-in
 * is renamed below. These metrics model the native syscall's documented
 * length refusal and unequal UTF-8 glyph advances, not actual font pixels.
 * Measured with this stub: 1600 ASCII bytes gave old width=0/new width=12000;
 * a shorter overflowing value put the old caret at exclusive x=80, new x=78.
 * OP_RECT checks below additionally watch the actual painter discard/draw it.
 * Guest font rounding and physical keyboard delivery need a guest run. */
#define main modal_caret_unused_main
#define text_measure modal_caret_unused_measure
#include "modal_paint_test.c"
#undef text_measure
#undef main
#include "forms.h"
static int calls, oversized;
int text_measure(const char *s,int len,int px,int face){
 (void)px;(void)face;calls++;
 if(len>1024){oversized++;return 0;}
 int width=0;for(int p=0;p<len;){unsigned char c=s[p];int n=1;
  if(c>=0xf0)n=4;else if(c>=0xe0)n=3;else if(c>=0xc0)n=2;
  width+=(c==0xe2&&p+2<len&&(unsigned char)s[p+1]==0x80&&(unsigned char)s[p+2]==0xa2)?8:(c>=128?16:(c=='W'?11:(c=='i'?4:8)));
  p+=n;
 }return width;
}
static void one(const char *pattern,int repeats,const char *label){
 char value[6000];int len=0,pl=strlen(pattern);for(int i=0;i<repeats;i++){memcpy(value+len,pattern,pl);len+=pl;}value[len]=0;
 const char *html="<!doctype html><input id=f>";
 struct node *root=dom_parse(html,strlen(html));struct node *n=dom_get_element_by_id(root->doc,"f");
 CHECK(n!=0,"parser fixture contains the measured input");if(!n){dom_free(root);return;}
 const char *css="body{margin:0}input{width:160px;height:28px;font-size:16px}";
 css_viewport(500,300);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,500);fc_set_value(n,value,len);focus_set(n);
 struct fpaint fp;oversized=0;fc_paint_state(n,16,0,80,&fp);
 printf("caret specimen %s bytes=%d width=%d scroll=%d caret=%d\n",label,len,fp.text_w,fp.scroll_x,fp.caret_x);
 CHECK(fp.text_w>0&&!oversized,"long value uses bounded native measurement calls");
 CHECK(fp.caret_x-fp.scroll_x>=0&&fp.caret_x-fp.scroll_x<80,"end caret lies inside the exclusive content clip");
 int off=fc_offset_at_px(n,fp.caret_x-fp.scroll_x,16,0);CHECK(off==len,"scrolled end hit resolves to the complete UTF-8 value");
 fc_edit_move(n,-1,0,0);int a,b;fc_selection(n,&a,&b);int prev=len-1;while(prev>0&&((unsigned char)value[prev]&0xc0)==0x80)prev--;
 CHECK(a==prev&&b==prev,"arrow during long input moves one UTF-8 scalar");
 fc_paint_state(n,16,0,80,&fp);CHECK(fc_offset_at_px(n,fp.caret_x-fp.scroll_x,16,0)==prev,"moved caret and mouse hit share scrolled geometry");
 CHECK(fc_edit_insert(n,"Z",1),"mid-string insertion changes real control value");fc_selection(n,&a,&b);CHECK(a==prev+1,"insertion advances caret at edit location");
 fc_edit_home(n,0);fc_paint_state(n,16,0,80,&fp);CHECK(fp.scroll_x==0&&fp.caret_x==0,"Home clears accumulated inner scroll");
 fc_edit_end(n,0);paint_nops=0;browser_paint_scroll(0,0,500,300,0,0);
 int bytes=0,last_text=-1,paint_value_len=0;const char *paint_value=fc_value(n,&paint_value_len);
 for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT){
  struct paintop *op=&paint_ops[i];
  CHECK(op->len<=1023,"each painted text run fits the real GUI syscall");
  CHECK(bytes+op->len<=paint_value_len&&!memcmp(op->text,paint_value+bytes,op->len),"painted chunks preserve complete value byte order");
  CHECK(!((unsigned char)op->text[0]>=128&&((unsigned char)op->text[0]&0xc0)==0x80),"painted chunk starts on a UTF-8 scalar boundary");
  bytes+=op->len;last_text=i;
 }
 CHECK(bytes==len+1,"all long control bytes reach the actual painter");
 /* Observe the actual clipped paint primitive, not only fpaint arithmetic:
  * the old clamp let fill() discard the caret before any GUI call was made. */
 int drawn_caret=0;
 if(last_text>=0){struct paintop *last=&paint_ops[last_text];
  int end_x=last->x+text_measure(last->text,last->len,last->px,last->mono);
  for(int i=last_text+1;i<paint_nops;i++)if(paint_ops[i].kind==OP_RECT&&paint_ops[i].w==1&&paint_ops[i].h>=16&&paint_ops[i].x==end_x&&paint_ops[i].color==last->color)drawn_caret++;
 }
 CHECK(drawn_caret==1,"actual painter emits one visible caret at the final glyph edge");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
int main(void){
 one("Wi",80,"ASCII");one("中文",50,"CJK");one("Wi中文",40,"mixed");
 one("Wi",800,"long ASCII");one("中文",300,"long CJK");one("Wi中文",200,"long mixed");
 const char *html="<!doctype html><input id=p type=password>";
 struct node *root=dom_parse(html,strlen(html));struct node *n=dom_get_element_by_id(root->doc,"p");fc_set_value(n,"WWWiii中文",12);focus_set(n);fc_set_selection(n,0,0);struct fpaint fp;fc_paint_state(n,16,0,160,&fp);
 CHECK(fc_offset_at_px(n,16,16,0)==2,"password mouse placement measures displayed bullets, not secret glyphs");
 char secret[1401];for(int i=0;i<1400;i++)secret[i]='W';secret[1400]=0;
 fc_set_value(n,secret,1400);fc_edit_end(n,0);fc_paint_state(n,16,0,80,&fp);
 CHECK(fp.len==4200,"password mask preserves characters beyond the old 256-character cap");
 CHECK(fp.caret_x-fp.scroll_x>=0&&fp.caret_x-fp.scroll_x<80,"long password end caret remains visible");
 CHECK(fc_offset_at_px(n,fp.caret_x-fp.scroll_x,16,0)==1400,"long masked caret hit maps to the last real value byte");
 fc_set_selection(n,300,300);fc_paint_state(n,16,0,80,&fp);
 CHECK(fc_offset_at_px(n,fp.caret_x-fp.scroll_x,16,0)==300,"password middle hit remains distinct past character 256");
 fc_reset();focus_reset();dom_free(root);puts(fail?"form-caret: FAIL":"form-caret: PASS");return fail?1:0;
}
