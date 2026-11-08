/* Daily bubbles stay local and only use confirmed state. */
static int64_t task_notice_time(const char *stamp);
static unsigned task_pet_waiting(const cJSON *data,int64_t now,unsigned kind,bool mark);

#define PET_REPEAT_MS 60000u
#define PET_MONITOR_GAP_MS 300000u
static int64_t pet_last_course,pet_last_rain_day;
static int64_t pet_quota_reset,pet_monitor_observed;
static unsigned pet_quota_band;
static uint32_t pet_monitor_tick;
static bool pet_monitor_gap_spoken;
static bool pet_low_battery_spoken;
static unsigned pet_next_reading;
static uint32_t pet_course_report_tick,pet_reading_ticks[4];
static int64_t pet_course_report;

typedef struct {
    int percent, minutes;
    bool powered;
} pet_battery_t;

static pet_battery_t pet_battery_read(void) {
#ifndef BERRY_SIMULATOR
    berry_panel_state_t panel;
    berry_panel_snapshot(&panel);
    return (pet_battery_t){panel.battery,panel.remaining_minutes,panel.charging||panel.external};
#else
    return (pet_battery_t){-1,-1,false};
#endif
}

static void pet_battery_update(pet_battery_t battery) {
    if(battery.powered||(battery.percent>25&&battery.percent<=100))pet_low_battery_spoken=false;
}

static int pet_course_summary(time_t now,int weekday,int minute,int *next) {
    cJSON *schedule=berry_schedule_create(now);
    const cJSON *courses=item(schedule,"courses");
    int count=0;
    *next=1440;
    for(int i=0;i<cJSON_GetArraySize(courses);i++) {
        const cJSON *course=cJSON_GetArrayItem(courses,i);
        const char *start=str(course,"time","");
        int h,m;
        if(num(course,"day")!=weekday||sscanf(start,"%2d:%2d",&h,&m)!=2||h<0||h>23||m<0||m>59)continue;
        int starts=h*60+m;
        if(starts<=minute)continue;
        if(starts<*next)*next=starts;
        count++;
    }
    cJSON_Delete(schedule);
    return count;
}

static bool pet_today_rain(const cJSON *data,const struct tm *local) {
    char date[12];
    if(!strftime(date,sizeof(date),"%Y-%m-%d",local))return false;
    const cJSON *forecast=item(item(data,"weather"),"forecast");
    for(int i=0;i<cJSON_GetArraySize(forecast);i++) {
        const cJSON *day=cJSON_GetArrayItem(forecast,i);
        if(strcmp(str(day,"date",""),date))continue;
        int code=num(day,"code"),chance=num(day,"rain_probability");
        return (code>=51&&code<=67)||(code>=80&&code<=82)||(code>=95&&code<=99)||(chance>=60&&chance<=100);
    }
    return false;
}

static bool pet_frame_fresh(const cJSON *data,time_t now) {
    int64_t observed=task_notice_time(str(data,"generated_at",""));
    return now>=1700000000&&strcmp(str(data,"status",""),"fresh")==0&&
           observed>0&&observed<=now+5&&now-observed<=120;
}

static void pet_monitor_observe(const cJSON *data,time_t now,uint32_t tick) {
    if(!pet_frame_fresh(data,now))return;
    const cJSON *pc=item(data,"dorm_pc"),*gpu=item(pc,"gpu");
    const cJSON *sources[]={pc,item(data,"aliyun"),gpu};
    int64_t latest=pet_monitor_observed;
    for(unsigned i=0;i<3;i++) {
        const cJSON *source=sources[i];
        int64_t observed=task_notice_time(str(source,i==2?"usage_updated_at":"updated_at",""));
        if(observed<=latest||observed>now+5||now-observed>120)continue;
        int cpu=num(item(source,"cpu"),"usage_percent"),memory=num(item(source,"memory"),"usage_percent");
        int gpu_usage=i<2?num(item(source,"gpu"),"usage_percent"):num(source,"usage_percent");
        int latency=i==1?num(source,"latency_ms"):-1;
        if((cpu>=0&&cpu<=100)||(memory>=0&&memory<=100)||(gpu_usage>=0&&gpu_usage<=100)||latency>=0)latest=observed;
    }
    if(latest>pet_monitor_observed) {
        pet_monitor_observed=latest;
        pet_monitor_tick=tick-(uint32_t)(latest<=now?now-latest:0)*1000u;
        pet_monitor_gap_spoken=false;
    }
}

static bool pet_quota_remind(const cJSON *quota,time_t now) {
    static const char *const days[]={"日","一","二","三","四","五","六"};
    const cJSON *remaining=item(quota,"remaining_percent");
    if(!cJSON_IsNumber(remaining))return false;
    double left=remaining->valuedouble;
    if(!(left>=0&&left<=10))return false;
    unsigned band=left==0?3:left<=5?2:1;
    int64_t resets=task_notice_time(str(quota,"resets_at",""));
    if(resets<=now||resets<pet_quota_reset||(resets==pet_quota_reset&&band<=pet_quota_band))return false;
    time_t reset_local=(time_t)(resets+8*3600);
    const struct tm *local=gmtime(&reset_local);
    if(!local)return false;
    int shown=(int)left;
    if(left>shown)shown++;
    char text[96];
    snprintf(text,sizeof(text),"本周额度只剩%d%%。\n周%s%02d:%02d重置。",
             shown,days[local->tm_wday],local->tm_hour,local->tm_min);
    if(!pet_say(text,5000))return false;
    pet_quota_reset=resets;
    pet_quota_band=band;
    return true;
}

static bool pet_report_useful(const cJSON *data,time_t now,uint32_t tick,bool tapped,pet_battery_t battery) {
    char text[96];
    pet_battery_update(battery);
    if(!pet_low_battery_spoken&&!battery.powered&&battery.percent>=0&&battery.percent<=20) {
        snprintf(text,sizeof(text),"电量还剩%d%%。\n记得充电。",battery.percent);
        if(pet_say(text,5000)){pet_low_battery_spoken=true;return true;}
    }

    time_t local_time=now+8*3600;
    const struct tm *converted=now>=1700000000?gmtime(&local_time):NULL;
    struct tm local={0};
    int remaining=0,next=1440,minutes=0;
    int64_t today=0,course_start=0;
    bool course_recent=false;
    if(converted) {
        local=*converted;
        minutes=local.tm_hour*60+local.tm_min;
        remaining=pet_course_summary(now,local.tm_wday?local.tm_wday:7,minutes,&next);
        today=local_time/86400;
        course_start=today*1440+next;
        course_recent=pet_course_report==course_start&&tick-pet_course_report_tick<PET_REPEAT_MS;
        if(remaining&&next-minutes<=20&&pet_last_course!=course_start&&!course_recent) {
            snprintf(text,sizeof(text),"下一节还有%d分钟。\n%02d:%02d开始。",next-minutes,next/60,next%60);
            if(pet_say(text,5000)) {
                pet_last_course=pet_course_report=course_start;
                pet_course_report_tick=tick;
                return true;
            }
        }
    }

    for(unsigned kind=2;kind<=4;kind*=2) {
        unsigned waiting=task_pet_waiting(data,now,kind,false);
        if(!waiting)continue;
        snprintf(text,sizeof(text),"有%u项任务等你%s。\n在 Codex 页。",waiting,kind==2?"批准":"输入");
        if(pet_say(text,5000)){task_pet_waiting(data,now,kind,true);return true;}
    }

    bool fresh=pet_frame_fresh(data,now);
    const cJSON *quota=fresh?cJSON_GetArrayItem(item(item(data,"codex"),"quota_windows"),1):NULL;
    if(!tapped) {
        if(pet_quota_remind(quota,now))return true;
        if(fresh&&today&&pet_last_rain_day!=today&&pet_today_rain(data,&local)&&
           pet_say("今天预报有雨。\n出门记得带伞。",5000)) {
            pet_last_rain_day=today;
            return true;
        }
        return false;
    }

    if(pet_monitor_observed&&!pet_monitor_gap_spoken&&tick-pet_monitor_tick>=PET_MONITOR_GAP_MS&&
       pet_say("监控数据5分钟没更新。\n读数可能已过时。",5000)) {
        pet_monitor_gap_spoken=true;
        return true;
    }
    if(remaining&&!course_recent) {
        snprintf(text,sizeof(text),"今天还有%d节课。\n下一节%02d:%02d。",remaining,next/60,next%60);
        if(pet_say(text,5000)){pet_course_report=course_start;pet_course_report_tick=tick;return true;}
    }
    const cJSON *pc=fresh?item(data,"dorm_pc"):NULL;
    int64_t pc_observed=task_notice_time(str(pc,"updated_at",""));
    if(pc_observed>0&&now-pc_observed>=PET_MONITOR_GAP_MS/1000u)pc=NULL;
    for(unsigned offset=0;offset<4;offset++) {
        unsigned reading=(pet_next_reading+offset)%4;
        if(pet_reading_ticks[reading]&&tick-pet_reading_ticks[reading]<PET_REPEAT_MS)continue;
        if(reading<2) {
            const cJSON *metric=item(pc,reading?"cpu":"gpu");
            int temp=num(metric,"temperature_c"),usage=num(metric,"usage_percent");
            if(temp<0||temp>150||usage<0||usage>100)continue;
            snprintf(text,sizeof(text),"%s现在%d℃。\n占用%d%%。",reading?"CPU":"显卡",temp,usage);
        }else if(reading==2) {
            if(battery.powered||battery.percent<0||battery.percent>100||battery.minutes<=0||battery.minutes>1440)continue;
            if(battery.minutes>=60)snprintf(text,sizeof(text),"小屏电量%d%%。\n预计还能用%d小时。",battery.percent,battery.minutes/60);
            else snprintf(text,sizeof(text),"小屏电量%d%%。\n预计还能用%d分钟。",battery.percent,battery.minutes);
        }else{
            int left=num(quota,"remaining_percent");
            int64_t resets=task_notice_time(str(quota,"resets_at",""));
            if(left<0||left>100||(resets>0&&resets<=now))continue;
            snprintf(text,sizeof(text),"本周额度还剩%d%%。",left);
        }
        if(pet_say(text,5000)) {
            pet_reading_ticks[reading]=tick;
            pet_next_reading=(reading+1)%4;
            return true;
        }
    }
    return false;
}
