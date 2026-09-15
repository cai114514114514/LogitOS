#ifndef ISLAND_GAME_H
#define ISLAND_GAME_H
/* Gameplay only. Rendering, interpolation, springs and character animation
 * live in OpenLogit. The same collision code runs in the guest and host gate. */
#include <math.h>
#include <string.h>
struct island_platform {float x,y,z,w,h,d,previous_x;};
struct island_game {
    float x,y,z,vy,yaw,pitch,time,hit;
    int grounded,standing,collected[3],score,won,paused,deaths;
    struct island_platform platforms[4];
};
static const float island_collect_z[3]={-2,-9,-16};
static const float island_collect_y[3]={.8f,1.8f,2.3f};
static void island_reset(struct island_game *g)
{
    memset(g,0,sizeof *g);g->z=1;g->standing=0;g->grounded=1;g->pitch=.58f;
    g->platforms[0]=(struct island_platform){0,-.5f,0,8,1,8,0};
    g->platforms[1]=(struct island_platform){0,.3f,-5,2.6f,.6f,2,0};
    g->platforms[2]=(struct island_platform){0,.5f,-9,6,1,6,0};
    g->platforms[3]=(struct island_platform){0,1,-16,6,1,6,0};
}
static void island_step(struct island_game *g,float dt,int xaxis,int zaxis,int jump,float platform_x)
{
    if(g->paused||g->won||dt<=0)return;
    g->time+=dt;if(g->hit>0)g->hit-=dt;
    struct island_platform *moving=&g->platforms[1];
    moving->previous_x=moving->x;moving->x=platform_x;
    if(g->standing==1&&g->grounded)g->x+=moving->x-moving->previous_x;
    if(jump&&g->grounded){g->vy=7.5f;g->grounded=0;g->standing=-1;}
    float vx=xaxis*4.2f,vz=zaxis*4.2f;
    if(xaxis&&zaxis){vx*=.70710678f;vz*=.70710678f;}
    float oldx=g->x,oldz=g->z,oldy=g->y;
    g->x+=vx*dt;g->z+=vz*dt;
    if(!g->grounded)g->vy-=16*dt;
    g->y+=g->vy*dt;g->grounded=0;g->standing=-1;
    for(int i=0;i<4;i++) {
        struct island_platform *p=&g->platforms[i];float top=p->y+p->h*.5f;
        if(fabsf(g->x-p->x)>p->w*.5f+.24f||fabsf(g->z-p->z)>p->d*.5f+.24f)continue;
        if(oldy>=top-.06f&&g->y<=top&&g->vy<=0) {
            g->y=top;g->vy=0;g->grounded=1;g->standing=i;
        } else if(g->y<top-.08f&&g->y+1.15f>p->y-p->h*.5f) {
            /* Resolve horizontal side contact using the previous position;
             * landing above takes priority, so platform edges are jumpable. */
            if(fabsf(oldx-p->x)>p->w*.5f+.24f)g->x=oldx;
            if(fabsf(oldz-p->z)>p->d*.5f+.24f)g->z=oldz;
        }
    }
    if(g->y < -7) {
        g->x=0;g->y=0;g->z=1;g->vy=0;g->grounded=1;g->standing=0;g->hit=.7f;g->deaths++;
    }
    for(int i=0;i<3;i++)if(!g->collected[i] && fabsf(g->x)<.8f &&
        fabsf(g->z-island_collect_z[i])<.75f && fabsf(g->y+.7f-island_collect_y[i])<1.1f) {
        g->collected[i]=1;g->score++;
    }
    if(g->score==3)g->won=1;
}
#endif
