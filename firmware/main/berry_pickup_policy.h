#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct {
    bool sampled,armed,candidate,candidate_off;
    uint32_t last,quiet_since,candidate_since,settled_since,last_wake;
    unsigned candidate_samples,wakes;
    float previous[3],filtered[3],rest[3];
} berry_pickup_policy_t;

static inline void berry_pickup_reset(berry_pickup_policy_t *p) {
    unsigned wakes=p->wakes;
    memset(p,0,sizeof(*p));p->wakes=wakes;
}

static inline bool berry_pickup_sample(berry_pickup_policy_t *p,float x,float y,float z,
                                      uint32_t now,bool screen_off) {
    float v[3]={x,y,z};
    float norm=sqrtf(x*x+y*y+z*z);
    if(!isfinite(norm)||norm<100||norm>3900) {
        p->sampled=false;p->armed=false;p->candidate=false;p->quiet_since=0;
        return false;
    }
    if(!p->sampled || (uint32_t)(now-p->last)>200) {
        p->sampled=true;p->armed=false;p->candidate=false;p->quiet_since=0;
        memcpy(p->previous,v,sizeof(v));memcpy(p->filtered,v,sizeof(v));p->last=now;return false;
    }
    float delta=0,raw_delta=0;
    for(int i=0;i<3;i++) {
        float d=v[i]-p->previous[i],step=(v[i]-p->filtered[i])*.25f;
        raw_delta+=d*d;delta+=step*step;
        p->previous[i]=v[i];p->filtered[i]+=step;v[i]=p->filtered[i];
    }
    p->last=now;
    norm=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    /* Ignore measured board jitter; retain a raw channel for brisk walking. */
    bool moving=raw_delta>180*180||delta>55*55||fabsf(norm-1000)>220;
    bool stable=raw_delta<160*160&&delta<28*28&&fabsf(norm-1000)<200;
    if(p->last_wake&&now-p->last_wake<10000)return false;
    if(!p->armed&&!p->candidate) {
        if(!stable){p->quiet_since=0;return false;}
        if(!p->quiet_since){p->quiet_since=now;memcpy(p->rest,v,sizeof(v));}
        float deviation=0;
        for(int i=0;i<3;i++){float d=v[i]-p->rest[i];deviation+=d*d;}
        if(deviation>80*80){p->quiet_since=now;memcpy(p->rest,v,sizeof(v));}
        else if(now-p->quiet_since>=3000){p->armed=true;p->quiet_since=0;}
        return false;
    }
    float rest_norm=0,dot=0,deviation=0;
    for(int i=0;i<3;i++){rest_norm+=p->rest[i]*p->rest[i];dot+=v[i]*p->rest[i];float d=v[i]-p->rest[i];deviation+=d*d;}
    float cosine=dot/(norm*sqrtf(rest_norm));
    if(!p->candidate) {
        if(deviation<140*140&&!moving)return false;
        p->candidate=true;p->candidate_off=screen_off;p->armed=false;p->candidate_since=now;
        p->candidate_samples=0;p->settled_since=0;
    }
    p->candidate_samples++;
    if(now-p->candidate_since>2000) {
        p->candidate=false;p->quiet_since=0;return false;
    }
    /* Require a 25–95 degree change, then a quiet hold; bumps and flat slides fail. */
    if(stable&&cosine<0.9063f&&cosine>-0.0872f) {
        if(!p->settled_since)p->settled_since=now;
        if(now-p->settled_since>=400&&p->candidate_samples>=8) {
            p->candidate=false;p->quiet_since=0;
            if(screen_off&&p->candidate_off){p->last_wake=now;p->wakes++;return true;}
        }
    } else p->settled_since=0;
    return false;
}
