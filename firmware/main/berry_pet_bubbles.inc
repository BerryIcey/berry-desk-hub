extern const lv_font_t berry_font_26;
/* Local, bounded UI copy; never generated from QQ text or stored as memory. */
static lv_obj_t *pet_bubbles[9];
static char pet_bubble_text[96];
static unsigned pet_bubble_until,pet_bubble_last_tap;
static bool pet_bubble_tapped;
static unsigned pet_utf8_count(const char *s){unsigned count=0;for(;*s;s++)if(((unsigned char)*s&0xc0)!=0x80)count++;return count;}
static void pet_bubble_apply(void){
    unsigned i=active_tile;
    if(!pet_bubbles[i]||!lv_obj_is_valid(pet_bubbles[i]))return;
    bool visible=pet_bubble_text[0]&&(int32_t)(pet_bubble_until-lv_tick_get())>0;
    if(visible){
        if(strcmp(lv_label_get_text(pet_labels[i]),pet_bubble_text)!=0)lv_label_set_text(pet_labels[i],pet_bubble_text);
        if(lv_obj_has_flag(pet_bubbles[i],LV_OBJ_FLAG_HIDDEN))lv_obj_remove_flag(pet_bubbles[i],LV_OBJ_FLAG_HIDDEN);
    }else if(!lv_obj_has_flag(pet_bubbles[i],LV_OBJ_FLAG_HIDDEN))lv_obj_add_flag(pet_bubbles[i],LV_OBJ_FLAG_HIDDEN);
}
static bool pet_say(const char *text,unsigned duration){
    if(page!=6||overlay_open()||!text||strlen(text)>=sizeof(pet_bubble_text)||pet_utf8_count(text)>24)return false;
    lv_point_t size;lv_text_get_size(&size,text,&berry_font_26,0,8,300,LV_TEXT_FLAG_NONE);
    if(size.y>60)return false;
    snprintf(pet_bubble_text,sizeof(pet_bubble_text),"%s",text);pet_bubble_until=lv_tick_get()+duration;pet_bubble_apply();return true;
}
#include "berry_pet_copy.inc"
static bool pet_say_status(void){
    static const char *const names[]={"电脑 CPU","电脑 GPU","服务器 CPU"};
    char text[96];
    if(pet_scene==PET_HOT){
        if(pet_source<0||pet_source>1||pet_value<90||pet_value>150)return false;
        snprintf(text,sizeof(text),"%s 温度偏高\n当前 %d℃",names[pet_source],pet_value);
    }else if(pet_scene==PET_BUSY){
        if(pet_source<0||pet_source>2||pet_value<95||pet_value>100)return false;
        snprintf(text,sizeof(text),"%s 持续高负载\n当前占用 %d%%",names[pet_source],pet_value);
    }else if(pet_scene==PET_OFFLINE)snprintf(text,sizeof(text),"芷清连接报告离线\n可查看芷清状态。");
    else if(pet_scene==PET_UNKNOWN)snprintf(text,sizeof(text),"部分数据暂时缺失\n暂不判断设备状态");
    else if((int32_t)(pet_recovery_until-lv_tick_get())>0)return pet_say("这次异常已恢复\n监测数据回到平稳",4500);
    else return false;
    return pet_say(text,6000);
}
static void pet_interact(bool tapped) {
    uint32_t tick=lv_tick_get();
    pet_action(true);
    pet_blink_at=tick;
    if(!tapped&&pet_scene!=PET_UNKNOWN&&pet_say_status())return;
    if(!tapped||(pet_rand()&1u)) {
        cJSON *data=last_data[0]?cJSON_Parse(last_data):NULL;
        bool spoken=pet_report_useful(data,time(NULL),tick,tapped,pet_battery_read());
        cJSON_Delete(data);
        if(spoken||(tapped&&pet_scene!=PET_UNKNOWN&&pet_say_status()))return;
    }
    pet_bubble_text[0]='\0';
    pet_bubble_apply();
}
static void pet_on_enter(void){
    pet_interact(false);
}
static void pet_on_tap(void){
    unsigned now=lv_tick_get();if(pet_bubble_tapped&&now-pet_bubble_last_tap<1000)return;
    pet_bubble_tapped=true;pet_bubble_last_tap=now;
    pet_interact(true);
}



#include "smooth_frame.h"
static lv_image_dsc_t comic_image;
static void comic_raster(void){
 comic_image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w=382,.h=130,.stride=382*4},.data_size=sizeof(comic_pixels),.data=(const uint8_t *)comic_pixels};
}

static void pet_bubble_create(void){
 lv_obj_t *bubble=lv_obj_create(root);lv_obj_remove_style_all(bubble);lv_obj_set_pos(bubble,50,8);lv_obj_set_size(bubble,382,130);
 lv_obj_clear_flag(bubble,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(bubble,LV_OBJ_FLAG_HIDDEN);
 comic_raster();lv_obj_t *frame=lv_image_create(bubble);lv_image_set_src(frame,&comic_image);lv_obj_clear_flag(frame,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *text=label(bubble,40,25,300,"",&berry_font_26,0x16191d);lv_label_set_long_mode(text,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(text,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_line_space(text,8,0);lv_obj_align(text,LV_ALIGN_CENTER,0,-14);
 pet_bubbles[active_tile]=bubble;pet_labels[active_tile]=text;pet_bubble_apply();
}
