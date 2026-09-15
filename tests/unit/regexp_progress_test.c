/* RepeatMatcher progress policy, without executing an unbounded negative
 * control. All runtime cases have at most three repetitions; the compiler
 * checks verify the protective edge directly on both builds. */
#define main syntax_existing_main
#include "js_syntax_test.c"
#undef main
#include "cutils.h"
#include "libregexp.h"
enum {
#define DEF(id, size) OP_ ## id,
#include "libregexp-opcode.h"
#undef DEF
    OP_COUNT
};
static const int sizes[] = {
#define DEF(id, size) size,
#include "libregexp-opcode.h"
#undef DEF
};
static void guard(JSContext *ctx, const char *pattern, int expected)
{
    char error[256]; int len=0, found=0;
    uint8_t *bc=lre_compile(&len,error,sizeof(error),pattern,strlen(pattern),0,ctx);
    checks++;
    if (!bc) { fail("progress policy compiles",error); return; }
    /* The seven-byte header precedes a length-delimited opcode stream. */
    int end=7+get_u32(bc+3), pos=7;
    while (pos<end && end<=len) {
        int op=bc[pos], size;
        if (op>=OP_COUNT) break;
        size=sizes[op];
        if (pos+size>end) break;
        if (op==OP_range) size+=4*get_u16(bc+pos+1);
        if (op==OP_range32) size+=8*get_u16(bc+pos+1);
        found+=op==OP_check_advance;
        pos+=size;
    }
    if (pos!=end || !!found!=expected)
        fail("nullable repetition has progress guard",pattern);
    lre_realloc(ctx,bc,0);
}
int main(void)
{
    JSRuntime *rt=JS_NewRuntime(); JSContext *ctx=JS_NewContext(rt);
    guard(ctx,"(a?){0,3}?",1);
    guard(ctx,"(a|){0,3}?",1);
    guard(ctx,"(a?){1,3}?",1);
    guard(ctx,"(a?){2,3}?",1);
    guard(ctx,"(a?){0,3}",1);
    guard(ctx,"(a){0,3}?",0);
    guard(ctx,"[ab]{0,3}?",0);
    expect_value(ctx,"lazy consuming capture","JSON.stringify(/^(a?){0,3}?b$/.exec('aab'))","[\"aab\",\"a\"]");
    expect_value(ctx,"lazy zero repetitions reset capture","JSON.stringify(/^(a?){0,3}?b$/.exec('b'))","[\"b\",null]");
    expect_value(ctx,"mandatory empty capture retained","JSON.stringify(/^(a?){1,3}?b$/.exec('b'))","[\"b\",\"\"]");
    expect_value(ctx,"mandatory multiple empty captures","JSON.stringify(/^(a?){2,3}?b$/.exec('b'))","[\"b\",\"\"]");
    expect_value(ctx,"lazy literal consumption unchanged","/^(a){0,3}?b$/.test('aaab')","true");
    expect_value(ctx,"repetition bound still enforced","/^(a){0,3}?b$/.test('aaaab')","false");
    expect_value(ctx,"greedy capture unchanged","JSON.stringify(/^(a?){0,3}b$/.exec('aab'))","[\"aab\",\"a\"]");
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf("regexp-progress: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
