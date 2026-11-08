/* Daily bubbles stay local and only use confirmed state. */
static int64_t task_notice_time(const char *stamp);
static unsigned task_pet_waiting(const cJSON *data,int64_t now,unsigned kind);

#define PET_MONITOR_GAP_MS 300000u
static int64_t pet_monitor_observed;
static uint32_t pet_monitor_tick;

enum {
    PET_LINE_LOW_BATTERY, PET_LINE_NEXT_COURSE, PET_LINE_APPROVAL, PET_LINE_INPUT,
    PET_LINE_RAIN, PET_LINE_LOW_QUOTA, PET_LINE_MONITOR_GAP, PET_LINE_COURSES,
    PET_LINE_GPU, PET_LINE_CPU, PET_LINE_BATTERY, PET_LINE_QUOTA, PET_LINE_COUNT
};

typedef struct {
    unsigned spoken;
    unsigned last;
} pet_rotation_t;
static pet_rotation_t pet_enter_rotation={0, PET_LINE_COUNT};
static pet_rotation_t pet_tap_rotation={0, PET_LINE_COUNT};

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
    }
}

static void pet_low_quota_line(char *text,size_t size,const cJSON *quota,time_t now) {
    static const char *const days[]={"日","一","二","三","四","五","六"};
    const cJSON *remaining=item(quota,"remaining_percent");
    if(!cJSON_IsNumber(remaining))return;
    double left=remaining->valuedouble;
    if(!(left>=0&&left<=10))return;
    int64_t resets=task_notice_time(str(quota,"resets_at",""));
    if(resets<=now)return;
    time_t reset_local=(time_t)(resets+8*3600);
    const struct tm *local=gmtime(&reset_local);
    if(!local)return;
    int shown=(int)left;
    if(left>shown)shown++;
    snprintf(text,size,"本周额度只剩%d%%。\n周%s%02d:%02d重置。",
             shown,days[local->tm_wday],local->tm_hour,local->tm_min);
}


static bool pet_say_next(const char lines[PET_LINE_COUNT][96],pet_rotation_t *rotation) {
    unsigned eligible=0;
    for(unsigned i=0;i<PET_LINE_COUNT;i++) {
        if(lines[i][0])eligible|=1u<<i;
    }
    unsigned choices=eligible&~rotation->spoken;
    if(!choices) {
        rotation->spoken&=~eligible;
        choices=eligible;
    }
    if(choices&(choices-1))choices&=~(1u<<rotation->last);
    while(choices) {
        unsigned count=0;
        for(unsigned bits=choices;bits;bits&=bits-1)count++;
        unsigned pick=pet_rand()%count;
        for(unsigned i=0;i<PET_LINE_COUNT;i++) {
            if(!(choices&(1u<<i)))continue;
            if(pick){pick--;continue;}
            if(pet_say(lines[i],5000)) {
                rotation->spoken|=1u<<i;
                rotation->last=i;
                return true;
            }
            choices&=~(1u<<i);
            break;
        }
    }
    return false;
}

static bool pet_report_useful(const cJSON *data,time_t now,uint32_t tick,bool tapped,pet_battery_t battery) {
    char lines[PET_LINE_COUNT][96]={{0}};
    if(!battery.powered&&battery.percent>=0&&battery.percent<=20) {
        snprintf(lines[PET_LINE_LOW_BATTERY],96,"电量还剩%d%%。\n记得充电。",battery.percent);
    }

    time_t local_time=now+8*3600;
    const struct tm *converted=now>=1700000000?gmtime(&local_time):NULL;
    bool fresh=pet_frame_fresh(data,now);
    if(converted) {
        struct tm local=*converted;
        int minute=local.tm_hour*60+local.tm_min;
        int next=1440;
        int courses=pet_course_summary(now,local.tm_wday?local.tm_wday:7,minute,&next);
        if(courses&&next-minute<=20) {
            snprintf(lines[PET_LINE_NEXT_COURSE],96,"下一节还有%d分钟。\n%02d:%02d开始。",next-minute,next/60,next%60);
        }
        if(tapped&&courses) {
            snprintf(lines[PET_LINE_COURSES],96,"今天还有%d节课。\n下一节%02d:%02d。",courses,next/60,next%60);
        }
        if(fresh&&pet_today_rain(data,&local)) {
            snprintf(lines[PET_LINE_RAIN],96,"今天预报有雨。\n出门记得带伞。");
        }
    }

    unsigned approvals=task_pet_waiting(data,now,2);
    unsigned inputs=task_pet_waiting(data,now,4);
    if(approvals)snprintf(lines[PET_LINE_APPROVAL],96,"有%u项任务等你批准。\n在 Codex 页。",approvals);
    if(inputs)snprintf(lines[PET_LINE_INPUT],96,"有%u项任务等你输入。\n在 Codex 页。",inputs);
    const cJSON *quota=fresh?cJSON_GetArrayItem(item(item(data,"codex"),"quota_windows"),1):NULL;
    pet_low_quota_line(lines[PET_LINE_LOW_QUOTA],96,quota,now);
    if(!tapped)return pet_say_next(lines,&pet_enter_rotation);

    if(pet_monitor_observed&&tick-pet_monitor_tick>=PET_MONITOR_GAP_MS) {
        snprintf(lines[PET_LINE_MONITOR_GAP],96,"监控数据5分钟没更新。\n读数可能已过时。");
    }
    const cJSON *pc=fresh?item(data,"dorm_pc"):NULL;
    int64_t pc_observed=task_notice_time(str(pc,"updated_at",""));
    if(pc_observed>0&&now-pc_observed>=PET_MONITOR_GAP_MS/1000u)pc=NULL;
    for(unsigned i=0;i<2;i++) {
        const cJSON *metric=item(pc,i?"cpu":"gpu");
        int temp=num(metric,"temperature_c"),usage=num(metric,"usage_percent");
        if(temp<0||temp>150||usage<0||usage>100)continue;
        snprintf(lines[i?PET_LINE_CPU:PET_LINE_GPU],96,"%s现在%d℃。\n占用%d%%。",i?"CPU":"显卡",temp,usage);
    }
    if(!battery.powered&&battery.percent>=0&&battery.percent<=100&&battery.minutes>0&&battery.minutes<=1440) {
        if(battery.minutes>=60) {
            snprintf(lines[PET_LINE_BATTERY],96,"小屏电量%d%%。\n预计还能用%d小时。",battery.percent,battery.minutes/60);
        }else{
            snprintf(lines[PET_LINE_BATTERY],96,"小屏电量%d%%。\n预计还能用%d分钟。",battery.percent,battery.minutes);
        }
    }
    const cJSON *remaining=item(quota,"remaining_percent");
    int64_t resets=task_notice_time(str(quota,"resets_at",""));
    if(cJSON_IsNumber(remaining)&&remaining->valuedouble>=0&&remaining->valuedouble<=100&&(resets==0||resets>now)) {
        int percent=(int)remaining->valuedouble;
        if(remaining->valuedouble>percent)percent++;
        snprintf(lines[PET_LINE_QUOTA],96,"本周额度还剩%d%%。",percent);
    }
    return pet_say_next(lines,&pet_tap_rotation);
}
