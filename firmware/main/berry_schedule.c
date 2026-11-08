#include "berry_schedule.h"
typedef struct { int day; const char *time,*title,*location; } course_t;
static const course_t courses[]={
 {1,"10:10–11:50","示例课程","示例教室"}
};
cJSON *berry_schedule_create(time_t utc_now) {
 cJSON *out=cJSON_CreateObject();if(!out)return NULL;
 cJSON_AddStringToObject(out,"status","available");
 cJSON_AddStringToObject(out,"source","firmware-built-in");
 cJSON *rows=cJSON_AddArrayToObject(out,"courses");
 cJSON *dates=cJSON_AddArrayToObject(out,"week_dates");
 if(!rows || !dates){cJSON_Delete(out);return NULL;}
 for(unsigned i=0;i<sizeof(courses)/sizeof(courses[0]);i++) {
  cJSON *c=cJSON_CreateObject();if(!c){cJSON_Delete(out);return NULL;}
  cJSON_AddNumberToObject(c,"day",courses[i].day);
  cJSON_AddStringToObject(c,"time",courses[i].time);
  cJSON_AddStringToObject(c,"title",courses[i].title);
  cJSON_AddStringToObject(c,"location",courses[i].location);
  cJSON_AddItemToArray(rows,c);
 }
 int day=-1;
 if(utc_now>=1700000000) {
  time_t local=utc_now+8*3600;struct tm *t=gmtime(&local);
  if(t) {
   day=t->tm_wday?t->tm_wday:7;
   time_t monday=local-(day-1)*86400;
   for(int i=0;i<7;i++){time_t stamp=monday+i*86400;t=gmtime(&stamp);cJSON_AddItemToArray(dates,cJSON_CreateNumber(t?t->tm_mday:0));}
  }
 }
 cJSON_AddNumberToObject(out,"day",day);
 return out;
}
