/* Production matchMedia and cascade, with output device intentionally larger
 * than the viewport. The bounded search is the observed consumer: with the old
 * always-false device feature its step never shrinks and the script hangs. */
#define main platform_existing_main
#include "webapi_platform_test.c"
#undef main
#include "css.h"
#include "js_webapi.h"

int main(void)
{
    css_set_screen(1920,1200); css_viewport(800,600); css_init();
    struct node *root=dom_parse(PAGE,(int)strlen(PAGE)); if(!root)return 2;
    const char *sheet="#wrap{color:#ff0000}@media(min-device-width:1500px){#wrap{color:#00ff00}}";
    css_apply(root,sheet,(int)strlen(sheet));
    struct node *n=dom_get_element_by_id_in(root,"wrap");
    ck(n && n->style && (((struct cstyle*)n->style)->color&0xffffff)==0x00ff00,"device media query selects actual stylesheet rule");
    js_page_set_clock(clock_fn); js_page_set_location("https://device-media.example/");
    js_webapi_set_viewport(800,600);
    if(!js_page_open(root))return 2; ctx=js_page_ctx();
    ckjs("screen.width===1920 && screen.height===1200", "screen metrics come from output device");
    ckjs("matchMedia('(min-device-width: 1px)').matches && matchMedia('(max-device-width: 1920px)').matches && !matchMedia('(min-device-width: 1921px)').matches", "device width range uses measured dimensions");
    ckjs("matchMedia('(device-height:1200px)').matches && !matchMedia('(device-height:600px)').matches", "device height differs from viewport height");
    ckjs("matchMedia('(device-width)').matches && matchMedia('(device-height)').matches", "boolean device dimensions are nonzero");
    run("function probeDevice(feature){var size=10000,step=2000,calls=0;while(step>0 && ++calls<100){if(matchMedia('('+feature+': '+(size+1)+'px)').matches){size+=step;step=Math.floor(step/2);}else size-=step;}return [size,calls,step];} var measured=probeDevice('min-device-width');");
    ckjs("measured[0]===1920 && measured[1]<100 && measured[2]===0", "real device-width search converges instead of spinning");
    css_viewport(320,200);js_webapi_set_viewport(320,200);js_webapi_pump(ctx);
    ckjs("screen.width===1920 && screen.height===1200 && matchMedia('(device-width:1920px)').matches && matchMedia('(width:320px)').matches", "viewport resize preserves device dimensions");
    css_set_screen(2560,1600);
    ckjs("screen.width===2560 && screen.height===1600 && matchMedia('(device-width:2560px)').matches", "screen getters and media read one current authority");
    css_set_reduced_motion(0);
    ckjs("matchMedia('(prefers-reduced-motion:no-preference)').matches && !matchMedia('(prefers-reduced-motion)').matches", "default motion preference matches CSS boolean semantics");
    run("var changes=0, motion=matchMedia('(prefers-reduced-motion:reduce)'); motion.addEventListener('change',function(e){if(e.matches)changes++;});");
    css_set_reduced_motion(1); js_webapi_media_changed(); js_webapi_pump(ctx);
    ckjs("motion.matches && changes===1 && matchMedia('(prefers-reduced-motion)').matches", "motion preference updates existing MQL and fires change once");
    sheet="#wrap{color:#ff0000}@media(prefers-reduced-motion:reduce){#wrap{color:#0000ff}}";
    css_apply(root,sheet,(int)strlen(sheet));
    ck(n && n->style && (((struct cstyle*)n->style)->color&0xffffff)==0x0000ff,"motion media query selects actual stylesheet rule");
    ckjs("!matchMedia('(prefers-reduced-motion:unknown)').matches && !matchMedia('(prefers-reduced-motion:no-preference)').matches", "motion preference rejects unknown and opposite value");
    ckjs("matchMedia('(prefers-reduced-motion: REDUCE)').matches", "motion keyword matching is ASCII case insensitive");
    js_page_close();ctx=NULL;dom_free(root);
    printf("device media: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
