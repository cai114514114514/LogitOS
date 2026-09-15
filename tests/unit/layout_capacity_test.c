/* Long ordinary documents must retain their tail, including inkless boxes.
 * An allocator refusal must discard a partial list rather than publish it. */
#define main layout_box_original_main
#define kmalloc layout_fixture_malloc
#include "layout_box_test.c"
#undef kmalloc
#undef main
static unsigned long refuse_above;
static int refused;
void layout_images_reset(void);
void *kmalloc(unsigned long n)
{
    if(refuse_above && n>refuse_above){refused++;return 0;}
    return layout_fixture_malloc(n);
}
static void release_page(void)
{
    layout_free();layout_images_reset();dom_free(g_root);g_root=0;
}
static void long_text(void)
{
    int words=22000;
    char *h=malloc((size_t)words*5+256); strcpy(h,"<body><p>");
    for(int i=0;i<words;i++)strcat(h,"word ");
    strcat(h,"TAIL</p></body>");page(h,800);free(h);
    int found=0,n=layout_count();const struct item *list=layout_items();
    for(int i=0;i<n;i++)if(list[i].type==IT_TEXT&&list[i].len==4&&!memcmp(list[i].text,"TAIL",4))found++;
    CHECK(found==1,"long text tail retained beyond old display limit");
    CHECK(n>16384,"long text publishes more than the former fixed limit");
    int height=layout_height();layout_page(g_root,800);
    CHECK(layout_count()==n&&layout_height()==height,"second full layout keeps stable tail and geometry");
    release_page();
}
static void long_boxes(void)
{
    int count=18000;
    char *h=malloc((size_t)count*11+512);strcpy(h,"<style>div{width:17px;height:1px}body{margin:0}</style><body>");
    for(int i=0;i<count;i++)strcat(h,"<div></div>");
    strcat(h,"<div id=tail></div></body>");page(h,800);free(h);
    int x=0,y=0,w=0,hh=0;
    CHECK(layout_node_box(ID("tail"),&x,&y,&w,&hh)&&y==count&&w==17&&hh==1,
          "inkless tail box retains real geometry beyond old box limit");
    CHECK(layout_height()==count+1,"scroll extent includes the entire long document");
    /* Refuse the retained grown arena on the next pass, without starving the
     * DOM or changing the document. This is a local allocation-failure seam. */
    refuse_above=sizeof(struct item)*16384UL;refused=0;
    layout_page(g_root,800);refuse_above=0;
    CHECK(refused>0&&layout_items()==0&&layout_count()==0,"arena allocation failure never exposes a partial display list");
    layout_page(g_root,800);
    CHECK(layout_node_box(ID("tail"),&x,&y,&w,&hh)&&y==count,"layout recovers after allocation pressure clears");
    release_page();
}
int main(void)
{
    css_init();css_viewport(800,600);long_text();long_boxes();
    printf("layout-capacity: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
