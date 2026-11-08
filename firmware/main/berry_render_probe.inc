#include "esp_timer.h"
static uint64_t probe_begin,probe_frame_begin,probe_wait_begin,probe_render_us,probe_wait_us,probe_pixels,probe_frame_pixels,probe_full_us;
static unsigned probe_full_frames;
static unsigned probe_frames,probe_chunks,probe_frame_chunks,probe_max_us;
static bool probe_registered;
static void render_probe_event(lv_event_t *e){
    uint64_t now=esp_timer_get_time();
    switch(lv_event_get_code(e)){
    case LV_EVENT_REFR_START:probe_frame_begin=now;probe_frame_chunks=0;probe_frame_pixels=0;break;
    case LV_EVENT_FLUSH_START:{lv_area_t *a=lv_event_get_param(e);probe_chunks++;probe_frame_chunks++;uint64_t count=(uint64_t)(a->x2-a->x1+1)*(a->y2-a->y1+1);probe_pixels+=count;probe_frame_pixels+=count;break;}
    case LV_EVENT_FLUSH_WAIT_START:probe_wait_begin=now;break;
    case LV_EVENT_FLUSH_WAIT_FINISH:if(probe_wait_begin)probe_wait_us+=now-probe_wait_begin;probe_wait_begin=0;break;
    case LV_EVENT_REFR_READY:if(probe_frame_chunks){unsigned elapsed=now-probe_frame_begin;probe_frames++;probe_render_us+=elapsed;if(elapsed>probe_max_us)probe_max_us=elapsed;if(probe_frame_pixels>=160000){probe_full_frames++;probe_full_us+=elapsed;}}break;
    default:break;
    }
}
static void render_probe_command(bool reset){
    if(esp_lv_adapter_lock(1000)!=ESP_OK){reply("RENDERDIAG busy\n");return;}
    if(!probe_registered){lv_display_add_event_cb(lv_display_get_default(),render_probe_event,LV_EVENT_ALL,NULL);probe_registered=true;}
    if(reset){probe_begin=esp_timer_get_time();probe_frame_begin=0;probe_wait_begin=0;probe_render_us=0;probe_wait_us=0;probe_pixels=0;probe_frames=0;probe_chunks=0;probe_max_us=0;probe_full_frames=0;probe_full_us=0;}
    char answer[240];snprintf(answer,sizeof(answer),"RENDERDIAG %llu %u %u %llu %llu %u %llu %u %llu\n",(unsigned long long)(esp_timer_get_time()-probe_begin),probe_frames,probe_chunks,(unsigned long long)probe_render_us,(unsigned long long)probe_wait_us,probe_max_us,(unsigned long long)probe_pixels,probe_full_frames,(unsigned long long)probe_full_us);
    esp_lv_adapter_unlock();reply(answer);
}
