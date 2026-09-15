/* Auto native button geometry must reserve its computed content and edges.
 * A 13px ten-character label has a 60px deterministic glyph advance. The old
 * 82x24 chrome stayed unchanged after 6px/12px padding, making two lines that
 * ended 23px below the button. Explicit dimensions are independent controls. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main

static int is_descendant(struct node *n, struct node *p)
{ for (; n; n=n->parent) if (n==p) return 1; return 0; }

static void scene(const char *host, const char *button_rule)
{
    char html[2048];
    snprintf(html,sizeof html,
        "<style>body{margin:0;font-size:13px;line-height:20px}"
        "#host{width:500px;%s}button{font-size:13px;line-height:20px;"
        "box-sizing:border-box;%s}</style>"
        "<div id=host><button id=b>Alpha Beta</button></div>",host,button_rule);
    page(html,600);
}

static void finish(void)
{ layout_free(); dom_free(g_root); g_root=0; }

static void contained_label(void)
{
    int b[4],first=-1,last=-1,count=0;
    struct node *n=ID("b");
    CHECK(layout_node_box(n,b,b+1,b+2,b+3),"auto button box exists");
    struct cstyle *s=n->style;
    const struct item *list=layout_items();
    for (int i=0;i<layout_count();i++) {
        const struct item *it=&list[i];
        if (it->type!=IT_TEXT || !is_descendant(it->node,n)) continue;
        if (first<0) first=it->y;
        last=it->y; count++;
        CHECK(it->x>=b[0]+s->pl+s->border_w[3] &&
              it->x+it->w<=b[0]+b[2]-s->pr-s->border_w[1],
              "auto button label fits horizontal content box");
        CHECK(it->y>=b[1]+s->pt+s->border_w[0] &&
              it->y+it->h<=b[1]+b[3]-s->pb-s->border_w[2],
              "auto button label fits vertical content box");
    }
    CHECK(count>0,"auto button has real text items");
    EQ(last,first,"auto button label remains one line");
}

int main(void)
{
    css_init(); css_viewport(600,400);
    const char *hosts[]={"","display:flex","display:grid;grid-template-columns:max-content"};
    for(int i=0;i<3;i++) {
        scene(hosts[i],"display:flex;border:1px solid;padding:6px 12px");
        EQ(width("b"),86,"auto button includes authored horizontal edges");
        EQ(height("b"),34,"auto button includes authored line-height and vertical edges");
        contained_label(); finish();
    }
    scene("display:flex","display:flex;border:1px solid;padding-inline:12px;padding-block:6px");
    EQ(width("b"),86,"logical padding participates in native auto width");
    EQ(height("b"),34,"logical padding participates in native auto height");
    contained_label(); finish();
    scene("display:flex","display:flex;border:2px solid;padding:3px 17px 9px 11px;line-height:28px");
    EQ(width("b"),92,"asymmetric padding and border count exactly once");
    EQ(height("b"),44,"authored line-height uses the same oracle as text placement");
    contained_label(); finish();
    /* No author padding: retain existing UA native minimum dimensions. */
    scene("display:flex","line-height:normal");
    EQ(width("b"),82,"UA auto button default width is unchanged");
    EQ(height("b"),24,"UA auto button default height is unchanged");
    contained_label(); finish();
    scene("display:flex","display:flex;border:1px solid;padding:0;line-height:40px");
    EQ(height("b"),42,"line-height alone enlarges auto native height");
    contained_label(); finish();
    scene("display:flex","display:flex;border:1px solid;padding:6px 12px;width:86px;height:34px");
    EQ(width("b"),86,"explicit border-box width is unchanged");
    EQ(height("b"),34,"explicit border-box height is unchanged");
    contained_label(); finish();
    scene("display:flex","display:flex;border:1px solid;padding:6px 12px;width:60px;height:20px;box-sizing:content-box");
    EQ(width("b"),86,"explicit content-box width adds edges once");
    EQ(height("b"),34,"explicit content-box height adds edges once");
    contained_label(); finish();
    scene("display:flex","display:flex;border:1px solid;padding:6px 12px;width:50px;height:18px");
    EQ(width("b"),50,"small explicit width is not expanded by auto floor");
    EQ(height("b"),18,"small explicit height is not expanded by auto floor");
    finish();
    scene("display:flex","display:flex;border:1px solid;padding:6px 12px;min-width:120px;max-width:140px");
    EQ(width("b"),120,"author min-width still follows auto metrics");
    contained_label(); finish();
    printf("button-auto-metrics: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
