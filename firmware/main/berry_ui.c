#include "berry_ui.h"
#ifndef BERRY_SIMULATOR
#include "berry_pairing.h"
#include "berry_panel.h"
#include "berry_wifi.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "nvs.h"
#include "berry_power.h"
#include "berry_pickup.h"
#endif
#include "berry_serial.h"
#include "berry_spinner_motion.h"
#include "berry_visual_logic.h"
#include "berry_schedule.h"
#include "qr_display.h"
#include <time.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const lv_font_t berry_font_16;
extern const lv_font_t berry_font_20;
extern const lv_font_t berry_temp_16,berry_temp_24,berry_temp_48;
extern const lv_image_dsc_t berry_portrait;
extern const lv_image_dsc_t icon_openai,icon_monitor,icon_user,icon_server,icon_weather,icon_calendar;
extern const lv_image_dsc_t icon_cpu,icon_disk,icon_ram,icon_clock,icon_ssh,icon_relay,icon_network,icon_up,icon_down,icon_qq;
extern const lv_image_dsc_t icon_process,icon_message;
extern const lv_image_dsc_t icon_sun,icon_partly,icon_cloud,icon_rain,icon_snow,icon_storm,icon_fog;
static lv_obj_t *root;
static lv_obj_t *clock_label;
#ifndef BERRY_SIMULATOR
static bool pairing_screen;static char pairing_name[32],pairing_proof[17];
static lv_obj_t *panel_wifi_label,*panel_keyboard_object;
static int panel_screen;static berry_panel_state_t panel_last;static char chosen_ssid[33];static bool chosen_secured;static lv_obj_t *password_input;static unsigned panel_refresh;
#endif
#ifndef BERRY_SIMULATOR
static void render(void);
static bool saver_enabled;
static int panel_delete_index=-1;
static void saver_toggle(lv_event_t *e) {
    (void)e;
    bool wanted=!saver_enabled;
    nvs_handle_t handle;
    esp_err_t rc=nvs_open("berry_ui",NVS_READWRITE,&handle);
    if(rc==ESP_OK) { rc=nvs_set_u8(handle,"screensaver",wanted); if(rc==ESP_OK)rc=nvs_commit(handle); nvs_close(handle); }
    if(rc==ESP_OK)saver_enabled=wanted;
    render();
}
static bool saver_due(bool enabled, bool panel, bool pairing, bool powered_off, uint32_t idle_ms) { return enabled && !panel && !pairing && !powered_off && idle_ms>=120000; }

#endif
static unsigned page;
static lv_obj_t *pager,*tiles[9],*panel_root;
static unsigned active_tile=1;
static void render(void);
static bool overlay_open(void);
static bool notice_overlay_visible(void);
static void notice_jump(unsigned target);
static bool pager_busy(void);
static void draw_page(void);
static void theme_toggle(lv_event_t *event);

static volatile unsigned ui_ticks,ui_updates,ui_page;
static volatile int ui_quota=-1,ui_cpu=-1;
static int selected_day;
static int source_day;
#ifndef BERRY_SIMULATOR
static EXT_RAM_BSS_ATTR char last_data[8192];
static EXT_RAM_BSS_ATTR char received_data[8192];
#else
static char last_data[8192];
static char received_data[8192];
#endif
static TickType_t last_received;
LV_DRAW_BUF_DEFINE_STATIC(login_qr_buffer, 300, 300, LV_COLOR_FORMAT_I1);
static bool login_qr_visible;
static bool login_entry_visible;
static unsigned clock_minutes;
static bool clock_valid;
static int rate_history[3][16];
static int rate_count[3];
static char last_cloud_update[40];
static lv_point_precise_t rate_points[3][16];

/* The build flag is the default for devices without a saved choice. */
#ifndef BERRY_LIGHT_THEME
#define BERRY_LIGHT_THEME 0
#endif
static bool light_theme=BERRY_LIGHT_THEME!=0;
typedef struct {
    uint32_t bg,card,border,text,muted,accent,dim,green,red,amber,status_idle,page_idle,selected;
} berry_palette_t;
static const berry_palette_t palettes[2]={
    [0]={.bg=0x191b20,.card=0x242d38,.border=0x34495e,.text=0xe4edf8,.muted=0xb2c5d9,.accent=0x69c2fa,
         .dim=0x38485c,.green=0x66d9aa,.red=0xf16d7b,.amber=0xffc568,.status_idle=0x8391a7,.page_idle=0x5c6c80,.selected=0x3c6383},
    [1]={.bg=0xeaf0f6,.card=0xffffff,.border=0xd3dfea,.text=0x213449,.muted=0x50667b,.accent=0x2a815c,
         .dim=0xdce9e3,.green=0x207759,.red=0xaf4051,.amber=0x99601a,.status_idle=0x6f8397,.page_idle=0x899daf,.selected=0xd9eee2}
};
#define BG (palettes[light_theme].bg)
#define CARD (palettes[light_theme].card)
#define BORDER (palettes[light_theme].border)
#define TEXT (palettes[light_theme].text)
#define MUTED (palettes[light_theme].muted)
#define BLUE (palettes[light_theme].accent)
#define DIM (palettes[light_theme].dim)
#define GREEN (palettes[light_theme].green)
#define RED (palettes[light_theme].red)
#define AMBER (palettes[light_theme].amber)
#define STATUS_IDLE (palettes[light_theme].status_idle)
#define PAGE_IDLE (palettes[light_theme].page_idle)
#define SELECTED (palettes[light_theme].selected)

static const cJSON *item(const cJSON *obj, const char *key) {
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj,key) : NULL;
}
static int num(const cJSON *obj, const char *key) {
    const cJSON *v=item(obj,key);
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 2147483647.0 ? (int)v->valuedouble : -1;
}
static const char *str(const cJSON *obj, const char *key, const char *fallback) {
    const cJSON *v=item(obj,key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : fallback;
}
static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *o=lv_label_create(parent); lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_label_set_long_mode(o,LV_LABEL_LONG_DOT); lv_label_set_text(o,text);
    lv_obj_add_flag(o,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE); return o;
}
static lv_obj_t *box(int x, int y, int w, int h) {
    lv_obj_t *o=lv_obj_create(root); lv_obj_remove_style_all(o); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(CARD),0); lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(o,1,0); lv_obj_set_style_border_color(o,lv_color_hex(BORDER),0);
    lv_obj_set_style_radius(o,12,0); lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE); return o;
}
static void right(lv_obj_t *o) { lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_RIGHT,0); }
static void pct_text(char *buf, size_t n, int value) { if(value>=0 && value<=100) snprintf(buf,n,"%d%%",value); else snprintf(buf,n,"--"); }
static const lv_font_t *token_font(const char *text) {
    lv_point_t size;
    lv_text_get_size(&size,text,&lv_font_montserrat_48,0,0,32767,LV_TEXT_FLAG_NONE);
    if(size.x<=260)return &lv_font_montserrat_48;
    lv_text_get_size(&size,text,&lv_font_montserrat_28,0,0,32767,LV_TEXT_FLAG_NONE);
    return size.x<=260?&lv_font_montserrat_28:&lv_font_montserrat_24;
}
static void image(lv_obj_t *parent,int x,int y,const lv_image_dsc_t *src) {
    lv_obj_t *o=lv_image_create(parent);lv_image_set_src(o,src);lv_obj_set_pos(o,x,y);
    if(light_theme) {
        lv_obj_set_style_image_recolor(o,lv_color_hex(MUTED),0);
        lv_obj_set_style_image_recolor_opa(o,LV_OPA_COVER,0);
    }
    lv_obj_add_flag(o,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
}
static void bar(lv_obj_t *parent, int x, int y, int w, int h, int value, int count) {
    if(count==1) {
        lv_obj_t *track=lv_obj_create(parent);lv_obj_remove_style_all(track);lv_obj_set_pos(track,x,y);lv_obj_set_size(track,w,h);lv_obj_set_style_bg_opa(track,LV_OPA_COVER,0);lv_obj_set_style_bg_color(track,lv_color_hex(DIM),0);lv_obj_clear_flag(track,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
        if(value>0 && value<=100){lv_obj_t *fill=lv_obj_create(track);lv_obj_remove_style_all(fill);lv_obj_set_size(fill,w*value/100,h);lv_obj_set_style_bg_opa(fill,LV_OPA_COVER,0);lv_obj_set_style_bg_color(fill,lv_color_hex(BLUE),0);lv_obj_clear_flag(fill,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);}return;
    }
    int gap=2, width=(w-gap*(count-1))/count;
    for(int i=0;i<count;i++) {
        lv_obj_t *o=lv_obj_create(parent); lv_obj_remove_style_all(o); lv_obj_set_pos(o,x+i*(width+gap),y); lv_obj_set_size(o,width,h);
        lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0); lv_obj_set_style_radius(o,2,0);
        lv_obj_set_style_bg_color(o,lv_color_hex(value>=0 && value<=100 && i*100<value*count ? BLUE : DIM),0);
        lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
}
static void header(const char *title, const char *status) {
    const lv_image_dsc_t *icon=strcmp(title,"Codex")==0?&icon_openai:strcmp(title,"宿舍电脑")==0?&icon_monitor:strcmp(title,"芷清")==0?&icon_user:strcmp(title,"云服务器")==0?&icon_server:strcmp(title,"天气")==0?&icon_weather:&icon_calendar;
    image(root,20,16,icon);
    label(root,53,strcmp(title,"Codex")==0?16:strcmp(title,"天气")==0||strcmp(title,"芷清")==0?20:18,255,title,strcmp(title,"Codex")==0?&lv_font_montserrat_24:&berry_font_20,TEXT);
    if(status && strcmp(title,"天气")==0) {
        lv_obj_t *location=label(root,170,20,140,status,&berry_font_16,MUTED);
        lv_obj_set_style_text_align(location,LV_TEXT_ALIGN_CENTER,0);
    } else if(status) {
        int x=strcmp(title,"芷清")==0?132:190;
        lv_obj_t *dot=lv_obj_create(root);lv_obj_remove_style_all(dot);lv_obj_set_pos(dot,x,24);lv_obj_set_size(dot,10,10);
        lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(dot,lv_color_hex(strcmp(status,"在线")==0||strcmp(status,"正常")==0||strcmp(status,"可用")==0?GREEN:strcmp(status,"异常")==0?RED:STATUS_IDLE),0);
        lv_obj_clear_flag(dot,LV_OBJ_FLAG_CLICKABLE);label(root,x+15,20,100,status,&berry_font_16,MUTED);
    }
    clock_label=label(root,350,14,105,"--:--",&lv_font_montserrat_24,TEXT); right(clock_label);
}
static void reset_text(char *out,size_t size,const char *stamp,bool weekly) {
    int year,month,day,hour,minute;
    if(sscanf(stamp,"%d-%d-%dT%d:%d",&year,&month,&day,&hour,&minute)!=5 || month<1 || month>12 || day<1 || day>31 || hour<0 || hour>23 || minute<0 || minute>59){snprintf(out,size,"重置 · --");return;}
    hour+=8;if(hour>=24){hour-=24;day++;int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};if(year%4==0 && (year%100!=0 || year%400==0))lengths[1]=29;if(day>lengths[month-1]){day=1;month=month%12+1;}}
    if(weekly)snprintf(out,size,"重置 · %d月%d日 %02d:%02d",month,day,hour,minute);else snprintf(out,size,"重置 · %02d:%02d",hour,minute);
}
static void indicator(void) {
    for(unsigned i=0;i<7;i++) {lv_obj_t *d=lv_obj_create(root);lv_obj_remove_style_all(d);lv_obj_set_pos(d,184+(int)i*17,460);lv_obj_set_size(d,7,7);lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(d,LV_OPA_COVER,0);lv_obj_set_style_bg_color(d,lv_color_hex(i==page?(light_theme?BLUE:TEXT):PAGE_IDLE),0);lv_obj_clear_flag(d,LV_OBJ_FLAG_CLICKABLE);}
}
static void rotate_task_indicator(void *object, int32_t value) {
    (void)value; lv_arc_set_rotation((lv_obj_t *)object,berry_spinner_angle(lv_tick_get()));
}
static void draw_task_indicator(lv_obj_t *parent, int y) {
    lv_obj_t *arc=lv_arc_create(parent);lv_obj_set_pos(arc,414,y-2);lv_obj_set_size(arc,16,16);
    lv_obj_set_style_pad_all(arc,0,0);lv_obj_set_style_arc_width(arc,2,LV_PART_MAIN);lv_obj_set_style_arc_width(arc,2,LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc,lv_color_hex(DIM),LV_PART_MAIN);lv_obj_set_style_arc_color(arc,lv_color_hex(BLUE),LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc,0,360);lv_arc_set_angles(arc,0,100);lv_obj_remove_style(arc,NULL,LV_PART_KNOB);lv_obj_clear_flag(arc,LV_OBJ_FLAG_CLICKABLE);
    rotate_task_indicator(arc,0);lv_anim_t anim;lv_anim_init(&anim);lv_anim_set_var(&anim,arc);lv_anim_set_exec_cb(&anim,rotate_task_indicator);
    lv_anim_set_values(&anim,0,360);lv_anim_set_duration(&anim,1600);lv_anim_set_path_cb(&anim,lv_anim_path_linear);lv_anim_set_repeat_count(&anim,LV_ANIM_REPEAT_INFINITE);lv_anim_start(&anim);
}
static void draw_codex(const cJSON *data) {
    header("Codex",NULL); const cJSON *codex=item(data,"codex"), *windows=item(codex,"quota_windows"), *tasks=item(codex,"recent_tasks");
    for(int i=0;i<2;i++) {
        const cJSON *q=cJSON_GetArrayItem(windows,i); int value=num(q,"remaining_percent"); char text[32]; pct_text(text,sizeof(text),value);
        lv_obj_t *b=box(20,69+i*92,440,85); if(i)label(b,10,8,270,"周额度",&berry_font_20,TEXT);else {label(b,10,10,18,"5",&berry_font_20,TEXT);label(b,24,8,250,"小时额度",&berry_font_20,TEXT);} right(label(b,330,8,98,text,&lv_font_montserrat_24,TEXT));
        bar(b,10,43,418,12,value,30); char reset[80];reset_text(reset,sizeof(reset),str(q,"resets_at",""),i!=0);label(b,10,60,350,reset,&berry_font_16,MUTED);
    }
    lv_obj_t *list=box(20,253,440,174);
    for(int i=0;i<4;i++) {
        const cJSON *t=cJSON_GetArrayItem(tasks,i); const char *name=str(t,"title","暂无任务数据"); const char *s=str(t,"status","unknown");
        const char *st=strcmp(s,"running")==0?"运行中":strcmp(s,"waiting_approval")==0?"等待批准":strcmp(s,"blocked")==0?"等待输入":strcmp(s,"error")==0?"遇到问题":"";
        lv_obj_t *task_name=label(list,10,17+i*41,310,name,&berry_font_16,TEXT);lv_label_set_long_mode(task_name,LV_LABEL_LONG_SCROLL_CIRCULAR);right(label(list,318,17+i*41,94,st,&berry_font_16,MUTED));
        if(strcmp(s,"running")==0){draw_task_indicator(list,19+i*41);}
        else if(strcmp(s,"waiting_approval")==0||strcmp(s,"blocked")==0||strcmp(s,"error")==0||strcmp(s,"completed")==0){lv_obj_t *light=lv_obj_create(list);lv_obj_remove_style_all(light);lv_obj_set_pos(light,418,21+i*41);lv_obj_set_size(light,8,8);lv_obj_set_style_radius(light,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(light,LV_OPA_COVER,0);lv_obj_set_style_bg_color(light,lv_color_hex(strcmp(s,"error")==0?RED:strcmp(s,"completed")==0?BLUE:AMBER),0);lv_obj_clear_flag(light,LV_OBJ_FLAG_CLICKABLE);}
        if(i<3){lv_obj_t *line=lv_obj_create(list);lv_obj_remove_style_all(line);lv_obj_set_pos(line,10,46+i*41);lv_obj_set_size(line,418,1);lv_obj_set_style_bg_color(line,lv_color_hex(BORDER),0);lv_obj_set_style_bg_opa(line,LV_OPA_COVER,0);lv_obj_clear_flag(line,LV_OBJ_FLAG_CLICKABLE);}
    }
}
static void draw_dorm(const cJSON *data) {
    const cJSON *pc=item(data,"dorm_pc"); header("宿舍电脑",num(item(pc,"cpu"),"usage_percent")>=0?"正常":"状态未知");
    const char *keys[]={"cpu","gpu","memory"}, *names[]={"CPU","GPU","内存"};
    for(int i=0;i<3;i++) {
        const cJSON *m=item(pc,keys[i]); int v=num(m,"usage_percent"); char text[32]; pct_text(text,sizeof(text),v);
        const cJSON *usage=item(m,"usage_percent");if(i==1 && cJSON_IsNumber(usage) && usage->valuedouble>0 && usage->valuedouble<1)snprintf(text,sizeof(text),"<1%%");
        lv_obj_t *b=box(20,58+i*109,440,101);
        if(i<2){
            label(b,12,30,70,names[i],&lv_font_montserrat_24,TEXT);
            lv_obj_t *arc=lv_arc_create(b);lv_obj_set_pos(arc,82,16);lv_obj_set_size(arc,82,82);lv_arc_set_bg_angles(arc,180,360);lv_arc_set_range(arc,0,100);lv_arc_set_value(arc,v>=0&&v<=100?v:0);lv_obj_set_style_arc_width(arc,8,LV_PART_MAIN);lv_obj_set_style_arc_width(arc,8,LV_PART_INDICATOR);lv_obj_set_style_arc_color(arc,lv_color_hex(DIM),LV_PART_MAIN);lv_obj_set_style_arc_color(arc,lv_color_hex(BLUE),LV_PART_INDICATOR);lv_obj_set_style_arc_rounded(arc,true,LV_PART_MAIN);lv_obj_set_style_arc_rounded(arc,true,LV_PART_INDICATOR);lv_obj_remove_style(arc,NULL,LV_PART_KNOB);lv_obj_clear_flag(arc,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_flag(arc,LV_OBJ_FLAG_GESTURE_BUBBLE);
            label(b,186,38,64,text,&lv_font_montserrat_24,TEXT);label(b,254,18,65,"温度",&berry_font_16,MUTED);int temp=num(m,"temperature_c");char reading[24];if(temp>=0)snprintf(reading,sizeof(reading),"%d℃",temp);else snprintf(reading,sizeof(reading),"--℃");right(label(b,327,12,100,reading,&berry_temp_24,TEXT));
            bar(b,254,70,173,12,berry_temperature_bar(temp),16);
        }else{
            label(b,12,18,75,names[i],&berry_font_20,TEXT);label(b,90,16,72,text,&lv_font_montserrat_24,TEXT);
            const cJSON *used=item(m,"used_bytes"),*total=item(m,"total_bytes");char details[80];if(cJSON_IsNumber(used)&&cJSON_IsNumber(total))snprintf(details,sizeof(details),"%.1f / %.1f GB",used->valuedouble/1073741824.0,total->valuedouble/1073741824.0);else snprintf(details,sizeof(details),"容量未知");right(label(b,207,23,220,details,&berry_font_20,TEXT));bar(b,12,62,414,14,v,24);
        }
    }
    lv_obj_t *u=box(20,389,440,40); label(u,12,12,120,"运行时间",&berry_font_16,MUTED);int seconds=num(pc,"uptime_seconds");char uptime[64];if(seconds>=0)snprintf(uptime,sizeof(uptime),"%d 天 %d 小时",seconds/86400,(seconds/3600)%24);else snprintf(uptime,sizeof(uptime),"--");right(label(u,200,12,225,uptime,&berry_font_16,TEXT));
}
static void draw_zhiqing(const cJSON *data) {
    const cJSON *z=item(data,"zhiqing"), *components=item(z,"components"); const char *process=str(components,"process","unknown"),*qq=str(components,"qq_connection","unknown"),*model=str(components,"model_api","unknown");const char *state=strcmp(process,"offline")==0||strcmp(qq,"offline")==0||strcmp(model,"offline")==0?"异常":strcmp(process,"healthy")==0&&strcmp(qq,"healthy")==0&&strcmp(model,"healthy")==0?"可用":"状态未知";header("芷清",state);
    const cJSON *avg=item(z,"chat_request_average"), *total=item(z,"observed_tokens");char avg_text[32]="--",total_text[32]="--";
    if(cJSON_IsNumber(avg) && avg->valuedouble>=0 && avg->valuedouble<=1e12)berry_grouped_number(avg_text,sizeof(avg_text),(uint64_t)avg->valuedouble);
    if(cJSON_IsNumber(total) && total->valuedouble>=0 && total->valuedouble<=1e12)berry_grouped_number(total_text,sizeof(total_text),(uint64_t)total->valuedouble);
    label(root,20,55,220,"此轮消耗 token",&berry_font_20,MUTED); label(root,20,80,260,avg_text,token_font(avg_text),TEXT);
    label(root,20,142,220,"累计消耗 token",&berry_font_20,MUTED); label(root,20,167,260,total_text,token_font(total_text),TEXT);
    lv_obj_t *portrait_box=lv_obj_create(root);lv_obj_remove_style_all(portrait_box);lv_obj_set_pos(portrait_box,287,57);lv_obj_set_size(portrait_box,170,164);lv_obj_set_style_radius(portrait_box,20,0);lv_obj_set_style_clip_corner(portrait_box,true,0);lv_obj_clear_flag(portrait_box,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(portrait_box,LV_OBJ_FLAG_GESTURE_BUBBLE);lv_obj_t *picture=lv_image_create(portrait_box); lv_image_set_src(picture,&berry_portrait); lv_obj_set_pos(picture,3,0); lv_obj_add_flag(picture,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *a=box(20,231,440,42);int age=num(z,"last_reply_seconds_ago"),rounds=num(z,"today_confirmed_rounds");char recent[60],today[60];if(age<0)snprintf(recent,sizeof(recent),"最近回复 --");else if(age<3600)snprintf(recent,sizeof(recent),"最近回复 %d 分钟前",age/60);else snprintf(recent,sizeof(recent),"最近回复 %d 小时前",age/3600);if(rounds<0)snprintf(today,sizeof(today),"未知");else snprintf(today,sizeof(today),"%d 轮",rounds);label(a,10,13,420,recent,&berry_font_16,MUTED);
    const char *keys[]={"process","qq_connection","model_api","message_processing"}; const char *names[]={"进程","QQ 连接","后端模型","消息轮次"};
    for(int i=0;i<4;i++) {lv_obj_t *b=box(20+(i%2)*224,283+(i/2)*81,216,65);image(b,12,21,i==0?&icon_process:i==1?&icon_qq:i==2?&icon_cpu:&icon_message);label(b,52,11,150,names[i],&berry_font_16,MUTED); const char *s=str(components,keys[i],"unknown");const char *value=i==3?today:i==2?str(z,"model_name","未知"):strcmp(s,"healthy")==0?"● 正常":strcmp(s,"offline")==0?"● 离线":"● 未知";label(b,52,35,150,value,&berry_font_16,i<2 && strcmp(s,"healthy")==0?GREEN:light_theme && i<2 && strcmp(s,"offline")==0?RED:MUTED);}
}
static void draw_cloud(const cJSON *data) {
    const cJSON *cloud=item(data,"aliyun"); header("云服务器",strcmp(str(cloud,"status","unknown"),"available")==0?"在线":"状态未知");
    const char *names[]={"CPU","内存","存储"}, *keys[]={"cpu","memory","storage"};
    for(int i=0;i<3;i++){lv_obj_t *b=box(20+i*149,57,142,95);int v=num(item(cloud,keys[i]),"usage_percent");char s[32];pct_text(s,sizeof(s),v);image(b,8,3,i==0?&icon_cpu:i==1?&icon_ram:&icon_disk);label(b,39,10,92,names[i],&berry_font_16,MUTED);label(b,10,36,120,s,&lv_font_montserrat_24,TEXT);bar(b,10,72,120,8,v,1);}
    lv_obj_t *u=box(20,162,440,45); image(u,8,10,&icon_clock);label(u,42,15,150,"运行时间",&berry_font_16,MUTED); int seconds=num(cloud,"uptime_seconds"); char uptime[60]; if(seconds>=0)snprintf(uptime,sizeof(uptime),"%d 天 %d 小时",seconds/86400,(seconds/3600)%24);else snprintf(uptime,sizeof(uptime),"--"); right(label(u,220,15,205,uptime,&berry_font_16,TEXT));
    for(int i=0;i<2;i++){lv_obj_t *b=box(20+i*224,217,216,70);image(b,9,21,i?&icon_relay:&icon_ssh);label(b,42,26,98,i?"远程中转":"SSH 服务",&berry_font_16,TEXT);bool healthy=i?strcmp(str(cloud,"relay_status","unknown"),"healthy")==0:strcmp(str(cloud,"status","unknown"),"available")==0;label(b,158,26,16,"●",&berry_font_16,healthy?GREEN:MUTED);right(label(b,175,26,32,healthy?"正常":"未知",&berry_font_16,MUTED));}
    for(int i=0;i<3;i++){lv_obj_t *b=box(20+i*149,297,142,132);image(b,7,5,i==0?&icon_network:i==1?&icon_up:&icon_down);label(b,37,12,96,i==0?"连接延迟":i==1?"上传速率":"下载速率",&berry_font_16,MUTED);char v[40]="--";int n=i==0?num(cloud,"latency_ms"):i==1?num(cloud,"upload_bytes_per_second"):num(cloud,"download_bytes_per_second");if(n>=0){if(i==0)snprintf(v,sizeof(v),"%d ms",n);else snprintf(v,sizeof(v),"%.1f KB/s",(double)n/1024);}label(b,10,52,120,v,&berry_font_16,TEXT);
        int index=i;if(n>=0 && rate_count[index]>=2){int peak=1;for(int j=0;j<rate_count[index];j++)if(rate_history[index][j]>peak)peak=rate_history[index][j];for(int j=0;j<rate_count[index];j++){rate_points[index][j].x=j*120/(rate_count[index]-1);rate_points[index][j].y=25-(int)((double)rate_history[index][j]*25/peak);}lv_obj_t *line=lv_line_create(b);lv_line_set_points(line,rate_points[index],rate_count[index]);lv_obj_set_pos(line,10,91);lv_obj_set_style_line_width(line,2,0);lv_obj_set_style_line_color(line,lv_color_hex(BLUE),0);}else if(n>=0 && rate_count[index]==1){lv_obj_t *dot=lv_obj_create(b);lv_obj_remove_style_all(dot);lv_obj_set_pos(dot,10,103);lv_obj_set_size(dot,4,4);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);lv_obj_set_style_bg_color(dot,lv_color_hex(BLUE),0);lv_obj_clear_flag(dot,LV_OBJ_FLAG_CLICKABLE);}else bar(b,10,106,120,3,-1,8);
    }
}
static const char *weather_name(int code) {
    if(code==0)return "晴";
    if(code==1 || code==2)return "多云";
    if(code==3)return "阴";
    if(code==45 || code==48)return "雾";
    if(code>=51 && code<=67)return "雨";
    if(code>=71 && code<=77)return "雪";
    if(code>=80 && code<=82)return "阵雨";
    if(code>=85 && code<=86)return "阵雪";
    if(code>=95 && code<=99)return "雷雨";
    return "未知";
}
static const lv_image_dsc_t *weather_icon(int code) {
    if(code==0)return &icon_sun;
    if(code==1 || code==2)return &icon_partly;
    if(code==3)return &icon_cloud;
    if(code==45 || code==48)return &icon_fog;
    if(code>=51 && code<=67)return &icon_rain;
    if(code>=71 && code<=77)return &icon_snow;
    if(code>=80 && code<=82)return &icon_rain;
    if(code>=85 && code<=86)return &icon_snow;
    if(code>=95 && code<=99)return &icon_storm;
    return NULL;
}
static void temperature(char *out,size_t size,const cJSON *v) {
    if(cJSON_IsNumber(v) && v->valuedouble>=-60 && v->valuedouble<=60)snprintf(out,size,"%.0f℃",v->valuedouble);else snprintf(out,size,"--℃");
}
static void draw_weather(const cJSON *data) {
    header("天气","翔安区"); const cJSON *w=item(data,"weather"),*forecast=item(w,"forecast");
    char temp[32]; temperature(temp,sizeof(temp),item(w,"temperature_c"));
    const lv_image_dsc_t *hero=weather_icon(num(w,"code"));if(hero){lv_obj_t *pic=lv_image_create(root);lv_image_set_src(pic,hero);if(num(w,"code")==0){lv_image_set_scale(pic,320);lv_obj_set_pos(pic,40,70);}else lv_obj_set_pos(pic,24,61);lv_obj_add_flag(pic,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);}lv_obj_t *temperature_label=label(root,145,65,190,temp,&berry_temp_48,TEXT);lv_obj_set_style_text_align(temperature_label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_letter_space(temperature_label,0,0);right(label(root,335,70,110,weather_name(num(w,"code")),&berry_font_20,TEXT));
    const cJSON *today=cJSON_GetArrayItem(forecast,0);char high[24],low[24],range[60];temperature(high,sizeof(high),item(today,"high"));temperature(low,sizeof(low),item(today,"low"));snprintf(range,sizeof(range),"%s ~ %s",low,high);lv_obj_t *range_label=label(root,290,120,160,range,&berry_temp_16,MUTED);right(range_label);lv_obj_set_style_text_letter_space(range_label,0,0);
    const char *names[]={"空气质量","紫外线","湿度","风速"};
    for(int i=0;i<4;i++){lv_obj_t *b=box(20+i*111,160,104,62);label(b,6,8,94,names[i],&berry_font_16,MUTED);int n=i==0?num(item(w,"air_quality"),"aqi"):i==2?num(w,"humidity_percent"):-1;char v[48];if(i==0 && n>=0)snprintf(v,sizeof(v),"%d %s",n,str(item(w,"air_quality"),"description",""));else if(i==1)snprintf(v,sizeof(v),"%s",str(w,"uv_text","--"));else if(i==3)snprintf(v,sizeof(v),"%s",str(w,"wind_text","--"));else if(n<0)snprintf(v,sizeof(v),"--");else snprintf(v,sizeof(v),"%d%%",n);label(b,6,34,94,v,&berry_font_16,TEXT);}
    label(root,20,238,200,"未来七天",&berry_font_16,MUTED);
    for(int i=0;i<7;i++){const cJSON *d=cJSON_GetArrayItem(forecast,i+1);lv_obj_t *b=box(20+i*64,268,59,106);const char *date=str(d,"date","--");if(strlen(date)>=10)date+=5;lv_obj_t *date_label=label(b,0,8,59,date,&berry_font_16,MUTED);lv_obj_set_style_text_align(date_label,LV_TEXT_ALIGN_CENTER,0);const lv_image_dsc_t *icon=weather_icon(num(d,"code"));if(icon){lv_obj_t *pic=lv_image_create(b);lv_image_set_src(pic,icon);lv_image_set_pivot(pic,0,0);if(num(d,"code")==0){lv_image_set_scale(pic,150);lv_obj_set_pos(pic,16,32);}else{lv_image_set_scale(pic,90);lv_obj_set_pos(pic,15,35);}}else label(b,4,34,52,"--",&berry_font_16,MUTED);char v[24];temperature(v,sizeof(v),item(d,"high"));lv_obj_t *forecast_temp=label(b,4,72,52,v,&berry_temp_16,TEXT);lv_obj_set_style_text_letter_space(forecast_temp,0,0);lv_obj_set_style_text_align(forecast_temp,LV_TEXT_ALIGN_CENTER,0);}
    lv_obj_t *notice=box(20,389,440,40);const char *warning=str(w,"warnings","unknown");label(notice,10,10,420,strcmp(warning,"none")==0?"墨迹天气预警：无":strcmp(warning,"present")==0?"墨迹天气预警：有":"墨迹天气预警：未知",&berry_font_16,MUTED);
}
static void render(void);
static void schedule_changed(void *value) { selected_day=(int)(intptr_t)value;render(); }
static void schedule_click(lv_event_t *event) { lv_async_call(schedule_changed,lv_event_get_user_data(event)); }
static void draw_schedule(const cJSON *data) {
    (void)data; header("课程表",NULL); cJSON *schedule=berry_schedule_create(time(NULL)); const cJSON *courses=item(schedule,"courses");
    if(selected_day<1 || selected_day>7)selected_day=1;
    if(strcmp(str(schedule,"status","unknown"),"available")!=0){lv_obj_t *b=box(20,75,440,330);label(b,18,28,400,"课表暂不可用",&berry_font_20,MUTED);cJSON_Delete(schedule);return;}
    int day=num(schedule,"day");if(day>=1 && day<=7 && day!=source_day){source_day=day;selected_day=day;}const char *names[]={"周一","周二","周三","周四","周五","周六","周日"};
    const cJSON *dates=item(schedule,"week_dates");
    for(int i=0;i<7;i++){
        lv_obj_t *b=box(20+i*64,56,58,54); if(i+1==selected_day)lv_obj_set_style_bg_color(b,lv_color_hex(SELECTED),0);
        if(light_theme&&i+1==selected_day)lv_obj_set_style_border_color(b,lv_color_hex(BLUE),0);
        if(i+1==day&&(!light_theme||i+1!=selected_day))lv_obj_set_style_border_color(b,lv_color_hex(AMBER),0);
        lv_obj_t *name=label(b,0,5,58,names[i],&berry_font_16,TEXT);lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
        const cJSON *d=cJSON_GetArrayItem(dates,i);char date[8];if(cJSON_IsNumber(d)&&d->valueint>=1&&d->valueint<=31)snprintf(date,sizeof(date),"%d",d->valueint);else snprintf(date,sizeof(date),"--");
        lv_obj_t *daylabel=label(b,0,27,58,date,&berry_font_16,TEXT);lv_obj_set_style_text_align(daylabel,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_add_event_cb(b,schedule_click,LV_EVENT_CLICKED,(void *)(intptr_t)(i+1));
    }
    const char *periods[]={"上午","下午","傍晚","晚上"};const char *times[]={"10:10–11:50","14:30–16:10","16:40–18:20","19:10–20:50"};
    int count=cJSON_GetArraySize(courses);
    for(int slot=0;slot<4;slot++){
        const cJSON *found=NULL;
        for(int i=0;i<count;i++){const cJSON *c=cJSON_GetArrayItem(courses,i);if(num(c,"day")!=selected_day)continue;const char *t=str(c,"time","");int h=-1;if(sscanf(t,"%d",&h)!=1)continue;int course_slot=h<12?0:h<16?1:h<19?2:3;if(course_slot==slot){found=c;break;}}
        lv_obj_t *b=box(20,118+slot*80,440,70);
        if(found){lv_obj_set_style_border_color(b,lv_color_hex(BLUE),0);lv_obj_set_style_border_width(b,2,0);lv_obj_set_style_border_side(b,LV_BORDER_SIDE_LEFT,0);}
        label(b,12,13,120,periods[slot],&berry_font_16,MUTED);
        label(b,12,39,125,found?str(found,"time",times[slot]):times[slot],&berry_font_16,MUTED);
        lv_obj_t *divider=lv_obj_create(b);lv_obj_remove_style_all(divider);lv_obj_set_pos(divider,140,12);lv_obj_set_size(divider,1,46);lv_obj_set_style_bg_color(divider,lv_color_hex(BORDER),0);lv_obj_set_style_bg_opa(divider,LV_OPA_COVER,0);
        lv_obj_t *course_name=label(b,154,13,270,found?str(found,"title","--"):"空",&berry_font_16,TEXT);lv_label_set_long_mode(course_name,LV_LABEL_LONG_SCROLL_CIRCULAR);
        if(found){lv_obj_t *location=label(b,154,39,270,str(found,"location","--"),&berry_font_16,MUTED);lv_label_set_long_mode(location,LV_LABEL_LONG_SCROLL_CIRCULAR);}
    }
    cJSON_Delete(schedule);
}
static bool valid_login_qr(const cJSON *data);
#include "berry_panel_ui.inc"
static bool valid_login_qr(const cJSON *data) {
    const cJSON *qr=item(data,"login_qr");
    const cJSON *expires=item(qr,"expires_at");
    int size=num(qr,"size");
    const char *bits=str(qr,"bits",NULL);
    time_t now=time(NULL);
    return now>=1700000000 && cJSON_IsNumber(expires) && expires->valuedouble>=1700000000.0 && expires->valuedouble<=4102444800.0 &&
        berry_qr_valid(size,bits,(int64_t)expires->valuedouble,(int64_t)now);
}
static void draw_login_qr(const cJSON *data) {
    const cJSON *qr=item(data,"login_qr");
    const int size=num(qr,"size"),scale=300/size,offset=(300-scale*size)/2;
    const char *bits=str(qr,"bits","");
    clock_label=NULL;
    label(root,24,32,430,"芷清需要登录",&berry_font_20,TEXT);
    label(root,24,65,430,"请用芷清的手机 QQ 扫码",&berry_font_16,MUTED);
    LV_DRAW_BUF_INIT_STATIC(login_qr_buffer);
    lv_obj_t *canvas=lv_canvas_create(root);
    lv_canvas_set_draw_buf(canvas,&login_qr_buffer);
    lv_obj_set_pos(canvas,90,105);
    lv_canvas_set_palette(canvas,0,lv_color32_make(255,255,255,255));
    lv_canvas_set_palette(canvas,1,lv_color32_make(0,0,0,255));
    unsigned char *pixels=lv_draw_buf_goto_xy(&login_qr_buffer,0,0);
    memset(pixels,0,(size_t)login_qr_buffer.header.stride*300);
    for(int y=0;y<size;y++)for(int x=0;x<size;x++)if(berry_qr_black(size,bits,x,y))
        for(int dy=0;dy<scale;dy++)for(int dx=0;dx<scale;dx++) {
            const int px=offset+x*scale+dx,py=offset+y*scale+dy;
            pixels[(size_t)py*login_qr_buffer.header.stride+px/8]|=(unsigned char)(0x80u>>(px%8));
        }
    lv_obj_invalidate(canvas);
    label(root,24,422,430,"二维码会自动更新",&berry_font_16,MUTED);
}
#include "berry_pet.inc"
#include "berry_task_notice.inc"
static void draw_page(void) {
    clock_label=NULL;
    lv_obj_clean(root);
    login_qr_visible=false;
#ifndef BERRY_SIMULATOR
    if(panel_screen==4){
        cJSON *data=last_data[0]?cJSON_Parse(last_data):NULL;
        if(valid_login_qr(data)){login_qr_visible=true;draw_login_qr(data);}
        else {clock_label=NULL;label(root,24,100,430,"二维码暂不可用",&berry_font_20,TEXT);label(root,24,140,430,"等待新的登录二维码",&berry_font_16,MUTED);}
        cJSON_Delete(data);
        lv_obj_t *back=lv_button_create(root);lv_obj_set_pos(back,370,8);lv_obj_set_size(back,90,40);lv_obj_set_style_bg_color(back,lv_color_hex(CARD),0);lv_obj_add_event_cb(back,panel_back,LV_EVENT_CLICKED,NULL);label(back,0,0,75,"返回",&berry_font_16,TEXT);
        return;
    }
    if(panel_screen){panel_draw();return;}
    if(pairing_screen){
        clock_label=NULL;
        label(root,24,35,430,"蓝牙配网",&berry_font_20,TEXT);
        label(root,24,95,430,"手机打开 ESP Provisioning",&berry_font_16,MUTED);
        label(root,24,145,430,"蓝牙名称",&berry_font_16,MUTED);
        label(root,24,175,430,pairing_name,&lv_font_montserrat_24,TEXT);
        label(root,24,245,430,"配对码",&berry_font_16,MUTED);
        label(root,24,275,430,pairing_proof,&lv_font_montserrat_24,TEXT);
        label(root,24,365,430,"三分钟内配网，向下滑动取消",&berry_font_16,MUTED);
        return;
    }
#endif
    cJSON *data=last_data[0]?cJSON_Parse(last_data):NULL;
    if(page==0) draw_codex(data); else if(page==1) draw_dorm(data); else if(page==2) draw_zhiqing(data); else if(page==3) draw_cloud(data); else if(page==4)draw_weather(data);else if(page==5)draw_schedule(data);else draw_pet();
    indicator(); cJSON_Delete(data);
}
#include "berry_pager.inc"
static bool notice_overlay_visible(void) {
    return (task_notice_box&&!lv_obj_has_flag(task_notice_box,LV_OBJ_FLAG_HIDDEN)) ||
           (pet_notice&&!lv_obj_has_flag(pet_notice,LV_OBJ_FLAG_HIDDEN));
}
static void notice_jump(unsigned target) {
    if(target>6||overlay_open())return;
#ifndef BERRY_SIMULATOR
    if(berry_power_screen_off())return;
#endif
    lv_display_trigger_activity(lv_display_get_default());
    task_notice_until=0;pet_notice_until=0;
    lv_obj_add_flag(task_notice_box,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(pet_notice,LV_OBJ_FLAG_HIDDEN);
    pager_go((int)target+1-(int)active_tile);pager_service();
}
static void pet_notice_clicked(lv_event_t *event) {
    (void)event;
    if((int32_t)(pet_notice_until-lv_tick_get())>0)notice_jump(pet_notice_target);
}
static void gesture(lv_event_t *event) {
    (void)event; lv_indev_t *input=lv_indev_active(); if(!input)return; lv_dir_t d=lv_indev_get_gesture_dir(input);
#ifndef BERRY_SIMULATOR
    if(panel_screen){if(d==LV_DIR_TOP && panel_screen==1){panel_screen=0;render();}return;}
    if(d==LV_DIR_BOTTOM && !pairing_screen){panel_screen=1;render();return;}
    if(d==LV_DIR_TOP){berry_pairing_request();return;}
    if(d==LV_DIR_BOTTOM && pairing_screen){berry_pairing_cancel();return;}
    if(pairing_screen)return;
#endif
    if(d==LV_DIR_LEFT)pager_go(1);else if(d==LV_DIR_RIGHT)pager_go(-1);
}
bool berry_ui_test_keyboard(void) {
#ifndef BERRY_SIMULATOR
 if(panel_screen!=2)return false;
 snprintf(chosen_ssid,sizeof(chosen_ssid),"Wi-Fi");chosen_secured=true;panel_screen=3;render();lv_obj_update_layout(root);lv_area_t area;lv_obj_get_coords(panel_keyboard_object,&area);return area.x1>=0 && area.y1>=0 && area.x2<480 && area.y2<480 && area.y2>area.y1;
#else
 return false;
#endif
}
bool berry_ui_test_wifi_click(void) {
#ifndef BERRY_SIMULATOR
 if(panel_screen!=1 || !panel_wifi_label)return false;
 lv_obj_send_event(panel_wifi_label,LV_EVENT_CLICKED,NULL);
 return panel_screen==2;
#else
 return false;
#endif
}
int berry_ui_panel_state(void) {
#ifndef BERRY_SIMULATOR
 return panel_screen;
#else
 return 0;
#endif
}
void berry_ui_button_action(berry_button_action_t action) {
#ifndef BERRY_SIMULATOR
    berry_power_user_activity_locked();
    if(berry_power_screen_off()){(void)berry_power_set_screen_off_locked(false);return;}
    lv_display_trigger_activity(lv_display_get_default());
    if(pairing_screen){berry_pairing_cancel();return;}
    if(panel_screen==4){panel_screen=1;render();return;}
    if(panel_screen==3){if(password_input)lv_textarea_set_text(password_input,"");panel_screen=2;render();return;}
    if(panel_screen==2){panel_screen=1;render();return;}
    if(action==BERRY_BUTTON_PANEL){panel_screen=panel_screen?0:1;render();return;}
    if(panel_screen){panel_screen=0;render();return;}
#endif
    if(action==BERRY_BUTTON_NEXT)pager_go(1);else if(action==BERRY_BUTTON_PREVIOUS)pager_go(-1);
}
static void tick(lv_timer_t *timer) {
    (void)timer; ui_ticks++;ui_page=page;
#ifndef BERRY_SIMULATOR
    char name[32]={0},proof[17]={0};bool show=berry_pairing_view(name,sizeof(name),proof,sizeof(proof));
    if(show!=pairing_screen || strcmp(name,pairing_name)!=0 || strcmp(proof,pairing_proof)!=0){pairing_screen=show;snprintf(pairing_name,sizeof(pairing_name),"%s",name);snprintf(pairing_proof,sizeof(pairing_proof),"%s",proof);render();}
#endif
#ifndef BERRY_SIMULATOR
    if(panel_screen){berry_panel_state_t q;berry_panel_snapshot(&q);if(panel_screen==1 && ++panel_refresh>=10 && !pager_busy()){panel_refresh=0;render();}else if(panel_screen==2 && (q.state!=panel_last.state || q.result!=panel_last.result || q.count!=panel_last.count))render();}
#endif
    bool available=berry_serial_get(received_data,sizeof(received_data));
    if(available && strcmp(received_data,last_data)!=0) {
        snprintf(last_data,sizeof(last_data),"%s",received_data); last_received=xTaskGetTickCount();
        cJSON *json=cJSON_Parse(received_data);if(json){ui_updates++;ui_quota=num(cJSON_GetArrayItem(item(item(json,"codex"),"quota_windows"),0),"remaining_percent");ui_cpu=num(item(item(json,"dorm_pc"),"cpu"),"usage_percent");}const cJSON *cloud=item(json,"aliyun");const char *cloud_update=str(cloud,"updated_at","");const char *rate_keys[]={"latency_ms","upload_bytes_per_second","download_bytes_per_second"};if(cloud_update[0] && strcmp(cloud_update,last_cloud_update)!=0){snprintf(last_cloud_update,sizeof(last_cloud_update),"%s",cloud_update);for(int i=0;i<3;i++){int value=num(cloud,rate_keys[i]);if(value>=0){if(rate_count[i]==16){memmove(rate_history[i],rate_history[i]+1,15*sizeof(int));rate_count[i]=15;}rate_history[i][rate_count[i]++]=value;}else rate_count[i]=0;}}
        const char *stamp=str(json,"generated_at",""); unsigned h,m; clock_valid=sscanf(stamp,"%*4u-%*2u-%*2uT%2u:%2u",&h,&m)==2 && h<24 && m<60;
        if(clock_valid)clock_minutes=((h+8)%24)*60+m;
        pet_monitor_observe(json,time(NULL),lv_tick_get());
        cJSON_Delete(json);
#ifndef BERRY_SIMULATOR
        if(!panel_screen || panel_screen==4)render();
#else
        render();
#endif
    } else if(!available && last_data[0]) { last_data[0]='\0'; clock_valid=false;
#ifndef BERRY_SIMULATOR
        if(!panel_screen || panel_screen==4)render();
#else
        render();
#endif
    }
    if(login_qr_visible){
        cJSON *check=last_data[0]?cJSON_Parse(last_data):NULL;
        bool still_valid=valid_login_qr(check);cJSON_Delete(check);
        if(!still_valid)render();
    }
#ifndef BERRY_SIMULATOR
    if(panel_screen==1){
        cJSON *check=last_data[0]?cJSON_Parse(last_data):NULL;
        bool still_available=valid_login_qr(check);cJSON_Delete(check);
        if(still_available!=login_entry_visible&&!pager_busy())render();
    }
#endif
    if(page==5 && ui_ticks%60==0) {
#ifndef BERRY_SIMULATOR
        if(!panel_screen && !pairing_screen)render();
#else
        render();
#endif
    }
#ifndef BERRY_SIMULATOR
    if(saver_due(saver_enabled, panel_screen != 0, pairing_screen, berry_power_screen_off(), lv_display_get_inactive_time(lv_display_get_default()))) (void)berry_power_set_screen_off_locked(true);
#endif
    unsigned now=lv_tick_get(),elapsed=pet_last_policy_tick?now-pet_last_policy_tick:0;pet_last_policy_tick=now;
    if(elapsed>2500){memset(pet_busy_ms,0,sizeof(pet_busy_ms));memset(pet_hot_ms,0,sizeof(pet_hot_ms));pet_offline_ms=0;elapsed=0;}
    cJSON *policy=last_data[0]?cJSON_Parse(last_data):NULL;pet_policy_update(policy,elapsed);cJSON_Delete(policy);
    if(pet_low_battery_spoken)pet_battery_update(pet_battery_read());
    pager_service();
#ifndef BERRY_SIMULATOR
    unsigned previous_events=task_events;
#endif
    cJSON *notice_data=last_data[0]?cJSON_Parse(last_data):NULL;
    task_notice_update(notice_data,(int64_t)time(NULL));cJSON_Delete(notice_data);
#ifndef BERRY_SIMULATOR
    berry_power_brief_tick_locked();
    if(task_events!=previous_events && task_pending_count && berry_power_screen_off() &&
       !task_notice_blocked() && !pager_busy() && berry_pickup_stationary())
        (void)berry_power_brief_wake_locked();
    task_notice_service(berry_power_screen_off());
#else
    task_notice_service(false);
#endif
    if(clock_label && !pager_busy() && lv_obj_is_valid(clock_label)){char time[8]="--:--";if(clock_valid){unsigned minutes=(clock_minutes+(xTaskGetTickCount()-last_received)/pdMS_TO_TICKS(60000))%(24*60);snprintf(time,sizeof(time),"%02u:%02u",minutes/60,minutes%60);}lv_label_set_text(clock_label,time);}
}
void berry_ui_diagnostics(unsigned *ticks,unsigned *updates,unsigned *page_index,int *quota,int *cpu) {
    *ticks=ui_ticks;*updates=ui_updates;*page_index=ui_page;*quota=ui_quota;*cpu=ui_cpu;
}
static void theme_toggle(lv_event_t *event) {
    (void)event;
    bool next=!light_theme;
#ifndef BERRY_SIMULATOR
    nvs_handle_t handle;
    esp_err_t result=nvs_open("berry_ui",NVS_READWRITE,&handle);
    if(result!=ESP_OK)return;
    result=nvs_set_u8(handle,"light_theme",next);
    if(result==ESP_OK)result=nvs_commit(handle);
    nvs_close(handle);
    if(result!=ESP_OK)return;
#endif
    light_theme=next;
    lv_color_t background=lv_color_hex(BG);
    lv_obj_set_style_bg_color(lv_obj_get_screen(panel_root),background,0);
    lv_obj_set_style_bg_color(panel_root,background,0);
    for(unsigned i=0;i<9;i++)lv_obj_set_style_bg_color(tiles[i],background,0);
    notice_apply_palette(task_notice_box,task_notice_heading,task_notice_title);
    notice_apply_palette(pet_notice,pet_notice_heading,pet_notice_detail);
    pet_paint();
    render();
}
void berry_ui_create(lv_display_t *display) {
#ifndef BERRY_SIMULATOR
    nvs_handle_t handle;
    if(nvs_open("berry_ui",NVS_READONLY,&handle)==ESP_OK){
        uint8_t value;
        if(nvs_get_u8(handle,"screensaver",&value)==ESP_OK)saver_enabled=value==1;
        if(nvs_get_u8(handle,"light_theme",&value)==ESP_OK && value<=1)light_theme=value==1;
        nvs_close(handle);
    }
#endif
    lv_obj_t *screen=lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    pager=lv_tileview_create(screen);lv_obj_remove_style_all(pager);lv_obj_set_size(pager,480,480);
    lv_obj_set_style_anim_duration(pager,280,0);lv_obj_set_scrollbar_mode(pager,LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(pager,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(pager,LV_DIR_HOR);lv_obj_clear_flag(pager,LV_OBJ_FLAG_SCROLLABLE);
    for(unsigned i=0;i<9;i++){
        tiles[i]=lv_tileview_add_tile(pager,i,0,LV_DIR_HOR);lv_obj_remove_style_all(tiles[i]);lv_obj_set_size(tiles[i],480,480);lv_obj_set_pos(tiles[i],i*480,0);
        lv_obj_set_style_bg_color(tiles[i],lv_color_hex(BG),0);lv_obj_set_style_bg_opa(tiles[i],LV_OPA_COVER,0);
        lv_obj_clear_flag(tiles[i],LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(tiles[i],LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    panel_root=lv_obj_create(screen);lv_obj_remove_style_all(panel_root);lv_obj_set_size(panel_root,480,480);lv_obj_clear_flag(panel_root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel_root,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(panel_root,LV_OPA_COVER,0);
    lv_obj_add_flag(panel_root,LV_OBJ_FLAG_HIDDEN|LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(pager,pager_changed,LV_EVENT_VALUE_CHANGED,NULL);
    lv_tileview_set_tile(pager,tiles[1],LV_ANIM_OFF);
    lv_obj_add_event_cb(screen,gesture,LV_EVENT_GESTURE,NULL);
    lv_obj_add_event_cb(screen,pet_tap,LV_EVENT_SHORT_CLICKED,NULL);
    pet_notice=notice_box_create(&pet_notice_heading,&pet_notice_detail);
    lv_obj_add_event_cb(pet_notice,pet_notice_clicked,LV_EVENT_CLICKED,NULL);
    task_notice_create();
    root=tiles[1];render();lv_timer_create(tick,1000,NULL);lv_timer_create(pet_tick,40,NULL);lv_timer_create(pager_service_timer,25,NULL);
}
#ifdef BERRY_SIMULATOR
void berry_ui_sim_page(unsigned selected) { page=selected%7;active_tile=page+1;lv_tileview_set_tile(pager,tiles[active_tile],LV_ANIM_OFF);render(); }
#endif

void berry_ui_motion_diagnostics(unsigned *p,unsigned *scene,unsigned *expression,unsigned *frames,int *position){*p=page;*scene=pet_scene;*expression=pet_expression;*frames=pet_frames;*position=lv_obj_get_scroll_x(pager);}

void berry_ui_notice_diagnostics(unsigned *events,unsigned *shown,unsigned *pending){*events=task_events;*shown=task_shown;*pending=task_pending_count;}
