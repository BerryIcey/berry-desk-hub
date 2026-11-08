#include "berry_panel.h"
#include "berry_battery_estimate.h"
#include "berry_wifi.h"
#include "berry_network_commit.h"
#include "berry_network_store.h"
#include "meter_battery.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdlib.h>
#include <string.h>
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static berry_panel_state_t state={.battery=-1,.remaining_minutes=-1};
static char selected_ssid[33],selected_password[65];
static berry_battery_estimate_t estimate={.percent=-1};
static int64_t battery_sample_time;
static void battery_update(const meter_battery_snapshot_t *b,void *ctx){
 (void)ctx;int64_t now=esp_timer_get_time()/1000000;
 int remaining=berry_battery_estimate(&estimate,b->percent,b->valid&&b->battery_present&&!b->charging&&!b->vbus_present,now);
 portENTER_CRITICAL(&lock);battery_sample_time=now;state.battery=b->valid&&b->battery_present?b->percent:-1;state.charging=b->charging;state.external=b->vbus_present;state.remaining_minutes=remaining;portEXIT_CRITICAL(&lock);
}
void berry_panel_start(void){(void)meter_battery_start(battery_update,NULL);}
void berry_panel_snapshot(berry_panel_state_t *out){if(!out)return;portENTER_CRITICAL(&lock);*out=state;int64_t sample=battery_sample_time;portEXIT_CRITICAL(&lock);if(sample==0 || esp_timer_get_time()/1000000-sample>30){out->battery=-1;out->remaining_minutes=-1;}}
static void scan_task(void *arg){
 (void)arg;wifi_ap_record_t *records=calloc(24,sizeof(*records));uint16_t count=24;esp_err_t rc=ESP_ERR_NO_MEM;
 if(records){wifi_scan_config_t cfg={0};berry_wifi_pause_reconnect(true);
  for(unsigned attempt=0;attempt<4;attempt++){rc=esp_wifi_scan_start(&cfg,true);if(rc==ESP_OK)break;vTaskDelay(pdMS_TO_TICKS(1000));}
  if(rc==ESP_OK){rc=esp_wifi_scan_get_ap_records(&count,records);}
  berry_wifi_pause_reconnect(false);
 }
 berry_network_list_t saved={0};bool have_saved=berry_network_store_load(&saved)==ESP_OK;
 portENTER_CRITICAL(&lock);state.count=0;
 if(rc==ESP_OK)for(unsigned i=0;i<count&&state.count<16;i++){
   if(!records[i].ssid[0]){continue;}
   bool dup=false;for(unsigned j=0;j<state.count;j++)if(strncmp(state.aps[j].ssid,(char *)records[i].ssid,32)==0){dup=true;break;}if(dup)continue;
   berry_ap_t *a=&state.aps[state.count++];memset(a,0,sizeof(*a));memcpy(a->ssid,records[i].ssid,32);a->secured=records[i].authmode!=WIFI_AUTH_OPEN;a->saved=have_saved && berry_network_find(&saved,a->ssid)!=NULL;a->rssi=records[i].rssi;
 }
 state.state=0;state.result=rc==ESP_OK?0:-1;portEXIT_CRITICAL(&lock);memset(&saved,0,sizeof(saved));free(records);vTaskDelete(NULL);
}
void berry_panel_scan(void){portENTER_CRITICAL(&lock);bool ok=state.state==0;if(ok){state.state=1;state.result=0;}portEXIT_CRITICAL(&lock);if(ok && xTaskCreate(scan_task,"wifi_scan_ui",4096,NULL,4,NULL)!=pdPASS){portENTER_CRITICAL(&lock);state.state=0;state.result=-1;portEXIT_CRITICAL(&lock);}}
static void connect_task(void *arg){
 (void)arg;wifi_config_t cfg={0};memcpy(cfg.sta.ssid,selected_ssid,strlen(selected_ssid));memcpy(cfg.sta.password,selected_password,strlen(selected_password));cfg.sta.threshold.authmode=selected_password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
 berry_wifi_pause_reconnect(true);berry_wifi_clear_connection();(void)esp_wifi_disconnect();vTaskDelay(pdMS_TO_TICKS(300));
 esp_err_t rc=esp_wifi_set_storage(WIFI_STORAGE_RAM);if(rc==ESP_OK)rc=esp_wifi_set_config(WIFI_IF_STA,&cfg);if(rc==ESP_OK)rc=esp_wifi_connect();
 unsigned connected=0,clock=0,ok=0,bad=0;
 if(rc==ESP_OK){for(unsigned i=0;i<30;i++){vTaskDelay(pdMS_TO_TICKS(1000));berry_wifi_diagnostics(&connected,&clock,&ok,&bad);if(connected)break;}if(!connected)rc=ESP_ERR_TIMEOUT;}
 if(rc==ESP_OK){rc=berry_network_commit_candidate(&cfg.sta);if(rc==ESP_OK)rc=berry_wifi_reload_networks();}
 if(rc!=ESP_OK){berry_wifi_clear_connection();(void)esp_wifi_disconnect();}
 memset(&cfg,0,sizeof(cfg));memset(selected_password,0,sizeof(selected_password));berry_wifi_pause_reconnect(false);
 portENTER_CRITICAL(&lock);state.state=0;state.result=rc==ESP_OK?1:-2;portEXIT_CRITICAL(&lock);vTaskDelete(NULL);
}
bool berry_panel_connect(const char *ssid,const char *password){
 if(!ssid || !password || !*ssid || strlen(ssid)>32 || strlen(password)>63 || (*password && strlen(password)<8))return false;
 portENTER_CRITICAL(&lock);bool ok=state.state==0;if(ok){state.state=2;state.result=0;strcpy(selected_ssid,ssid);strcpy(selected_password,password);}portEXIT_CRITICAL(&lock);
 if(ok && xTaskCreate(connect_task,"wifi_connect_ui",6144,NULL,4,NULL)!=pdPASS){memset(selected_password,0,sizeof(selected_password));portENTER_CRITICAL(&lock);state.state=0;state.result=-2;portEXIT_CRITICAL(&lock);return false;}return ok;
}

bool berry_panel_connect_saved(const char *ssid,bool secured){
 berry_network_list_t saved={0};bool started=false;
 if(berry_network_store_load(&saved)==ESP_OK){
  const berry_network_t *network=berry_network_find(&saved,ssid);
  if(network && ((network->password[0]!=0)==secured))started=berry_panel_connect(ssid,network->password);
 }
 memset(&saved,0,sizeof(saved));return started;
}

static char forgotten_ssid[33];
static void forget_task(void *arg){
 (void)arg;berry_wifi_pause_reconnect(true);
 esp_err_t rc=berry_network_store_forget(forgotten_ssid);
 if(rc==ESP_OK)rc=berry_wifi_reload_networks();
 berry_wifi_pause_reconnect(false);
 portENTER_CRITICAL(&lock);
 if(rc==ESP_OK)for(unsigned i=0;i<state.count;i++)if(strcmp(state.aps[i].ssid,forgotten_ssid)==0)state.aps[i].saved=false;
 state.state=0;state.result=rc==ESP_OK?2:-3;portEXIT_CRITICAL(&lock);
 memset(forgotten_ssid,0,sizeof(forgotten_ssid));vTaskDelete(NULL);
}
bool berry_panel_forget(const char *ssid){
 if(!ssid || !*ssid || strlen(ssid)>32)return false;
 portENTER_CRITICAL(&lock);bool ok=state.state==0;
 if(ok){state.state=3;state.result=0;strcpy(forgotten_ssid,ssid);}portEXIT_CRITICAL(&lock);
 if(ok && xTaskCreate(forget_task,"wifi_forget_ui",6144,NULL,4,NULL)!=pdPASS){portENTER_CRITICAL(&lock);state.state=0;state.result=-3;portEXIT_CRITICAL(&lock);return false;}
 return ok;
}
