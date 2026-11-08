#include "berry_wifi.h"
#include "berry_http_date.h"
#include "berry_clock_bootstrap.h"
#include "berry_serial.h"
#include "esp_attr.h"
#include "berry_network_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#define DATA_CAP 8192
#define CONNECTED BIT0
static EventGroupHandle_t events;
static berry_network_list_t networks;
static SemaphoreHandle_t network_lock;
static volatile bool provisioning;
static TickType_t last_attempt;
static bool attempt_started;
static char ssid[33], password[65], url[180], token[128];
static EXT_RAM_BSS_ATTR char response[DATA_CAP];
static size_t response_len;
static int64_t response_date;
static bool overflow;
static volatile unsigned successes,failures;
static unsigned transport_failures;
volatile esp_err_t berry_wifi_start_status=ESP_FAIL;
volatile unsigned berry_wifi_phase=0;
volatile unsigned berry_wifi_disconnect_reason=0;
volatile unsigned berry_diag_assoc=0,berry_diag_disconnect=0,berry_diag_ip=0,berry_diag_scan=0;
volatile int berry_freshness_delta=0,berry_freshness_stage=0;
volatile int berry_http_error=0,berry_http_status=0,berry_http_stage=0,berry_tls_error=0,berry_tls_flags=0,berry_tls_code=0;

static bool field(const cJSON *obj, const char *key, char *dst, size_t cap, bool required) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    size_t n=strlen(v->valuestring);
    if ((required && !n) || n>=cap) return false;
    for (size_t i=0;i<n;i++) if ((unsigned char)v->valuestring[i]<32 || v->valuestring[i]==127) return false;
    memcpy(dst,v->valuestring,n+1);return true;
}
static bool decode(const char *json) {
    cJSON *obj=cJSON_ParseWithOpts(json,NULL,true);
    bool ok=obj && field(obj,"ssid",ssid,sizeof(ssid),true) && field(obj,"password",password,sizeof(password),false) && field(obj,"url",url,sizeof(url),true) && field(obj,"token",token,sizeof(token),true);
    if(ok) ok=strncmp(url,"https://",8)==0 && strchr(url,'\r')==NULL && strchr(url,'\n')==NULL && (strlen(password)==0 || (strlen(password)>=8 && strlen(password)<=63));
    cJSON_Delete(obj);return ok;
}
bool berry_wifi_configure(const char *json) {
    if(!json || strlen(json)>1024 || !decode(json)) return false;
    nvs_handle_t handle; if(nvs_open("berry_wifi",NVS_READWRITE,&handle)!=ESP_OK)return false;
    esp_err_t rc=nvs_set_str(handle,"config",json);if(rc==ESP_OK)rc=nvs_commit(handle);nvs_close(handle);
    memset(password,0,sizeof(password));memset(token,0,sizeof(token));return rc==ESP_OK;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)data;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED)berry_diag_assoc++;
    if(base==WIFI_EVENT && id==WIFI_EVENT_SCAN_DONE)berry_diag_scan++;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED)berry_diag_disconnect++;
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP)berry_diag_ip++;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) { /* Connection task starts after initialization. */ }
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED){berry_wifi_disconnect_reason=data?((wifi_event_sta_disconnected_t *)data)->reason:0;xEventGroupClearBits(events,CONNECTED);}
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP)xEventGroupSetBits(events,CONNECTED);
}
static esp_err_t http_event(esp_http_client_event_t *e) {
    if(e->event_id==HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && strcasecmp(e->header_key,"Date")==0){int64_t parsed=0;if(berry_http_date_epoch(e->header_value,&parsed))response_date=parsed;}
    if(e->event_id==HTTP_EVENT_ON_DATA && e->data_len>0) {
        size_t n=(size_t)e->data_len;
        if(response_len+n>=sizeof(response))overflow=true;
        else {memcpy(response+response_len,e->data,n);response_len+=n;response[response_len]=0;}
    } return ESP_OK;
}
static bool current_payload(const char *body) {
    cJSON *obj=cJSON_ParseWithOpts(body,NULL,true);if(!obj)return false;
    const cJSON *status=cJSON_GetObjectItemCaseSensitive(obj,"status"),*ts=cJSON_GetObjectItemCaseSensitive(obj,"generated_at");
    int y,mo,d,h,mi,s; struct tm tm={0};bool ok=cJSON_IsString(status) && status->valuestring && strcmp(status->valuestring,"fresh")==0 && cJSON_IsString(ts) && ts->valuestring && sscanf(ts->valuestring,"%d-%d-%dT%d:%d:%d",&y,&mo,&d,&h,&mi,&s)==6;
    if(ok){tm.tm_year=y-1900;tm.tm_mon=mo-1;tm.tm_mday=d;tm.tm_hour=h;tm.tm_min=mi;tm.tm_sec=s;time_t observed=mktime(&tm), now=time(NULL);berry_freshness_delta=(int)difftime(observed,now);berry_freshness_stage=1;ok=now>=1700000000 && difftime(observed,now)<=5 && difftime(now,observed)<=120;}else berry_freshness_stage=-1;
    cJSON_Delete(obj);return ok;
}
static void connect_once(void) {
    static size_t cursor;
    if(provisioning || (xEventGroupGetBits(events)&CONNECTED))return;
    if(attempt_started && (TickType_t)(xTaskGetTickCount()-last_attempt)<pdMS_TO_TICKS(30000)){esp_wifi_connect();return;}
    wifi_ap_record_t *records=calloc(24,sizeof(*records));if(!records)return;
    uint16_t count=24;wifi_scan_config_t scan={.show_hidden=true};
    if(esp_wifi_scan_start(&scan,true)==ESP_OK && esp_wifi_scan_get_ap_records(&count,records)==ESP_OK){
        const char *visible[24];for(size_t i=0;i<count;i++)visible[i]=(const char *)records[i].ssid;
        if(xSemaphoreTake(network_lock,pdMS_TO_TICKS(1000))==pdTRUE){
            int index=berry_network_choose(&networks,visible,count,cursor);
            if(index>=0 && !provisioning){
                wifi_config_t cfg={0};berry_network_t *network=&networks.entries[index];
                memcpy(cfg.sta.ssid,network->ssid,strlen(network->ssid));memcpy(cfg.sta.password,network->password,strlen(network->password));
                cfg.sta.threshold.authmode=network->password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
                cursor=((size_t)index+1)%networks.count;
                if(esp_wifi_set_config(WIFI_IF_STA,&cfg)==ESP_OK){last_attempt=xTaskGetTickCount();attempt_started=true;esp_wifi_connect();}
                memset(&cfg,0,sizeof(cfg));
            }
            xSemaphoreGive(network_lock);
        }
    }
    free(records);
}

static void bootstrap_clock(void) {
    char plain_url[sizeof(url)];
    if(strncmp(url,"https://",8)!=0 || snprintf(plain_url,sizeof(plain_url),"http://%s",url+8)>=(int)sizeof(plain_url))return;
    esp_http_client_config_t conf={.url=plain_url,.method=HTTP_METHOD_HEAD,.event_handler=http_event,.timeout_ms=5000,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&conf);
    if(!client)return;
    response_date=0;response_len=0;overflow=false;
    esp_err_t rc=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);
    if(rc==ESP_OK && berry_clock_can_bootstrap((int64_t)time(NULL),status,response_date)){
        struct timeval tv={.tv_sec=(time_t)response_date,.tv_usec=0};(void)settimeofday(&tv,NULL);
    }
    esp_http_client_cleanup(client);
}

static void poll_task(void *arg) {
    (void)arg;
    unsigned unsynced_cycles=0;
    for(;;){
        connect_once();
        if(!provisioning && (xEventGroupGetBits(events)&CONNECTED) && time(NULL)<1700000000){
            if(++unsynced_cycles>=3){unsynced_cycles=0;bootstrap_clock();}
        }else unsynced_cycles=0;
        if(!provisioning && xEventGroupGetBits(events)&CONNECTED && time(NULL)>=1700000000) {
            esp_http_client_config_t conf={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,.event_handler=http_event,.timeout_ms=10000,.disable_auto_redirect=true};
            esp_http_client_handle_t client=esp_http_client_init(&conf);
            if(client){
                char auth[140];snprintf(auth,sizeof(auth),"Bearer %s",token);response_len=0;response_date=0;overflow=false;response[0]=0;
                esp_http_client_set_header(client,"Authorization",auth);esp_err_t rc=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);
                if(rc==ESP_OK && status==200 && response_date>0){time_t now=time(NULL);int64_t delta=response_date-(int64_t)now;if(delta>=-86400 && delta<=86400 && (delta>2 || delta< -2)){struct timeval tv={.tv_sec=(time_t)response_date,.tv_usec=0};(void)settimeofday(&tv,NULL);}}
                berry_http_error=(int)rc;berry_http_status=status;
                if(rc!=ESP_OK)berry_tls_code=(int)esp_http_client_get_and_clear_last_tls_error(client,(int *)&berry_tls_error,(int *)&berry_tls_flags);
                if(rc!=ESP_OK)berry_http_stage=1;
                else if(status!=200)berry_http_stage=2;
                else if(overflow)berry_http_stage=3;
                else if(!current_payload(response))berry_http_stage=4;
                else if(!berry_snapshot_publish(response))berry_http_stage=5;
                else {berry_http_stage=0;successes++;}
                if(berry_http_stage)failures++;
                memset(auth,0,sizeof(auth));esp_http_client_cleanup(client);
                if(!provisioning){
                    if(rc!=ESP_OK || status!=200)transport_failures++;else transport_failures=0;
                    if(transport_failures>=3){
                        bool alternatives=false;
                        if(xSemaphoreTake(network_lock,pdMS_TO_TICKS(1000))==pdTRUE){alternatives=networks.count>1;xSemaphoreGive(network_lock);}
                        if(alternatives){xEventGroupClearBits(events,CONNECTED);esp_wifi_disconnect();attempt_started=false;}
                        transport_failures=0;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
esp_err_t berry_wifi_start(void) {
    berry_wifi_phase=1; nvs_handle_t h;esp_err_t rc=nvs_open("berry_wifi",NVS_READONLY,&h);if(rc!=ESP_OK)return rc;
    char json[1025];size_t size=sizeof(json);rc=nvs_get_str(h,"config",json,&size);nvs_close(h);if(rc!=ESP_OK || !decode(json)){memset(json,0,sizeof(json));return ESP_ERR_INVALID_STATE;}memset(json,0,sizeof(json));
    network_lock=xSemaphoreCreateMutex();if(!network_lock)return ESP_ERR_NO_MEM;
    rc=berry_network_store_import_legacy(ssid,password);if(rc!=ESP_OK)return rc;
    rc=berry_network_store_load(&networks);if(rc!=ESP_OK)return rc;
    berry_wifi_phase=2; events=xEventGroupCreate();if(!events)return ESP_ERR_NO_MEM;
    berry_wifi_phase=3; if((rc=esp_netif_init())!=ESP_OK)return rc;
    berry_wifi_phase=4; if((rc=esp_event_loop_create_default())!=ESP_OK)return rc;
    berry_wifi_phase=5; if(!esp_netif_create_default_wifi_sta())return ESP_FAIL;
    berry_wifi_phase=6; wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    init.static_rx_buf_num=6;init.dynamic_rx_buf_num=16;init.dynamic_tx_buf_num=16;
    if((rc=esp_wifi_init(&init))!=ESP_OK)return rc;
    berry_wifi_phase=7; if((rc=esp_wifi_set_storage(WIFI_STORAGE_RAM))!=ESP_OK)return rc;
    berry_wifi_phase=8; if((rc=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,NULL))!=ESP_OK)return rc;
    if((rc=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,NULL))!=ESP_OK)return rc;
    berry_wifi_phase=9; wifi_config_t wifi={0};memcpy(wifi.sta.ssid,ssid,strlen(ssid));memcpy(wifi.sta.password,password,strlen(password));wifi.sta.threshold.authmode=strlen(password)?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
    if((rc=esp_wifi_set_mode(WIFI_MODE_STA))!=ESP_OK)return rc;
    if((rc=esp_wifi_set_config(WIFI_IF_STA,&wifi))!=ESP_OK)return rc;
    memset(&wifi,0,sizeof(wifi));memset(password,0,sizeof(password));
    berry_wifi_phase=10; if((rc=esp_wifi_start())!=ESP_OK)return rc;
    berry_wifi_phase=11; esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"ntp.aliyun.com");esp_sntp_init();
    berry_wifi_phase=12;
    return xTaskCreateWithCaps(poll_task,"berry_wifi",8192,NULL,4,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}

void berry_wifi_diagnostics(unsigned *connected,unsigned *clock_ready,unsigned *ok,unsigned *bad) {
    *connected=events && (xEventGroupGetBits(events)&CONNECTED)?1:0;
    *clock_ready=time(NULL)>=1700000000?1:0;*ok=successes;*bad=failures;
}

void berry_wifi_pause_reconnect(bool value){provisioning=value;if(!value)attempt_started=false;}
esp_err_t berry_wifi_reload_networks(void){
    berry_network_list_t next={0};esp_err_t rc=berry_network_store_load(&next);if(rc!=ESP_OK)return rc;
    if(!network_lock){memset(&next,0,sizeof(next));return ESP_ERR_INVALID_STATE;}
    if(xSemaphoreTake(network_lock,pdMS_TO_TICKS(1000))!=pdTRUE){memset(&next,0,sizeof(next));return ESP_ERR_TIMEOUT;}
    networks=next;xSemaphoreGive(network_lock);memset(&next,0,sizeof(next));return ESP_OK;
}

void berry_wifi_clear_connection(void){if(events)xEventGroupClearBits(events,CONNECTED);}
