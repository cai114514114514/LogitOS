/* Reuse the exact-route loader transport. Only one script response is held;
 * the production browser must return to its event loop to process queued input.
 * Pump never dispatches input or JS. A bounded pump fallback lets the old
 * synchronous wait finish and fail an assertion instead of hanging the gate. */
#define bfetch_start_from held_base_start_from
#define bfetch_start held_base_start
#define bfetch_start_nav held_base_start_nav
#define bfetch_state held_base_state
#define bfetch_pump held_base_pump
#define bfetch_take held_base_take
#define bfetch_release held_base_release
#define bfetch_close_all held_base_close_all
#include "loader_fakebfetch.c"
#undef bfetch_start_from
#undef bfetch_start
#undef bfetch_start_nav
#undef bfetch_state
#undef bfetch_pump
#undef bfetch_take
#undef bfetch_release
#undef bfetch_close_all

extern unsigned long long host_clock;
extern void held_native_arrival(void);
extern void held_lifetime_observe(void);
int held_id=-1, held_started, held_active, held_pumps, held_fallback;
int held_allow, held_cancelled, held_taken, held_released, held_auto;
int bfetch_start_from(const char *base,const char *ref)
{
    int id=held_base_start_from(base,ref);
    if(id>=0 && strstr(bfetch_url(id),"/slow.js")){
        held_id=id;held_started++;held_active=1;held_pumps=0;
    }
    return id;
}
int bfetch_start(const char *ref){return bfetch_start_from(0,ref);}
int bfetch_start_nav(const char *ref){return held_base_start_nav(ref);}
int bfetch_state(int id)
{return held_active&&id==held_id?BF_PENDING:held_base_state(id);}
int bfetch_pump(void)
{
    if(!held_active)return 0;
    held_lifetime_observe();
    host_clock+=10;held_pumps++;
    if(held_pumps==2)held_native_arrival();
    if(held_allow || (held_auto&&held_pumps>=30)){
        held_active=0;held_released++;return 0;
    }
    if(held_pumps>=500){held_fallback=1;held_active=0;held_released++;return 0;}
    return 1;
}
int bfetch_take(int id,unsigned char **out)
{
    if(id==held_id){held_taken++;held_active=0;held_id=-1;}
    return held_base_take(id,out);
}
void bfetch_release(int id)
{
    if(id==held_id){held_lifetime_observe();held_cancelled++;held_active=0;held_id=-1;}
    held_base_release(id);
}
void bfetch_close_all(void)
{
    /* Unlike the original synchronous fixture, this fixture owns a live ID.
     * Observe actual close-all cancellation, including native window close. */
    if(held_id>=0)bfetch_release(held_id);
    held_base_close_all();
}
