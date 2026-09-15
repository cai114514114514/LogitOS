#include "island_game.h"
#include <stdio.h>
static int checks,failed;
#define CHECK(x,s) do{checks++;if(!(x)){failed++;printf("FAIL %s\n",s);}else printf("PASS %s\n",s);}while(0)
int main(void)
{
    struct island_game g;island_reset(&g);
    for(int i=0;i<1200&&!g.won;i++) {
        int jump=g.grounded&&((g.z< -3.0f&&g.z> -4.1f)||(g.z< -11.0f&&g.z> -12.1f));
        island_step(&g,1.f/120,0,-1,jump,0);
    }
    CHECK(g.won&&g.score==3&&g.deaths==0,"normal movement and two jumps complete the level");
    island_reset(&g);g.paused=1;struct island_game same=g;
    island_step(&g,.1f,1,1,1,1);
    CHECK(!memcmp(&g,&same,sizeof g),"paused simulation does not advance");
    g.paused=0;g.standing=1;g.grounded=1;g.x=0;g.y=.6f;g.z=-5;
    island_step(&g,.01f,0,0,0,.7f);
    CHECK(fabsf(g.x-.7f)<.0001f&&g.standing==1,"standing player rides the moving platform");
    g.x=20;g.y=-8;g.grounded=0;island_step(&g,.01f,0,0,0,.7f);
    CHECK(g.deaths==1&&g.x==0&&g.z==1&&g.hit>0,"fall resets position with feedback");
    island_reset(&g);CHECK(!g.score&&!g.deaths&&!g.won&&!g.paused,"restart clears the level");
    printf("Island gameplay: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
