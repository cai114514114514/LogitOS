#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pinyin.h"
static int checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", label); } else printf("ok: %s\n",label); } while (0)
static void type(struct ime_state *s, const struct ime_dict *d, const char *p) {
    ime_reset(s, d); while (*p) ime_feed(s, *p++);
}
static int equal(const uint32_t *p, int n, const char *word) {
    char b[256]; int at=0;
    for(int i=0;i<n;i++) { unsigned c=p[i];
        if(c<128) b[at++]=c;
        else if(c<2048) {b[at++]=192|(c>>6);b[at++]=128|(c&63);}
        else {b[at++]=224|(c>>12);b[at++]=128|((c>>6)&63);b[at++]=128|(c&63);}
    } b[at]=0; return !strcmp(b,word);
}
static int find(struct ime_state *s,const char *word) {
    for(int i=0;i<s->ncand;i++) if(equal(s->cand[i].cp,s->cand[i].ncp,word)) return i;
    return -1;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb"); if(!f)return 2; fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
    unsigned char *blob=malloc(len); if(!blob||fread(blob,1,len,f)!=(size_t)len)return 2; fclose(f);
    const struct ime_dict *d=ime_open(blob,len); CHECK(d!=NULL,"real dictionary opens"); if(!d)return 1;
    struct ime_state s;
    const char *cases[][2]={{"woaizhongguo","我爱中国"},{"jintiantianqihenhao","今天天气很好"},
        {"womenquchifan","我们去吃饭"},{"shurufa","输入法"},{"xiexieni","谢谢你"},
        {"xi'an","西安"},{"ni'","你"}};
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++) {
        type(&s,d,cases[i][0]); CHECK(s.ncand && equal(s.cand[0].cp,s.cand[0].ncp,cases[i][1]),cases[i][0]);
    }
    type(&s,d,"nihaom"); int idx=find(&s,"你好"); CHECK(idx>=0,"unfinished suffix keeps a selectable word");
    uint32_t out[IME_MAX_RAW];
    if(idx>=0) {
        s.page=idx/IME_PAGE_SIZE;
        int n=ime_accept(&s,idx%IME_PAGE_SIZE,out,IME_MAX_RAW);
        CHECK(equal(out,n,"你好")&&s.raw_len==1&&s.raw[0]=='m',"choosing 你好 leaves m editable");
        ime_feed(&s,'a'); CHECK(find(&s,"吗")>=0,"remaining m can continue to ma");
    }
    type(&s,d,"nihaom"); idx=find(&s,"泥"); CHECK(idx>=0,"alternative first character is selectable");
    if(idx>=0) {
        s.page=idx/IME_PAGE_SIZE; int n=ime_accept(&s,idx%IME_PAGE_SIZE,out,IME_MAX_RAW);
        CHECK(equal(out,n,"泥")&&s.raw_len==4&&!memcmp(s.raw,"haom",4),"choosing 泥 preserves haom");
    }
    type(&s,d,"ni'hao'm");idx=find(&s,"你好");CHECK(idx>=0,"partial phrase respects separators");
    if(idx>=0) {s.page=idx/IME_PAGE_SIZE;ime_accept(&s,idx%IME_PAGE_SIZE,out,IME_MAX_RAW);
        CHECK(s.raw_len==1&&s.raw[0]=='m',"consume separators with selected prefix");}
    type(&s,d,"nihao");out[0]=0xdeadbeef;
    CHECK(ime_accept(&s,0,out,1)==-1&&s.raw_len==5&&out[0]==0xdeadbeef,"short output capacity cannot consume or partially write");
    CHECK(ime_commit(&s,0,out,1)==-1&&out[0]==0xdeadbeef,"preview commit also refuses truncation");
    CHECK(ime_accept(&s,8,out,64)==-1&&s.raw_len==5,"missing numeric candidate preserves composition");
    struct ime_state other; type(&other,d,"beijing");
    ime_feed(&s,'x');ime_feed(&s,8);
    CHECK(s.ncand&&equal(s.cand[0].cp,s.cand[0].ncp,"你好"),"backspace restores the prior candidates");
    CHECK(other.ncand&&equal(other.cand[0].cp,other.cand[0].ncp,"北京"),"independent state remains unchanged");
    type(&s,d,"");for(int i=0;i<64;i++)ime_feed(&s,'a');
    CHECK(s.raw_len==64&&ime_feed(&s,'a')==IME_FEED_IGNORED,"raw length bounded at 64");
    int committed=0;
    while(s.raw_len>0&&committed<65) {
        int before=s.raw_len;
        int n=ime_accept(&s,0,out,64);
        if(n<0) { CHECK(0,"long spelling remains progressively committable");break; }
        committed+=n;
        if(s.raw_len>=before)break;
    }
    CHECK(s.raw_len==0&&committed==64,"64 syllables commit all 64 characters without truncation");
    type(&s,d,"xyzzy"); int n=ime_accept(&s,IME_COMMIT_RAW,out,64);
    CHECK(equal(out,n,"xyzzy")&&s.raw_len==0,"literal fallback round trips every byte");
#ifdef IME_STATS
    type(&s,d,""); for(int i=0;i<63;i++)ime_feed(&s,'a');ime_stat_reset();ime_feed(&s,'a');
    CHECK(ime_stat_word_probes<=64*65/2,"word DAG probes have a quadratic hard bound");
#endif
    if(d->key_count>32768) {
        const char *words[][2]={{"nvhai","女孩"},{"erweima","二维码"},{"weixin","微信"},
            {"duoxiancheng","多线程"},{"shurufa","输入法"}};
        for(unsigned i=0;i<sizeof words/sizeof words[0];i++){type(&s,d,words[i][0]);CHECK(find(&s,words[i][1])>=0,words[i][1]);}
    }
    printf("%d checks, %d failed\n",checks,failures);free(blob);return failures?1:0;
}
