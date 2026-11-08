#ifndef BERRY_SIMULATOR
static void render(void);
static void panel_back(lv_event_t *e){(void)e;panel_screen=panel_screen==4?1:panel_screen==3?2:panel_screen==2?1:0;render();}
static void panel_open_login(lv_event_t *e){(void)e;cJSON *data=last_data[0]?cJSON_Parse(last_data):NULL;bool available=valid_login_qr(data);cJSON_Delete(data);if(available){panel_screen=4;}render();}
static void panel_open_wifi(lv_event_t *e){(void)e;panel_delete_index=-1;panel_screen=2;berry_panel_scan();render();}
static void panel_select_wifi(lv_event_t *e){unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);berry_panel_state_t q;berry_panel_snapshot(&q);if(i>=q.count)return;snprintf(chosen_ssid,sizeof(chosen_ssid),"%s",q.aps[i].ssid);chosen_secured=q.aps[i].secured;if(q.aps[i].saved && q.result!=-2 && berry_panel_connect_saved(chosen_ssid,chosen_secured)){panel_screen=2;render();return;}panel_screen=3;render();}
static void panel_swipe_wifi(lv_event_t *e){
 lv_indev_t *input=lv_indev_active();if(!input)return;unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);
 berry_panel_state_t q;berry_panel_snapshot(&q);
 if(lv_indev_get_gesture_dir(input)==LV_DIR_LEFT && i<q.count && q.aps[i].saved && q.state==0){panel_delete_index=(int)i;render();}
}
static void panel_forget_wifi(lv_event_t *e){
 unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);berry_panel_state_t q;berry_panel_snapshot(&q);
 if(i<q.count && q.aps[i].saved)berry_panel_forget(q.aps[i].ssid);
 panel_delete_index=-1;render();
}
static void panel_password_insert(lv_event_t *e){const char *s=lv_event_get_param(e);if(s && (strchr(s,'\n') || strchr(s,'\r')))lv_textarea_set_insert_replace(lv_event_get_target(e),"");}
static void panel_keyboard(lv_event_t *e){lv_event_code_t code=lv_event_get_code(e);if(code==LV_EVENT_CANCEL){panel_screen=2;render();return;}if(code!=LV_EVENT_READY)return;const char *password=lv_textarea_get_text(password_input);if(chosen_secured && strlen(password)<8){lv_textarea_set_placeholder_text(password_input,"至少输入8位密码");return;}if(berry_panel_connect(chosen_ssid,password)){lv_textarea_set_text(password_input,"");panel_screen=2;render();}}
static lv_obj_t *panel_action_button(int column,const char *text,uint32_t color,bool enabled,lv_event_cb_t callback){
 enum { width=140, gap=12 };
 lv_obj_t *button=lv_button_create(root);lv_obj_remove_style_all(button);
 lv_obj_set_pos(button,18+column*(width+gap),411);lv_obj_set_size(button,width,48);
 lv_obj_set_style_bg_color(button,lv_color_hex(CARD),0);lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
 lv_obj_set_style_bg_color(button,lv_color_hex(light_theme?SELECTED:BORDER),LV_STATE_PRESSED);
 lv_obj_set_style_border_width(button,1,0);lv_obj_set_style_border_color(button,lv_color_hex(BORDER),0);
 lv_obj_set_style_radius(button,12,0);lv_obj_remove_flag(button,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_flag(button,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
 if(enabled)lv_obj_add_event_cb(button,callback,LV_EVENT_CLICKED,NULL);
 else {lv_obj_remove_flag(button,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_state(button,LV_STATE_DISABLED);lv_obj_set_style_opa(button,LV_OPA_50,0);}
 lv_obj_t *caption=label(button,0,0,width-16,text,&berry_font_16,color);
 lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(caption);return button;
}
static void panel_battery_bar(lv_obj_t *parent,int percent,bool charging){
 bool valid=percent>=0 && percent<=100;
 uint32_t color=charging?GREEN:percent<=20?(light_theme?RED:0xf28b82):percent<=40?AMBER:BLUE;
 for(int i=0;i<20;i++){
  lv_obj_t *segment=lv_obj_create(parent);lv_obj_remove_style_all(segment);
  lv_obj_set_pos(segment,15+i*388/19,65);lv_obj_set_size(segment,18,12);
  lv_obj_set_style_bg_color(segment,lv_color_hex(BORDER),0);lv_obj_set_style_bg_opa(segment,LV_OPA_COVER,0);lv_obj_set_style_radius(segment,2,0);
  lv_obj_remove_flag(segment,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);lv_obj_add_flag(segment,LV_OBJ_FLAG_GESTURE_BUBBLE);
  int amount=valid?percent-i*5:0;if(amount>5)amount=5;
  if(amount>0){lv_obj_t *fill=lv_obj_create(segment);lv_obj_remove_style_all(fill);
   lv_obj_set_size(fill,(amount*18+2)/5,12);lv_obj_set_style_bg_color(fill,lv_color_hex(color),0);lv_obj_set_style_bg_opa(fill,LV_OPA_COVER,0);lv_obj_set_style_radius(fill,2,0);
   lv_obj_remove_flag(fill,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);lv_obj_add_flag(fill,LV_OBJ_FLAG_GESTURE_BUBBLE);
  }
 }
}
static void panel_brightness_event(lv_event_t *event) {
 lv_obj_t *slider=lv_event_get_target(event);
 lv_obj_t *caption=lv_event_get_user_data(event);
 if(lv_event_get_code(event)==LV_EVENT_VALUE_CHANGED){
  int level=lv_slider_get_value(slider);
  if(berry_power_set_brightness_locked(level)==ESP_OK)lv_label_set_text_fmt(caption,"%d%%",level);
 }else{
  int level=berry_power_brightness_locked();
  lv_slider_set_value(slider,level,LV_ANIM_OFF);
  lv_label_set_text_fmt(caption,"%d%%",level);
  berry_power_save_brightness_locked();
 }
}
static void panel_draw_brightness(void) {
 lv_obj_t *row=box(18,239,444,78);
 label(row,15,10,300,"屏幕亮度",&berry_font_16,MUTED);
 lv_obj_t *caption=label(row,320,7,99,"",&berry_font_20,TEXT);right(caption);
 int level=berry_power_brightness_locked();
 lv_label_set_text_fmt(caption,"%d%%",level);
 lv_obj_t *slider=lv_slider_create(row);
 lv_obj_remove_style_all(slider);lv_obj_set_pos(slider,20,48);lv_obj_set_size(slider,404,8);
 lv_obj_set_ext_click_area(slider,14);
 lv_slider_set_range(slider,5,100);lv_slider_set_value(slider,level,LV_ANIM_OFF);
 lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_MAIN);
 lv_obj_set_style_bg_color(slider,lv_color_hex(BORDER),LV_PART_MAIN);
 lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_MAIN);
 lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_INDICATOR);
 lv_obj_set_style_bg_color(slider,lv_color_hex(BLUE),LV_PART_INDICATOR);
 lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
 lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_KNOB);
 lv_obj_set_style_bg_color(slider,lv_color_hex(BLUE),LV_PART_KNOB);
 lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
 lv_obj_set_style_pad_all(slider,5,LV_PART_KNOB);
 lv_obj_add_event_cb(slider,panel_brightness_event,LV_EVENT_VALUE_CHANGED,caption);
 lv_obj_add_event_cb(slider,panel_brightness_event,LV_EVENT_RELEASED,caption);
 lv_obj_add_event_cb(slider,panel_brightness_event,LV_EVENT_PRESS_LOST,caption);
}
static void panel_draw(void){
 berry_panel_state_t q;berry_panel_snapshot(&q);panel_last=q;clock_label=NULL;
 label(root,20,15,350,panel_screen==1?"状态中心":panel_screen==2?"选择 Wi-Fi":"连接 Wi-Fi",&berry_font_20,TEXT);
 lv_obj_t *back=lv_button_create(root);lv_obj_set_pos(back,370,8);lv_obj_set_size(back,90,40);lv_obj_set_style_bg_color(back,lv_color_hex(CARD),0);
 if(light_theme){
  lv_obj_set_style_shadow_width(back,0,0);lv_obj_set_style_border_width(back,1,0);lv_obj_set_style_border_color(back,lv_color_hex(BORDER),0);
  lv_obj_set_style_bg_color(back,lv_color_hex(SELECTED),LV_STATE_PRESSED);
 }
 lv_obj_add_event_cb(back,panel_back,LV_EVENT_CLICKED,NULL);label(back,0,0,75,"返回",&berry_font_16,TEXT);
 if(panel_screen==1){
   wifi_ap_record_t ap={0};bool connected=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;char wifi_name[33]={0};if(connected)memcpy(wifi_name,ap.ssid,32);
   lv_obj_t *row=box(18,60,444,74);lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(row,panel_open_wifi,LV_EVENT_CLICKED,NULL);
   panel_wifi_label=label(row,15,10,410,"Wi-Fi  轻触选择网络",&berry_font_16,MUTED);label(row,15,36,410,connected?wifi_name:"未连接",&berry_font_20,TEXT);
   row=box(18,143,444,87);label(row,15,10,410,"电池电量与剩余续航",&berry_font_16,MUTED);char b[100];
   if(q.battery<0 || q.battery>100)snprintf(b,sizeof(b),"电量未知  续航未知");else if(q.charging || q.external)snprintf(b,sizeof(b),"%d%%  %s",q.battery,q.charging?"充电中":"外部供电");else if(q.remaining_minutes<0)snprintf(b,sizeof(b),"%d%%  续航估算中",q.battery);else snprintf(b,sizeof(b),"%d%%  约 %d 小时 %d 分",q.battery,q.remaining_minutes/60,q.remaining_minutes%60);label(row,15,34,410,b,&berry_font_20,TEXT);
   panel_battery_bar(row,q.battery,q.charging);
   panel_draw_brightness();
   row=box(18,326,444,74);label(row,15,10,410,"最近一次收到数据",&berry_font_16,MUTED);unsigned age=berry_serial_last_age_seconds();char last[80];if(age==UINT32_MAX)snprintf(last,sizeof(last),"尚未收到数据");else if(age<60)snprintf(last,sizeof(last),"%u 秒前",age);else if(age<3600)snprintf(last,sizeof(last),"%u 分钟前",age/60);else snprintf(last,sizeof(last),"%u 小时前",age/3600);label(row,15,36,410,last,&berry_font_20,TEXT);
   cJSON *data=last_data[0]?cJSON_Parse(last_data):NULL;login_entry_visible=valid_login_qr(data);cJSON_Delete(data);
   panel_action_button(0,"QQ 扫码",login_entry_visible?BLUE:MUTED,login_entry_visible,panel_open_login);
   panel_action_button(1,light_theme?"主题 浅色":"主题 深色",BLUE,true,theme_toggle);
   panel_action_button(2,saver_enabled?"屏保  开":"屏保  关",saver_enabled?BLUE:TEXT,true,saver_toggle);return;
 }
 if(panel_screen==2){
   const char *status=q.state==1?"正在搜索 Wi-Fi":q.state==2?"正在连接，请稍候":q.state==3?"正在删除，请稍候":q.result==-2?"连接失败，请检查密码":q.result==-1?"搜索失败，请返回重试":q.result==-3?"删除失败，请重试":q.result==2?"密码已删除":q.result==1?"连接成功，已保存":q.count?"选择附近的 Wi-Fi":"未发现网络，请返回重试";
   label(root,20,55,440,status,&berry_font_16,MUTED);
   lv_obj_t *list=lv_obj_create(root);lv_obj_set_pos(list,12,88);lv_obj_set_size(list,456,370);lv_obj_set_style_bg_opa(list,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(list,0,0);lv_obj_set_style_pad_all(list,0,0);lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(list,6,0);
   for(unsigned i=0;i<q.count;i++){
     lv_obj_t *b=lv_button_create(list);lv_obj_set_size(b,430,54);lv_obj_set_style_bg_color(b,lv_color_hex(CARD),0);
     if((int)i==panel_delete_index && q.aps[i].saved){lv_obj_add_event_cb(b,panel_forget_wifi,LV_EVENT_CLICKED,(void *)(uintptr_t)i);label(b,0,0,285,q.aps[i].ssid,&berry_font_16,TEXT);label(b,300,0,100,"删除密码",&berry_font_16,BLUE);}
     else {lv_obj_add_event_cb(b,panel_select_wifi,LV_EVENT_CLICKED,(void *)(uintptr_t)i);label(b,0,0,345,q.aps[i].ssid,&berry_font_16,TEXT);label(b,345,0,65,q.aps[i].saved?"已存":q.aps[i].secured?"锁":"开",&berry_font_16,MUTED);}
     lv_obj_remove_flag(b,LV_OBJ_FLAG_GESTURE_BUBBLE);lv_obj_add_event_cb(b,panel_swipe_wifi,LV_EVENT_GESTURE,(void *)(uintptr_t)i);
   }
   return;
 }
 label(root,20,58,440,chosen_ssid,&berry_font_20,TEXT);label(root,20,95,440,chosen_secured?"请输入 Wi-Fi 密码":"开放网络，直接点确定",&berry_font_16,MUTED);
 password_input=lv_textarea_create(root);lv_obj_set_pos(password_input,20,128);lv_obj_set_size(password_input,440,78);lv_textarea_set_one_line(password_input,false);lv_textarea_set_password_mode(password_input,false);lv_obj_add_event_cb(password_input,panel_password_insert,LV_EVENT_INSERT,NULL);lv_textarea_set_max_length(password_input,63);
 lv_obj_t *kb=lv_keyboard_create(root);panel_keyboard_object=kb;lv_obj_set_size(kb,440,230);lv_obj_align(kb,LV_ALIGN_TOP_MID,0,220);lv_keyboard_set_textarea(kb,password_input);lv_obj_add_event_cb(kb,panel_keyboard,LV_EVENT_ALL,NULL);
}
#endif
