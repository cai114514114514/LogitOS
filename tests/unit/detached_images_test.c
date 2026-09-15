/* SPDX-License-Identifier: MIT
 * Loader fixtures normally stub ALL codecs to failure. Rename those four
 * stand-ins before including the apparatus; this gate links IMGCHK_SRC's
 * actual registry/SVG decoder so load and naturalWidth require real pixels. */
#define main loader_existing_main
#define img_decode loader_unused_img_decode
#define img_free loader_unused_img_free
#define img_init loader_unused_img_init
#include "loader_test.c"
#undef main
#undef img_decode
#undef img_free
#undef img_init
void app_main(void);
static int polls,finished;
static int expr(const char *s){JSContext *ctx=js_page_ctx();if(!ctx)return 0;
 JSValue v=JS_Eval(ctx,s,strlen(s),"<image-observer>",JS_EVAL_TYPE_GLOBAL);int ok=0;
 if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else ok=JS_ToBool(ctx,v);
 JS_FreeValue(ctx,v);return ok;}
void loader_poll_hook(void){
 host_clock+=50;if(finished)return;
 if(++polls<1500&&!expr("document.getElementById('result')&&document.getElementById('result').textContent.indexOf('DETACHED-IMAGES checks=')===0"))return;
 finished=1;
 CHECK(expr("checks===12&&failures===0"),"detached image lifecycle passes shared page assertions");
 CHECK(fake_site_fetched("/image.svg")>0&&fake_site_fetched("/corrupt.svg")>0,"image preloads actually reached transport");
 int decoded=0;const struct item *items=layout_items();for(int i=0;i<layout_count();i++)if(items[i].type==IT_IMAGE&&items[i].img&&items[i].img->w==24&&items[i].img->h==16)decoded++;
 CHECK(decoded>0,"preloaded pixels reached an actual display-list image");
 struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
int main(void){
 FILE *f=fopen("tests/fixtures/browser/detached-images.html","rb");if(!f)return 2;
 fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);char *page=malloc((size_t)len+1);
 if(!page||fread(page,1,(size_t)len,f)!=(size_t)len)return 2;fclose(f);page[len]=0;
 fake_site_reset();fake_site_add("http://fixture.test/images.html",page);
 fake_site_add("http://fixture.test/image.svg","<svg xmlns='http://www.w3.org/2000/svg' width='24' height='16'><rect width='24' height='16' fill='#2458da'/></svg>");
 fake_site_add("http://fixture.test/corrupt.svg","Not an image");
 tabs_set_store(&memfs);const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/images.html\timages\t0\n";memfs_write(SESSION_PATH,session,strlen(session));
 struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
 if(setjmp(host_exit_jmp)==0)app_main();
 CHECK(finished,"image real app_main reached observations");free(page);
 puts(fail?"detached-images: FAIL":"detached-images: PASS");return fail?1:0;
}
