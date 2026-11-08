/* Native renderer using nasawz/GrokBot expression geometry (BSD-3-Clause).
 * Copyright (c) 2026 nasawz; grokbot-web contributors. See GrokBot-LICENSE. */
#include <math.h>
#include <stdlib.h>
#include "berry_pet_expressions.h"
#ifndef BERRY_SIMULATOR
#include "esp_random.h"
#include "esp_heap_caps.h"
#endif
enum { PET_IDLE, PET_BUSY, PET_HOT, PET_OFFLINE, PET_UNKNOWN };
static int pet_scene=PET_UNKNOWN,pet_previous_scene=PET_UNKNOWN;
static unsigned pet_busy_ms[3],pet_hot_ms[2],pet_offline_ms;
static unsigned pet_recovery_until,pet_action_until,pet_next_action,pet_blink_at,pet_blink_start,pet_notice_until,pet_last_notice;
static unsigned pet_last_policy_tick,pet_random_state=0x94ae17;
static unsigned pet_last_expression=255,pet_expression;
static unsigned pet_motion;
static float pet_turn,pet_gaze;
static unsigned pet_frames;
static int pet_source=-1,pet_value=-1,pet_notice_scene=-1;
static unsigned pet_notice_target;
static char pet_notice_text[96];
static void pet_text(char *out,size_t size);
static float pet_eye[2][96];
static bool pet_eye_ready;
static uint16_t *pet_pixels;
static lv_area_t pet_bounds={320,320,-1,-1},pet_damage={0,0,319,319};
static lv_image_dsc_t pet_image;
static lv_obj_t *pet_images[9],*pet_labels[9],*pet_notice,*pet_notice_heading,*pet_notice_detail;
static unsigned pet_rand(void){
#ifndef BERRY_SIMULATOR
    pet_random_state^=esp_random();
#endif
    pet_random_state^=pet_random_state<<13;pet_random_state^=pet_random_state>>17;pet_random_state^=pet_random_state<<5;return pet_random_state;
}
static unsigned pet_pick(const unsigned *pool,unsigned count){unsigned pick=pool[pet_rand()%count];if(count>1 && pick==pet_last_expression)for(unsigned i=0;i<count;i++)if(pool[i]!=pick){pick=pool[i];break;}pet_last_expression=pick;return pick;}
static void pet_action(bool greeting){
    static const unsigned idle[]={0,8,15,19,24},hello[]={2,3,8,17,21},busy[]={7,16,10},hot[]={3,21},lost[]={14,5,8},sleepy[]={4,13,22};
    unsigned now=lv_tick_get();
    if(pet_scene==PET_HOT)pet_expression=pet_pick(hot,2);
    else if(pet_scene==PET_BUSY)pet_expression=pet_pick(busy,3);
    else if(pet_scene==PET_OFFLINE)pet_expression=pet_pick(lost,3);
    else if((int32_t)(pet_recovery_until-now)>0 || greeting)pet_expression=pet_pick(hello,5);
    else if(pet_scene==PET_UNKNOWN)pet_expression=pet_pick(lost,3);
    else if(lv_display_get_inactive_time(lv_display_get_default())>60000)pet_expression=pet_pick(sleepy,3);
    else pet_expression=pet_pick(idle,5);
    pet_action_until=now+2200;pet_next_action=now+6000+pet_rand()%7000;
    pet_motion=(pet_scene==PET_HOT||pet_scene==PET_OFFLINE)?0:pet_rand()%3;
    pet_blink_at=now+3500+pet_rand()%5500;
}
static bool pet_say_status(void);
static void pet_policy_update(const cJSON *data,unsigned elapsed){
    int old=pet_scene;
    bool fresh=strcmp(str(data,"status","unknown"),"fresh")==0;
    const cJSON *pc=fresh?item(data,"dorm_pc"):NULL,*cloud=fresh?item(data,"aliyun"):NULL;
    const cJSON *metric[]={item(pc,"cpu"),item(pc,"gpu"),item(cloud,"cpu")};
    for(int i=0;i<3;i++){int n=num(metric[i],"usage_percent");pet_busy_ms[i]=(n>=95&&n<=100)?(pet_busy_ms[i]<300000?pet_busy_ms[i]+elapsed:300000):0;}
    for(int i=0;i<2;i++){int n=num(metric[i],"temperature_c");pet_hot_ms[i]=(n>=90&&n<=150)?(pet_hot_ms[i]<30000?pet_hot_ms[i]+elapsed:30000):0;}
    const cJSON *components=fresh?item(item(data,"zhiqing"),"components"):NULL;
    bool offline=strcmp(str(components,"process",""),"offline")==0||strcmp(str(components,"qq_connection",""),"offline")==0;
    pet_offline_ms=offline?(pet_offline_ms<30000?pet_offline_ms+elapsed:30000):0;
    pet_scene=PET_IDLE;pet_source=-1;pet_value=-1;
    if(pet_hot_ms[0]>=30000||pet_hot_ms[1]>=30000){pet_scene=PET_HOT;pet_source=pet_hot_ms[0]>=30000?0:1;pet_value=num(metric[pet_source],"temperature_c");}
    else if(pet_offline_ms>=30000)pet_scene=PET_OFFLINE;
    else for(int i=0;i<3;i++)if(pet_busy_ms[i]>=300000){pet_scene=PET_BUSY;pet_source=i;pet_value=num(metric[i],"usage_percent");break;}
    if(pet_scene==PET_IDLE && (!fresh || num(metric[0],"usage_percent")<0 || num(metric[1],"usage_percent")<0 || strcmp(str(cloud,"status","unknown"),"available")!=0))pet_scene=PET_UNKNOWN;
    /* Unknown samples end continuity, but never prove a recovery. */
    if(old!=pet_scene){
        pet_previous_scene=old;
        if((old==PET_BUSY||old==PET_HOT||old==PET_OFFLINE)&&pet_scene==PET_IDLE)pet_recovery_until=lv_tick_get()+4500;
        else if(pet_scene!=PET_IDLE)pet_recovery_until=0;
        pet_action(false);
        if(pet_scene==PET_HOT||pet_scene==PET_BUSY||pet_scene==PET_OFFLINE||old==PET_HOT||old==PET_BUSY||old==PET_OFFLINE)pet_say_status();
        if(pet_scene!=PET_HOT && pet_scene!=PET_OFFLINE && pet_scene!=PET_BUSY)pet_notice_scene=-1;
        if((pet_scene==PET_HOT||pet_scene==PET_OFFLINE||pet_scene==PET_BUSY)&&pet_notice_scene!=pet_scene && (!pet_last_notice||lv_tick_get()-pet_last_notice>=300000)){
            pet_notice_until=lv_tick_get()+6000;pet_last_notice=lv_tick_get();pet_notice_scene=pet_scene;
            pet_notice_target=pet_scene==PET_OFFLINE?2:pet_source==2?3:1;pet_text(pet_notice_text,sizeof(pet_notice_text));
        }
    }
}
static void pet_text(char *out,size_t size){
    const char *names[]={"CPU","GPU","服务器 CPU"};
    if(pet_scene==PET_HOT)snprintf(out,size,"%s 温度偏高\n当前 %d℃",names[pet_source],pet_value);
    else if(pet_scene==PET_BUSY)snprintf(out,size,"%s 持续高负载 5 分钟\n当前占用 %d%%",names[pet_source],pet_value);
    else if(pet_scene==PET_OFFLINE)snprintf(out,size,"芷清连接报告离线\n查看芷清状态");
    else if(pet_scene==PET_UNKNOWN)snprintf(out,size,"部分数据暂缺");
    else if((int32_t)(pet_recovery_until-lv_tick_get())>0)snprintf(out,size,"恢复平稳啦");
    else snprintf(out,size,"陪你待一会儿");
}
#include "berry_pet_bubbles.inc"
/* Use the same RGB565 quantization as the display so the square stays invisible. */
#define PET_RGB565(c) ((((c)>>8)&0xf800)|(((c)>>5)&0x07e0)|(((c)>>3)&0x001f))
static void pet_fill_polygon(float point[96],float blink){
    float cx=0,cy=0;for(int p=0;p<48;p++){cx+=point[p*2];cy+=point[p*2+1];}cx/=48;cy/=48;
    float normalized=fmaxf(-1,fminf(1,(cx-114.2705f)/105.f));
    float base=asinf(normalized),longitude=base+pet_turn,depth=cosf(longitude);
    if(depth<=.02f)return;
    float projected=114.2705f+105*sinf(longitude)+pet_gaze;
    float perspective=fminf(2.4f,fmaxf(.02f,depth)/fmaxf(.02f,cosf(base)));
    float x[48],y[48];int ymin=320,ymax=0;
    for(int p=0;p<48;p++){x[p]=160+(projected+(point[p*2]-cx)*perspective-114.2705f)*1.225f;y[p]=160+(cy+(point[p*2+1]-cy)*blink-114.2705f)*1.225f;ymin=(int)fminf(ymin,y[p]);ymax=(int)fmaxf(ymax,y[p]);}
    for(int p=0;p<48;p++){int xx=(int)x[p],yy=(int)y[p];if(xx<pet_bounds.x1)pet_bounds.x1=xx;if(xx>pet_bounds.x2)pet_bounds.x2=xx;if(yy<pet_bounds.y1)pet_bounds.y1=yy;if(yy>pet_bounds.y2)pet_bounds.y2=yy;}
    if(ymin<0)ymin=0;
    if(ymax>319)ymax=319;
    uint16_t eye_color=light_theme?0xffff:0x18c3;
    for(int row=ymin;row<=ymax;row++){
        float intersections[48];int count=0;float sy=row+.5f;
        for(int p=0,q=47;p<48;q=p++)if((y[p]<=sy&&y[q]>sy)||(y[q]<=sy&&y[p]>sy))intersections[count++]=x[p]+(sy-y[p])*(x[q]-x[p])/(y[q]-y[p]);
        for(int i=1;i<count;i++){float v=intersections[i];int j=i;while(j>0&&intersections[j-1]>v){intersections[j]=intersections[j-1];j--;}intersections[j]=v;}
        for(int i=0;i+1<count;i+=2){int from=(int)ceilf(intersections[i]),to=(int)floorf(intersections[i+1]);if(from<0)from=0;if(to>319)to=319;for(int px=from;px<=to;px++)if((px-160)*(px-160)+(row-160)*(row-160)<=140*140)pet_pixels[row*320+px]=eye_color;}
    }
}
static void pet_paint(void){
    if(!pet_pixels)return;
    lv_area_t previous=pet_bounds;pet_bounds=(lv_area_t){320,320,-1,-1};
    uint16_t outside=light_theme?PET_RGB565(BG):0x18c4;
    uint16_t body=light_theme?PET_RGB565(0x101214):0xe73d;
    uint16_t edge=light_theme?PET_RGB565(0x26303b):body;
    for(int y=0;y<320;y++)for(int x=0;x<320;x++){
        int r2=(x-160)*(x-160)+(y-160)*(y-160);
        pet_pixels[y*320+x]=r2>140*140?outside:r2>138*138?edge:body;
    }
    unsigned now=lv_tick_get();float blink=1;
    pet_frames++;pet_turn=0;pet_gaze=0;
    if((int32_t)(pet_action_until-now)>0){float phase=1-(pet_action_until-now)/2200.f;if(pet_motion==1)pet_turn=6.2831853f*(phase*phase*(3-2*phase));else if(pet_motion==2)pet_gaze=13.2f*sinf(phase*6.2831853f);}
    if(now>=pet_blink_at){pet_blink_start=now;pet_blink_at=now+5000+pet_rand()%6000;}
    unsigned age=now-pet_blink_start;if(pet_blink_start&&age<320){blink=age<130?1-age/130.f:.04f+(age-130)/190.f;if(blink<.04f)blink=.04f;}
    for(int e=0;e<2;e++){for(int p=0;p<96;p++){float target=berry_pet_rings[pet_expression][e][p]/100.f;if(!pet_eye_ready)pet_eye[e][p]=target;else pet_eye[e][p]+=(target-pet_eye[e][p])*.18f;}pet_fill_polygon(pet_eye[e],blink);}
    pet_eye_ready=true;
    pet_damage=pet_bounds;
    if(previous.x1<pet_damage.x1)pet_damage.x1=previous.x1;
    if(previous.y1<pet_damage.y1)pet_damage.y1=previous.y1;
    if(previous.x2>pet_damage.x2)pet_damage.x2=previous.x2;
    if(previous.y2>pet_damage.y2)pet_damage.y2=previous.y2;
    pet_damage.x1=pet_damage.x1<2?0:pet_damage.x1-2;pet_damage.y1=pet_damage.y1<2?0:pet_damage.y1-2;
    pet_damage.x2=pet_damage.x2>317?319:pet_damage.x2+2;pet_damage.y2=pet_damage.y2>317?319:pet_damage.y2+2;
}
static void pet_tap(lv_event_t *e){(void)e;if(page==6&&!overlay_open())pet_on_tap();}
static void draw_pet(void){
    clock_label=NULL;
    if(!pet_pixels){
#ifndef BERRY_SIMULATOR
        pet_pixels=heap_caps_malloc(320*320*2,MALLOC_CAP_SPIRAM);
#else
        pet_pixels=malloc(320*320*2);
#endif
        if(pet_pixels){pet_image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,.w=320,.h=320,.stride=640},.data_size=320*320*2,.data=(const uint8_t *)pet_pixels};pet_paint();}
    }
    if(pet_pixels){lv_obj_t *o=lv_image_create(root);lv_image_set_src(o,&pet_image);lv_obj_set_pos(o,80,112);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);lv_obj_add_event_cb(o,pet_tap,LV_EVENT_SHORT_CLICKED,NULL);pet_images[active_tile]=o;}
    pet_bubble_create();
}
static void pet_tick(lv_timer_t *timer){
    (void)timer;
    if(page!=6||pager_busy()||overlay_open())return;
#ifndef BERRY_SIMULATOR
    if(berry_power_screen_off())return;
#endif
    if(lv_tick_get()>=pet_next_action)pet_action(false);
    pet_paint();
    lv_obj_t *sprite=pet_images[active_tile];
    if(sprite&&lv_obj_is_valid(sprite)&&pet_damage.x2>=pet_damage.x1&&pet_damage.y2>=pet_damage.y1){lv_area_t area;lv_obj_get_coords(sprite,&area);int x=area.x1,y=area.y1;area=(lv_area_t){x+pet_damage.x1,y+pet_damage.y1,x+pet_damage.x2,y+pet_damage.y2};lv_obj_invalidate_area(sprite,&area);}
    if(!notice_overlay_visible())pet_bubble_apply();
    else if(pet_bubbles[active_tile]&&lv_obj_is_valid(pet_bubbles[active_tile]))lv_obj_add_flag(pet_bubbles[active_tile],LV_OBJ_FLAG_HIDDEN);
}
