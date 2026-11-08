typedef struct {
    char key[33];
    int64_t observed;
    bool running;
    unsigned announced;
    unsigned queued, shown, pet_spoken;
} berry_task_seen_t;
static berry_task_seen_t task_seen[12];
static unsigned task_events,task_shown,task_pending_count;
static uint32_t task_notice_until,task_pending_until;
static unsigned task_pending_kind;
static char task_pending_title[241];
static char task_pending_key[33];
static lv_obj_t *task_notice_box,*task_notice_heading,*task_notice_title;

static int64_t task_notice_time(const char *stamp) {
    int y,m,d,h,mi,s;
    size_t n=strlen(stamp);
    if(n<20 || !(stamp[n-1]=='Z'||(n>=6&&strcmp(stamp+n-6,"+00:00")==0)))return 0;
    if(sscanf(stamp,"%d-%d-%dT%d:%d:%d",&y,&m,&d,&h,&mi,&s)!=6||y<2020||y>2100||m<1||m>12||h<0||h>23||mi<0||mi>59||s<0||s>59)return 0;
    int days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=y%4==0&&(y%100!=0||y%400==0);if(leap)days[1]=29;
    if(d<1||d>days[m-1])return 0;
    int64_t total=0;
    for(int year=1970;year<y;year++)total+=365+(year%4==0&&(year%100!=0||year%400==0));
    for(int month=1;month<m;month++)total+=days[month-1];
    return ((total+d-1)*24+h)*3600+mi*60+s;
}
static bool task_key_valid(const char *key) {
    if(strlen(key)!=32)return false;
    for(unsigned i=0;i<32;i++)if(!((key[i]>='0'&&key[i]<='9')||(key[i]>='a'&&key[i]<='f')))return false;
    return true;
}
static int64_t task_fresh_observation(const cJSON *data,int64_t now) {
    const cJSON *codex=item(data,"codex");
    int64_t observed=task_notice_time(str(codex,"tasks_updated_at",""));
    if(strcmp(str(data,"status",""),"fresh")||strcmp(str(codex,"tasks_status",""),"available")||
       now<1700000000||observed<=0||observed>now+5||now-observed>120)return 0;
    return observed;
}
static unsigned task_pet_waiting(const cJSON *data,int64_t now,unsigned kind,bool mark) {
    int64_t observed=task_fresh_observation(data,now);
    if(!observed)return 0;
    const cJSON *tasks=item(item(data,"codex"),"recent_tasks");
    const char *state=kind==2?"waiting_approval":"blocked";
    unsigned found=0;
    for(int i=0;i<cJSON_GetArraySize(tasks)&&i<4;i++) {
        const cJSON *task=cJSON_GetArrayItem(tasks,i);
        const char *key=str(task,"notice_key","");
        if(!task_key_valid(key)||strcmp(str(task,"notice_state",""),state))continue;
        for(unsigned j=0;j<12;j++) {
            berry_task_seen_t *seen=&task_seen[j];
            if(seen->observed!=observed||strcmp(seen->key,key)||
               ((seen->queued|seen->shown|seen->pet_spoken)&kind)||(found&(1u<<j)))continue;
            found|=1u<<j;
            if(mark)seen->pet_spoken|=kind;
            break;
        }
    }
    unsigned count=0;
    for(;found;found&=found-1)count++;
    return count;
}
static void task_notice_update(const cJSON *data,int64_t now) {
    const cJSON *codex=item(data,"codex");
    int64_t observed=task_fresh_observation(data,now);
    if(!observed){for(unsigned i=0;i<12;i++)task_seen[i].running=false;return;}
    const cJSON *tasks=item(codex,"recent_tasks");
    for(int i=0;i<cJSON_GetArraySize(tasks)&&i<4;i++) {
        const cJSON *task=cJSON_GetArrayItem(tasks,i);
        const char *key=str(task,"notice_key",""),*state=str(task,"notice_state","unknown");
        if(!task_key_valid(key))continue;
        berry_task_seen_t *seen=NULL,*oldest=&task_seen[0];
        for(unsigned j=0;j<12;j++) {
            if(strcmp(task_seen[j].key,key)==0){seen=&task_seen[j];break;}
            if(task_seen[j].observed<oldest->observed)oldest=&task_seen[j];
        }
        if(!seen){seen=oldest;memset(seen,0,sizeof(*seen));snprintf(seen->key,sizeof(seen->key),"%s",key);}
        if(observed<=seen->observed)continue;
        if(seen->observed&&observed-seen->observed>120)seen->running=false;
        unsigned kind=strcmp(state,"completed")==0?1:strcmp(state,"waiting_approval")==0?2:strcmp(state,"blocked")==0?4:0;
        if(kind&&seen->running&&!(seen->announced&kind)) {
            seen->announced|=kind;seen->queued|=kind;task_events++;task_pending_count++;
            task_pending_kind=kind;task_pending_until=lv_tick_get()+120000;
            snprintf(task_pending_title,sizeof(task_pending_title),"%s",str(task,"title","Codex"));
            snprintf(task_pending_key,sizeof(task_pending_key),"%s",key);
        }
        if(!kind&&task_pending_kind!=1&&strcmp(task_pending_key,key)==0)task_pending_count=0;
        if(strcmp(state,"running")==0)seen->running=true;
        else if(!kind)seen->running=false;
        seen->observed=observed;
    }
}
static void task_notice_clicked(lv_event_t *event) {
    (void)event;
    if(!task_notice_until||(int32_t)(task_notice_until-lv_tick_get())<=0)return;
#ifndef BERRY_SIMULATOR
    if(panel_screen==1){panel_screen=0;render();}
#endif
    notice_jump(0);
}

static int notice_line_width(const char *text,const lv_font_t *font) {
    lv_point_t size;lv_text_get_size(&size,text,font,0,0,32767,LV_TEXT_FLAG_NONE);return size.x;
}
static void notice_fit_line(char *out,size_t size,const char *text,const lv_font_t *font) {
    size_t length=strlen(text),keep=length<size-3?length:size-4;
    while(keep&&keep<length&&((unsigned char)text[keep]&0xc0)==0x80)keep--;
    memcpy(out,text,keep);out[keep]='\0';
    for(char *p=out;*p;p++)if((unsigned char)*p<32)*p=' ';
    if(!out[0])snprintf(out,size,"Codex");
    uint32_t len=(uint32_t)strlen(out);
    if(notice_line_width(out,font)>386) {
        do {
            do{len--;}while(len&&((unsigned char)out[len]&0xc0)==0x80);
            memcpy(out+len,"...",4);
        }while(len&&notice_line_width(out,font)>386);
    }
}
static int notice_ink(const lv_font_t *font,const char *text,int *top) {
    int low=10000,high=-10000;
    for(const unsigned char *p=(const unsigned char*)text;*p;) {
        uint32_t code=*p++;unsigned more=code>=0xf0?3:code>=0xe0?2:code>=0xc0?1:0;
        if(more)code&=0x7fU>>(more+1);
        while(more--&&(*p&0xc0)==0x80)code=(code<<6)|(*p++&0x3f);
        lv_font_glyph_dsc_t glyph={0};
        (void)lv_font_get_glyph_dsc(font,&glyph,code,0);
        if(!glyph.box_h)continue;
        int y=font->line_height-font->base_line-glyph.box_h-glyph.ofs_y;
        int first=0,last=glyph.box_h;
        if(glyph.resolved_font&&glyph.resolved_font->get_glyph_bitmap==lv_font_get_bitmap_fmt_txt) {
            const lv_font_fmt_txt_dsc_t *format=glyph.resolved_font->dsc;
            unsigned bpp=format->bpp;
            if(format->bitmap_format==LV_FONT_FMT_TXT_PLAIN&&(bpp==1||bpp==2||bpp==4||bpp==8)) {
                glyph.req_raw_bitmap=1;
                const uint8_t *pixels=lv_font_get_bitmap_fmt_txt(&glyph,NULL);
                if(pixels) {
                    first=glyph.box_h;last=0;
                    unsigned row_bits=glyph.stride?glyph.stride*8:glyph.box_w*bpp;
                    for(unsigned row=0;row<glyph.box_h;row++)for(unsigned column=0;column<glyph.box_w;column++) {
                        unsigned bit=row*row_bits+column*bpp;
                        if((pixels[bit/8]>>(8-bpp-(bit%8)))&((1U<<bpp)-1)) {
                            if((int)row<first)first=(int)row;
                            last=(int)row+1;break;
                        }
                    }
                }
            }
        }
        lv_font_glyph_release_draw_data(&glyph);
        if(last<=first)continue;
        if(y+first<low)low=y+first;
        if(y+last>high)high=y+last;
    }
    if(low==10000){*top=0;return font->line_height;}
    *top=low;return high-low;
}
static void notice_apply_palette(lv_obj_t *box,lv_obj_t *heading,lv_obj_t *detail) {
    lv_obj_set_style_bg_color(box,lv_color_hex(CARD),0);
    lv_obj_set_style_border_color(box,lv_color_hex(BLUE),0);
    lv_obj_set_style_text_color(heading,lv_color_hex(TEXT),0);
    lv_obj_set_style_text_color(detail,lv_color_hex(MUTED),0);
}
static lv_obj_t *notice_box_create(lv_obj_t **heading,lv_obj_t **detail) {
    lv_obj_t *box=lv_obj_create(lv_layer_top());lv_obj_remove_style_all(box);
    lv_obj_set_pos(box,30,55);lv_obj_set_size(box,420,76);
    lv_obj_set_style_bg_opa(box,LV_OPA_COVER,0);
    lv_obj_set_style_radius(box,12,0);lv_obj_set_style_border_width(box,2,0);
    lv_obj_clear_flag(box,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(box,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
    *heading=label(box,14,0,390,"",&berry_font_20,TEXT);
    *detail=label(box,14,0,390,"",&berry_font_16,MUTED);
    notice_apply_palette(box,*heading,*detail);
    lv_obj_clear_flag(*heading,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(*detail,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    return box;
}
static void notice_layout(lv_obj_t *box,lv_obj_t *heading,lv_obj_t *detail,const char *first,const char *second) {
    char line1[192],line2[192];
    notice_fit_line(line1,sizeof(line1),first,&berry_font_20);notice_fit_line(line2,sizeof(line2),second,&berry_font_16);
    int top1,top2;
    int h1=notice_ink(&berry_font_20,line1,&top1),h2=notice_ink(&berry_font_16,line2,&top2);
    const int gap=11,border=2;
    lv_label_set_text(heading,line1);lv_label_set_text(detail,line2);
    lv_obj_set_height(box,2*border+3*gap+h1+h2);
    lv_obj_update_layout(box);
    lv_area_t area,content;lv_obj_get_coords(box,&area);lv_obj_get_content_coords(box,&content);
    int origin=content.y1-area.y1;
    lv_obj_set_y(heading,border+gap-top1-origin);
    lv_obj_set_y(detail,border+2*gap+h1-top2-origin);
}
static void task_notice_create(void) {
    task_notice_box=notice_box_create(&task_notice_heading,&task_notice_title);
    lv_obj_add_event_cb(task_notice_box,task_notice_clicked,LV_EVENT_CLICKED,NULL);
}
static bool task_notice_blocked(void) {
#ifndef BERRY_SIMULATOR
    if(panel_screen>1||pairing_screen)return true;
#else
    if(overlay_open())return true;
#endif
    return (int32_t)(pet_notice_until-lv_tick_get())>0;
}
static void task_notice_service(bool screen_off) {
    if(!task_notice_box)return;
    uint32_t now=lv_tick_get();
    if(task_pending_count&&(int32_t)(task_pending_until-now)<=0)task_pending_count=0;
    if(!task_pending_count)for(unsigned i=0;i<12;i++)task_seen[i].queued=0;
    if(screen_off||task_notice_blocked()) {
        lv_obj_add_flag(task_notice_box,LV_OBJ_FLAG_HIDDEN);task_notice_until=0;return;
    }
    if(pager_busy())return;
    if(task_notice_until&&(int32_t)(task_notice_until-now)<=0) {
        lv_obj_add_flag(task_notice_box,LV_OBJ_FLAG_HIDDEN);task_notice_until=0;
    }
    if(task_pending_count&&!task_notice_until) {
        const char *heading=task_pending_kind==2?"任务等待批准":task_pending_kind==4?"任务等待输入":"任务已完成";
        char combined[64];
        if(task_pending_count>1){snprintf(combined,sizeof(combined),"%u 条任务更新",task_pending_count);heading=combined;}
        notice_layout(task_notice_box,task_notice_heading,task_notice_title,heading,task_pending_title);
        lv_obj_remove_flag(task_notice_box,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(task_notice_box);
        for(unsigned i=0;i<12;i++) {
            task_seen[i].shown|=task_seen[i].queued;
            task_seen[i].queued=0;
        }
        task_pending_count=0;task_notice_until=now+6000;task_shown++;
    }
}
