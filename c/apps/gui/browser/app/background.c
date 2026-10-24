/* CSS bitmap backgrounds share the <img> fetch table, decoder, negative cache
 * and tab lifetime. A second downloader would refetch every restyle and lose
 * in-flight cancellation when an element changes its background. Discovery
 * runs after layout; the painter only reads cached pixels and never does IO. */
#include "css/background.h"
extern int css_background_url(const char *,int,char *,int) LOGIT_WEAK;
extern int css_background_layers(const struct cstyle *,struct bg_layers *) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_background_url);
LOGIT_WEAK_STUB(css_background_layers);

static int background_sources(const struct item *item,struct bg_layers *layers)
{
    const struct cstyle *style=item->generated_style?item->generated_style:
                              item->node?item->node->style:0;
    if(item->type!=IT_RECT || !style || style->mask_refused || !style->xraw[XR_BG_IMAGE] ||
       !LOGIT_HAVE(css_background_url)||!LOGIT_HAVE(css_background_layers))return 0;
    return css_background_layers(style,layers);
}
static int background_images_wants(const char *src)
{
    const struct item *items=layout_items();char url[4096];struct bg_layers layers;
    for(int i=0;i<layout_count();i++)if(background_sources(&items[i],&layers))
        for(int j=0;j<layers.count;j++)if(css_background_url(layers.layer[j].image.value,layers.layer[j].image.len,url,sizeof url)&&!strcmp(url,src))return 1;
    return 0;
}
static int background_images_pending(void)
{
    if(!layout_img_can_load())return 0;
    const struct item *items=layout_items();char url[4096];struct bg_layers layers;
    for(int i=0;i<layout_count();i++)if(background_sources(&items[i],&layers))
        for(int j=0;j<layers.count;j++)if(css_background_url(layers.layer[j].image.value,layers.layer[j].image.len,url,sizeof url)&&!layout_img_cached(url))return 1;
    return 0;
}
static int background_images_load(int budget,int (*fetch)(const char *,unsigned char **,int *))
{
#ifdef CSS_BACKGROUND_NO_LOAD
    (void)budget;(void)fetch;return 0;
#else
    int loaded=0,started=0;const struct item *items=layout_items();char url[4096];struct bg_layers layers;
    for(int i=0;i<layout_count();i++){
        if(!layout_img_can_load())break;
        if(!background_sources(&items[i],&layers))continue;
        /* Later layers remain desires even while an earlier URL is pending.
         * Looking only at layer zero would cancel the lower image exactly when
         * the first asynchronous request appeared to start successfully. */
        for(int j=0;j<layers.count;j++){
            if(!layout_img_can_load()||started>=budget)return loaded;
            if(!css_background_url(layers.layer[j].image.value,layers.layer[j].image.len,url,sizeof url)||layout_img_cached(url))continue;
            started++;unsigned char *bytes=0;int len=0;int rc=fetch(url,&bytes,&len);
            if(rc>0)continue;             /* pending is not a broken image */
            int stored=layout_img_store(url,rc==0?bytes:0,rc==0?len:0);
            int width=0,height=0,state=layout_img_dimensions(url,&width,&height);
            printf("[img-background] fetch=%d bytes=%d prefix=%02x%02x decoded=%d state=%d size=%dx%d key=%.180s\n",
                   rc,len,bytes&&len>0?bytes[0]:0,bytes&&len>1?bytes[1]:0,stored,state,width,height,url);
            loaded+=stored;free(bytes);
        }
    }
    return loaded;
#endif
}
