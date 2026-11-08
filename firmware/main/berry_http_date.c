#include "berry_http_date.h"
#include <stdio.h>
#include <string.h>
bool berry_http_date_epoch(const char *value,int64_t *epoch){
 if(!value || !epoch)return false;
 char weekday[4]={0},month[4]={0},zone[4]={0},extra;int d,y,h,m,s;
 if(sscanf(value,"%3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d %3[A-Za-z]%c",weekday,&d,month,&y,&h,&m,&s,zone,&extra)!=8)return false;
 if(strcmp(zone,"GMT") || y<2023 || y>2100 || h<0 || h>23 || m<0 || m>59 || s<0 || s>59)return false;
 const char *months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};int mo=0;for(int i=0;i<12;i++)if(strcmp(month,months[i])==0){mo=i+1;break;}if(!mo)return false;
 const int days_month[]={31,28,31,30,31,30,31,31,30,31,30,31};bool leap=(y%4==0 && (y%100!=0 || y%400==0));int limit=days_month[mo-1]+(mo==2&&leap?1:0);if(d<1 || d>limit)return false;
 int64_t days=0;for(int i=1970;i<y;i++)days+=365+((i%4==0 && (i%100!=0 || i%400==0))?1:0);for(int i=1;i<mo;i++)days+=days_month[i-1]+(i==2&&leap?1:0);days+=d-1;
 *epoch=((days*24+h)*60+m)*60+s;return true;
}
