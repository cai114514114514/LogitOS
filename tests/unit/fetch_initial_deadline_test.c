/* A normal finite HTTP response after a delayed first scheduler visit, and
 * an idle connection serviced regularly. The clock is controlled; no sleep,
 * external network, unbounded computation or exceptional memory is used. */
#include "stream_net.h"

static unsigned long long controlled_now(void) { return fake_now; }
static const struct webapi_net CLOCK_NET =
    { f_open, f_poll, f_send, f_recv, f_close, controlled_now, f_unix };

static void route(struct fakesock *s,const char *method,const char *target)
{
    (void)method;
    if(!strcmp(target,"/idle")){s->avail=-1;s->finished=0;return;}
    rsp_add(s,"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n42");
}
static void begin(unsigned long long start)
{
    fs_reset();fake_now=start;fs_set_router(route);
    open_ctx("https://page.example/");js_webapi_set_net(&CLOCK_NET);
    fake_now=start; /* Installation's default fixture clock must not shift zero. */
    run("var done=false,body='',err='';");
}
static void start(const char *path)
{
    char script[512];
    snprintf(script,sizeof script,
        "fetch('%s').then(r=>r.text()).then(t=>{body=t;done=true},"
        "e=>{err=e.name;done=true});",path);
    run(script);drain_jobs();
}
static void tick(unsigned long long ms)
{fake_now+=ms;js_webapi_pump(ctx);drain_jobs();}

static void delayed_first(unsigned long long start_time)
{
    begin(start_time);start("/answer");
    ck(fs_opened==1&&req_count()==0,"constructor opens transport before any HTTP send");
    fake_now+=60000;
    for(int i=0;i<40;i++)tick(16);
    ckjs("done&&body==='42'&&err===''",start_time?
        "delayed first pump receives the ordinary response":
        "time zero is a valid initial observation baseline");
    ck(!js_webapi_pending(),"completed request releases its pending transport");
    close_ctx();
}
static void idle_clock(int initial_gap)
{
    begin(1000);start("/idle");
    if(initial_gap)fake_now+=60000;
    for(int i=0;i<1800;i++)tick(16); /* 28.8 seconds of serviced time */
    ckjs("!done",initial_gap?
        "first-pump gap does not consume the serviced idle budget":
        "regular polling does not expire before thirty seconds");
    for(int i=0;i<100;i++)tick(16);  /* 30.4 seconds of serviced time */
    ckjs("done&&err==='TypeError'&&body===''",initial_gap?
        "after the initial gap the ordinary idle timeout still expires":
        "regular polling still expires the ordinary idle timeout");
    ck(!js_webapi_pending(),"idle timeout releases its pending transport");
    close_ctx();
}
static void unready_clock(void)
{
    begin(1000);start("/answer");
    int fd=fs_live();if(fd>=0)fs[fd].ready_after=100000;
    for(int i=0;i<1800;i++)tick(16);
    ckjs("!done","regular connection polling keeps the original connect budget");
    for(int i=0;i<100;i++)tick(16);
    ckjs("done&&err==='TypeError'","regular connection polling still times out");
    ck(req_count()==0&&!js_webapi_pending(),"unready connection times out without an HTTP request");
    close_ctx();
}
int main(void)
{
    delayed_first(1000);delayed_first(0);
    idle_clock(0);idle_clock(1);unready_clock();
    printf("fetch-initial-deadline: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
