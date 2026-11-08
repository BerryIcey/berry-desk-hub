#include "berry_network_commit.h"
#include "berry_network_store.h"
#include <string.h>
#include "nvs.h"
#include "cJSON.h"

/* Call only after Wi-Fi confirms the new credentials. */
esp_err_t berry_network_commit_candidate(const wifi_sta_config_t *candidate) {
    if(!candidate)return ESP_ERR_INVALID_ARG;
    berry_network_list_t list={0};esp_err_t rc=berry_network_store_load(&list);
    if(rc==ESP_ERR_NVS_NOT_FOUND){
        nvs_handle_t h;rc=nvs_open("berry_wifi",NVS_READONLY,&h);if(rc!=ESP_OK)return rc;
        char json[1025]={0};size_t size=sizeof(json);rc=nvs_get_str(h,"config",json,&size);nvs_close(h);
        if(rc!=ESP_OK){memset(json,0,sizeof(json));return rc;}
        cJSON *obj=cJSON_ParseWithOpts(json,NULL,true);memset(json,0,sizeof(json));
        const cJSON *s=obj?cJSON_GetObjectItemCaseSensitive(obj,"ssid"):NULL;
        const cJSON *p=obj?cJSON_GetObjectItemCaseSensitive(obj,"password"):NULL;
        bool ok=cJSON_IsString(s) && cJSON_IsString(p) && berry_network_put(&list,s->valuestring,p->valuestring);
        cJSON_Delete(obj);if(!ok){memset(&list,0,sizeof(list));return ESP_ERR_INVALID_STATE;}
    }else if(rc!=ESP_OK)return rc;
    char ssid[33]={0},password[65]={0};memcpy(ssid,candidate->ssid,sizeof(candidate->ssid));memcpy(password,candidate->password,sizeof(candidate->password));
    if(!berry_network_put(&list,ssid,password))rc=ESP_ERR_INVALID_ARG;else rc=berry_network_store_save(&list);
    memset(&list,0,sizeof(list));memset(ssid,0,sizeof(ssid));memset(password,0,sizeof(password));return rc;
}
