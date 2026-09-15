/* Same durable store and core as the existing suite. Only fixtures are local;
 * no account data, wall-clock manipulation or network requests are involved. */
#define main prior_persistence_main
#include "cookie_persistence_test.c"
#undef main

static void put64(unsigned char *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(unsigned char)(v>>(8*i)); }
static void mutate64(int offset,uint64_t value)
{ for(int i=0;i<2;i++){put64(files[i]+offset,value);fix_crc(files[i],sizes[i]);} }
static void rollback(void)
{
    fresh();int64_t future=now+28800;
    ck(set("early=one; Max-Age=3600; Path=/; Secure; HttpOnly; SameSite=Strict",future),"rollback short seed commits");
    ck(set("late=two; Max-Age=34560000; Path=/; Secure",future+10),"rollback horizon seed commits");
    int rc=cookie_persistence_open(&state,&jar,&store,now);
    ck(rc==0 && jar.n==2,"clock rollback restores both validated records");
    int bounds=jar.n==2,deadline=jar.n==2,order=jar.n==2,attrs=jar.n==2;
    if(jar.n==2){
        for(int i=0;i<2;i++)bounds &= jar.v[i].created>=0 && jar.v[i].created<=jar.v[i].accessed && jar.v[i].accessed<=now && jar.v[i].expires<=now+CK_MAX_AGE_SECONDS;
        deadline &= jar.v[0].expires==future+3600 && jar.v[1].expires==now+CK_MAX_AGE_SECONDS;
        order &= jar.v[0].created==now-10 && jar.v[1].created==now;
        attrs &= jar.v[0].secure && jar.v[0].http_only && jar.v[0].host_only && jar.v[0].samesite==CK_SS_STRICT;
    }
    ck(bounds,"clock rollback normalizes bookkeeping and bounds remaining lifetime");
    ck(deadline,"clock rollback never extends absolute expiry");
    ck(order,"clock rollback preserves relative creation order");
    ck(attrs,"clock rollback retains all security attributes");
    char header[CK_HEADER_MAX];cookie_header(&jar,&ctx,now,header,sizeof header);
    ck(!strcmp(header,"early=one; late=two"),"clock rollback preserves actual Cookie ordering");
    ck(cookie_persistence_flush(&state,&jar,now)==0,"clock rollback next access can durably flush");
    ck(cookie_persistence_open(&state,&jar,&store,now+1)==0 && jar.n==2,"clock rollback rewritten snapshot reopens without another rollback");
    ck(cookie_persistence_open(&state,&jar,&store,future+3600)==0 && !has("early","one") && has("late","two"),"clock rollback still expires at the unchanged absolute deadline");
}
static void malformed(void)
{
    seed();mutate64(32+28,now+1);
    ck(cookie_persistence_open(&state,&jar,&store,now-28800)==CK_PERSIST_CORRUPT&&jar.n==0,"rollback cannot repair created after accessed");
    seed();mutate64(32+20,now);
    ck(cookie_persistence_open(&state,&jar,&store,now-28800)==CK_PERSIST_CORRUPT&&jar.n==0,"rollback cannot repair expiry before access");
    seed();mutate64(32+20,now+CK_MAX_AGE_SECONDS+1);
    ck(cookie_persistence_open(&state,&jar,&store,now-28800)==CK_PERSIST_CORRUPT&&jar.n==0,"rollback validates original 400-day lifetime before clamp");
    seed();for(int i=0;i<2;i++)files[i][sizes[i]-1]^=1;
    ck(cookie_persistence_open(&state,&jar,&store,now-28800)==CK_PERSIST_CORRUPT&&jar.n==0,"rollback never bypasses checksum validation");
    fresh();ck(set("__Host-test=local; Secure; Path=/; Max-Age=3600",now),"prefix seed commits");
    for(int i=0;i<2;i++){files[i][48]&=~4;fix_crc(files[i],sizes[i]);}
    ck(cookie_persistence_open(&state,&jar,&store,now-28800)==CK_PERSIST_CORRUPT&&jar.n==0,"rollback never bypasses prefix security validation");
    seed();for(int i=0;i<2;i++){files[i][48]|=128;fix_crc(files[i],sizes[i]);}
    ck(cookie_persistence_open(&state,&jar,&store,now-28800)==CK_PERSIST_CORRUPT&&jar.n==0,"rollback never bypasses stored flags validation");
}
static void extremes(void)
{
    seed();ck(cookie_persistence_open(&state,&jar,&store,10)==0&&jar.n==1,"large rollback remains bounded near Unix epoch");
    ck(jar.n==1&&jar.v[0].created==10&&jar.v[0].accessed==10&&jar.v[0].expires==10+CK_MAX_AGE_SECONDS,"large rollback clamps only absolute deadline downward");
    fresh();ck(set("end=bounded; Secure; Path=/; Max-Age=3600",INT64_MAX-100),"saturating expiry seed commits");
    ck(cookie_persistence_open(&state,&jar,&store,INT64_MAX-101)==0&&jar.n==1&&jar.v[0].expires==INT64_MAX,"clock rollback near INT64_MAX does not overflow");
    seed();ck(cookie_persistence_open(&state,&jar,&store,now+1)==0&&jar.n==1&&jar.v[0].created==now,"forward clock retains original metadata");
    ck(cookie_persistence_open(&state,&jar,&store,-1)==CK_PERSIST_IO&&jar.n==0,"negative wall clock is an explicit clock IO failure");
    fresh();ck(set("late=two; Secure; Path=/; Max-Age=3600",now+10),"unordered creation late seed commits");
    ck(set("early=one; Secure; Path=/; Max-Age=3600",now),"unordered creation early seed commits");
    char header[CK_HEADER_MAX];cookie_header(&jar,&ctx,now+100,header,sizeof header);
    ck(set("trigger=three; Secure; Path=/; Max-Age=3600",now+100),"later mutation checkpoints original access times");
    ck(cookie_persistence_open(&state,&jar,&store,10)==0&&jar.n==3,"epoch rollback accepts unordered snapshot creation dates");
    cookie_header(&jar,&ctx,10,header,sizeof header);
    ck(!strcmp(header,"early=one; late=two; trigger=three"),"epoch clipping keeps original creation ordering for actual header");
}
int main(void)
{
    cookie_jar_init(&jar);rollback();malformed();extremes();
    cookie_persistence_open(&state,&jar,NULL,now);
    for(int i=0;i<2;i++)free(files[i]);cookie_jar_free(&jar);
    printf("cookie-clock: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
