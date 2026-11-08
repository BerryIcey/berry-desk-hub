#include "berry_ble_provision.h"
#include <string.h>
#include "esp_event.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static wifi_sta_config_t candidate;
static berry_prov_commit_fn commit_callback;
static bool pending,ready,handler_registered;
static portMUX_TYPE candidate_lock=portMUX_INITIALIZER_UNLOCKED;
RTC_DATA_ATTR volatile int berry_prov_checkpoint=0;
static volatile esp_err_t last_commit=ESP_ERR_INVALID_STATE;
static void clear_candidate(void){memset(&candidate,0,sizeof(candidate));pending=false;}
static void provisioning_event(void *arg,esp_event_base_t base,int32_t id,void *data){
    (void)arg;
    if(base!=WIFI_PROV_EVENT)return;
    if(id==WIFI_PROV_CRED_RECV && data){portENTER_CRITICAL(&candidate_lock);candidate=*(const wifi_sta_config_t *)data;pending=true;ready=false;portEXIT_CRITICAL(&candidate_lock);berry_prov_checkpoint=1;}
    else if(id==WIFI_PROV_CRED_FAIL){berry_prov_checkpoint=-1;portENTER_CRITICAL(&candidate_lock);clear_candidate();ready=false;portEXIT_CRITICAL(&candidate_lock);wifi_prov_mgr_reset_sm_state_on_failure();}
    else if(id==WIFI_PROV_CRED_SUCCESS){
        berry_prov_checkpoint=2;
        portENTER_CRITICAL(&candidate_lock);ready=pending;portEXIT_CRITICAL(&candidate_lock);
    }
    else if(id==WIFI_PROV_END){wifi_prov_mgr_deinit();}

}
esp_err_t berry_prov_start(const char *name,const char *proof,berry_prov_commit_fn commit){
    if(!name || !*name || !proof || strlen(proof)<8 || !commit)return ESP_ERR_INVALID_ARG;
    wifi_prov_mgr_config_t config={.scheme=wifi_prov_scheme_ble,.scheme_event_handler=WIFI_PROV_EVENT_HANDLER_NONE};
    esp_err_t rc=ESP_OK;
    if(!handler_registered){rc=esp_event_handler_register(WIFI_PROV_EVENT,ESP_EVENT_ANY_ID,provisioning_event,NULL);if(rc!=ESP_OK)return rc;handler_registered=true;}
    rc=wifi_prov_mgr_init(config);if(rc!=ESP_OK)return rc;
    last_commit=ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&candidate_lock);clear_candidate();ready=false;commit_callback=commit;portEXIT_CRITICAL(&candidate_lock);
    rc=wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,proof,name,NULL);
    if(rc!=ESP_OK){commit_callback=NULL;wifi_prov_mgr_deinit();}
    return rc;
}
void berry_prov_stop(void){wifi_prov_mgr_stop_provisioning();}

esp_err_t berry_prov_last_commit(void){return last_commit;}

void berry_prov_process_commit(void){
    wifi_sta_config_t copy;berry_prov_commit_fn callback=NULL;
    portENTER_CRITICAL(&candidate_lock);
    if(ready && pending){copy=candidate;callback=commit_callback;ready=false;clear_candidate();}
    portEXIT_CRITICAL(&candidate_lock);
    if(callback){last_commit=callback(&copy);berry_prov_checkpoint=last_commit==ESP_OK?3:-(int)last_commit;memset(&copy,0,sizeof(copy));}
}
