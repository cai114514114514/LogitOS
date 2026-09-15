/* The ordinary cascade, extension cascade and variable pre-pass must agree
 * with the query API. Matching only '(min-width:...)' cannot exercise the
 * range parser that modern responsive menus and picture sources use. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static void query(const char *q,int expected)
{
 int actual=css_media_matches(q,-1);checks++;
 if(actual!=expected){printf("FAIL range %s: got %d want %d\n",q,actual,expected);fails++;}
}
int main(void)
{
 css_init();css_viewport(1126,562);
 const char *ops[]={"<","<=","=",">=",">"};
 for(int dim=0;dim<2;dim++)for(int d=-1;d<=1;d++)for(int o=0;o<5;o++){
  int basis=dim?562:1126,v=basis+d;char q[128];
  int truth=o==0?basis<v:o==1?basis<=v:o==2?basis==v:o==3?basis>=v:basis>v;
  snprintf(q,sizeof q,"(%s%s%dpx)",dim?"height":"width",ops[o],v);query(q,truth);
  truth=o==0?v<basis:o==1?v<=basis:o==2?v==basis:o==3?v>=basis:v>basis;
  snprintf(q,sizeof q,"(%dpx %s %s)",v,ops[o],dim?"height":"width");query(q,truth);
 }
 query("(1012px <= width <= 1279px)",1);query("(1279px >= width >= 1012px)",1);
 query("(1126px < width <= 1279px)",0);query("(1126px <= width < 1279px)",1);
 query("(1279px > width > 1126px)",0);query("(1279px > width >= 1126px)",1);
 query("(width>=63.25rem)",1);query("(width>=80em)",0);
 query("(width <= 1125.99px)",0);query("(width > 1125.99px)",1);
 query("screen and (width>=1012px) and (width<=1279px)",1);
 query("(width>2000px), (height<=562px)",1);
 query("(min-width:1126px)",1);query("(max-width:1126px)",1);
 query("(width > = 1000px)",0);query("(1000px < = width)",0);
 query("(1000px < width > 1500px)",0);query("(width == 1126px)",0);
 query("(1126px <= width <=)",0);query("(width>=1012px) garbage",0);
 const char *html="<!doctype html><style>body{margin:0}:root{--distance:7px}#n{display:block;gap:3px}"
  "@media(width>=1012px){:root{--distance:19px}#n{display:flex;gap:11px;transform:translateX(30px)}}"
  "@media(width<1012px){#n{transform:translateX(5px)}}#n{padding-left:var(--distance)}</style><div id=n>Responsive</div>";
 page(html,1126);struct cstyle *st=ID("n")->style;
 EQ(st->display,DISP_FLEX,"range selects actual desktop display declaration");
 EQ(st->grid_gap_x,11,"range selects extension declaration");
 EQ(st->pl,19,"range selects custom property before expansion");
 CHECK(st->xraw[XR_TRANSFORM]&&st->xrawlen[XR_TRANSFORM]==16&&!memcmp(st->xraw[XR_TRANSFORM],"translateX(30px)",16),"range selector does not leak narrow extension");
 dom_free(g_root);g_root=NULL;css_viewport(800,562);page(html,800);st=ID("n")->style;
 EQ(st->display,DISP_BLOCK,"resize restores narrow display rule");EQ(st->pl,7,"resize restores narrow variable winner");
 CHECK(st->xraw[XR_TRANSFORM]&&st->xrawlen[XR_TRANSFORM]==15&&!memcmp(st->xraw[XR_TRANSFORM],"translateX(5px)",15),"resize selects narrow transform");
 dom_free(g_root);g_root=NULL;
 printf("media-range: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
