/* Same shipping installers as the native/JS select-state gate. The fixture is
 * also served unchanged to the guest; no copied FileList implementation and
 * no file picker, host file access, or invented DataTransfer source. */
#define main input_files_existing_main
#include "select_state_test.c"
#undef main

int main(void)
{
    const char *path="tests/fixtures/engine-expansion/input-files.html";
    FILE *fp=fopen(path,"rb");
    if (!fp) return 2;
    fseek(fp,0,SEEK_END); long n=ftell(fp); rewind(fp);
    if (n<=0) { fclose(fp); return 2; }
    char *html=malloc((size_t)n+1);
    if (!html) { fclose(fp); return 2; }
    size_t got=fread(html,1,(size_t)n,fp); fclose(fp);
    if (got!=(size_t)n) { free(html); return 2; }
    html[n]=0;
    struct node *root=dom_parse(html,(int)n);
    js_page_set_location("http://fixture.test/input-files");
    if (!root || !js_page_open(root)) { if(root)dom_free(root);free(html);return 2; }
    char *src=strstr(html,"<script>"), *end=src?strstr(src,"</script>"):NULL;
    if (!end) { js_page_close();dom_free(root);free(html);return 2; }
    /* QuickJS requires a NUL at input_len too; a slice into HTML otherwise
     * lets the lexer see the closing </script> and falsely reports a product
     * SyntaxError. The DOM was already parsed before terminating this copy. */
    src+=8;*end=0;js_page_eval(src,(int)(end-src),"<input-files-fixture>",0);
    ck("all fixture checks completed", "fileChecks===23&&fileFailures===0");
    ck("render after strict binding", "document.getElementById('result').textContent==='INPUT-FILES PASS checks=23 failures=0'");
    /* Host click covers callback semantics only; the guest runner separately
     * drives QMP pointer events at the actual laid-out button rectangle. */
    ck("binding callback remains callable", "document.getElementById('check').click();document.getElementById('result').textContent==='INPUT-FILES-CLICK PASS'");
    struct node *field=dom_get_element_by_id(root->doc,"f");
    int vl=-1; const char *v=fc_value(field,&vl);
    checks++; if(vl!=0||!v||*v){puts("FAIL native file value ignores markup");failures++;}
    checks++; if(fc_set_value(field,"invented",8)){puts("FAIL native file value refuses invented path");failures++;}
    paint_label("native file label ignores markup",field,"Choose File");
    js_page_close();fc_reset();dom_free(root);free(html);
    printf("input-files: %d host assertions, %d failures (23 shared fixture checks)\n",checks,failures);
    return failures ? 1 : 0;
}
