#include "berry_battery_estimate.h"
int berry_battery_estimate(berry_battery_estimate_t *s,int percent,bool discharging,int64_t now){
 if(!s)return -1;
 if(!discharging || percent<0 || percent>100 || now<0){s->percent=-1;s->timestamp=0;return -1;}
 if(s->percent<0 || percent>s->percent || now<s->timestamp){s->percent=percent;s->timestamp=now;return -1;}
 int drop=s->percent-percent;int64_t elapsed=now-s->timestamp;
 if(drop<2 || elapsed<1800)return -1;
 int64_t minutes=elapsed*(int64_t)percent/drop/60;
 return minutes>0 && minutes<7*24*60?(int)minutes:-1;
}
