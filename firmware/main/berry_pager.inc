static bool overlay_open(void){
#ifndef BERRY_SIMULATOR
    return panel_screen||pairing_screen;
#else
    return false;
#endif
}
static bool pager_busy(void){if(pager&&lv_obj_is_scrolling(pager))return true;for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input))if(lv_indev_get_type(input)==LV_INDEV_TYPE_POINTER&&lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED)return true;return false;}
static lv_obj_t *tile_clock[9];
static unsigned tile_revision[9],data_revision=1;
static bool pager_pending,pager_recenter;
static void draw_tile(unsigned i){
    unsigned selected=page;lv_obj_t *saved=root;
    page=(i+6)%7;root=tiles[i];
    pet_images[i]=NULL;pet_labels[i]=NULL;pet_bubbles[i]=NULL;clock_label=NULL;
    draw_page();tile_clock[i]=clock_label;tile_revision[i]=data_revision;
    page=selected;root=saved;
}
static void pager_refresh(void){
    if(pager_busy()){pager_pending=true;return;}
    if(overlay_open()){
        lv_obj_add_flag(pager,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(panel_root,LV_OBJ_FLAG_HIDDEN);root=panel_root;clock_label=NULL;draw_page();pager_pending=false;return;
    }
    if(!lv_obj_has_flag(panel_root,LV_OBJ_FLAG_HIDDEN)){lv_obj_clean(panel_root);lv_obj_add_flag(panel_root,LV_OBJ_FLAG_HIDDEN);}
    lv_obj_remove_flag(pager,LV_OBJ_FLAG_HIDDEN);
    if(pager_recenter){pager_recenter=false;active_tile=page+1;lv_tileview_set_tile(pager,tiles[active_tile],LV_ANIM_OFF);}
    for(unsigned i=active_tile-1;i<=active_tile+1;i++)if(i<9&&tile_revision[i]!=data_revision){unsigned save=active_tile;active_tile=i;draw_tile(i);active_tile=save;}
    root=tiles[active_tile];clock_label=tile_clock[active_tile];pager_pending=false;
}
static void render(void){data_revision++;pager_refresh();}
static void pager_changed(lv_event_t *e){
    if(lv_event_get_target(e)!=pager)return;
    lv_obj_t *selected=lv_tileview_get_tile_active(pager);unsigned i=0;for(;i<9&&tiles[i]!=selected;i++);
    if(i==9)return;
    unsigned old=page;active_tile=i;page=(i+6)%7;ui_page=page;
    root=tiles[i];clock_label=tile_clock[i];
    if(page==6&&old!=6)pet_on_enter();
    if(i==0||i==8)pager_recenter=true;
    pager_pending=true;
}
static void pager_go(int direction){
    if((pager&&lv_obj_is_scrolling(pager))||overlay_open())return;
    int target=(int)active_tile+direction;if(target<0||target>8)return;
    unsigned old=page;active_tile=(unsigned)target;page=(active_tile+6)%7;ui_page=page;
    lv_tileview_set_tile(pager,tiles[target],LV_ANIM_OFF);
    root=tiles[target];clock_label=tile_clock[target];
    if(page==6&&old!=6)pet_on_enter();
    if(target==0||target==8)pager_recenter=true;
    pager_pending=true;
}
static void pager_service(void){
    if(pager_pending||pager_recenter)pager_refresh();
    if(!overlay_open() && !pager_busy()){
        const char *value=pet_notice_text;
        bool visible=(int32_t)(pet_notice_until-lv_tick_get())>0;
#ifndef BERRY_SIMULATOR
        visible=visible&&!berry_power_screen_off();
#endif
        if(visible){
            static char rendered[96];
            if(strcmp(rendered,value)!=0){
                char first[96];snprintf(first,sizeof(first),"%s",value);char *second=strchr(first,'\n');
                if(second)*second++='\0';
                notice_layout(pet_notice,pet_notice_heading,pet_notice_detail,first,second?second:"查看详情");
                snprintf(rendered,sizeof(rendered),"%s",value);
            }
            lv_obj_remove_flag(pet_notice,LV_OBJ_FLAG_HIDDEN);
        }
        else lv_obj_add_flag(pet_notice,LV_OBJ_FLAG_HIDDEN);
    }else if(overlay_open())lv_obj_add_flag(pet_notice,LV_OBJ_FLAG_HIDDEN);
}
static void pager_service_timer(lv_timer_t *t){(void)t;pager_service();}
