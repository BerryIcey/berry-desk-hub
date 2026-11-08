#include "berry_network_store.h"
#include <string.h>
#include "nvs.h"
#include "esp_err.h"
#include "cJSON.h"
#include <stdlib.h>

typedef struct { unsigned version; berry_network_list_t list; } stored_networks_t;
static bool valid(const berry_network_list_t *list) {
    if(!list || list->count>BERRY_NETWORK_LIMIT)return false;
    berry_network_list_t checked={0};
    for(size_t i=0;i<list->count;i++) {
        const berry_network_t *n=&list->entries[i];
        if(!memchr(n->ssid,0,sizeof(n->ssid)) || !memchr(n->password,0,sizeof(n->password)))return false;
        if(berry_network_find(&checked,n->ssid) || !berry_network_put(&checked,n->ssid,n->password))return false;
    }
    return true;
}
esp_err_t berry_network_store_load(berry_network_list_t *list) {
    if(!list)return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t rc=nvs_open("berry_nets",NVS_READONLY,&h);if(rc!=ESP_OK)return rc;
    stored_networks_t stored={0};size_t size=sizeof(stored);
    rc=nvs_get_blob(h,"entries",&stored,&size);nvs_close(h);
    if(rc==ESP_OK && (size!=sizeof(stored) || stored.version!=1 || !valid(&stored.list)))rc=ESP_ERR_INVALID_STATE;
    if(rc==ESP_OK)*list=stored.list;
    memset(&stored,0,sizeof(stored));return rc;
}
esp_err_t berry_network_store_save(const berry_network_list_t *list) {
    if(!valid(list))return ESP_ERR_INVALID_ARG;
    stored_networks_t stored={.version=1,.list=*list};nvs_handle_t h;
    esp_err_t rc=nvs_open("berry_nets",NVS_READWRITE,&h);
    if(rc==ESP_OK){rc=nvs_set_blob(h,"entries",&stored,sizeof(stored));if(rc==ESP_OK)rc=nvs_commit(h);nvs_close(h);}
    memset(&stored,0,sizeof(stored));return rc;
}

esp_err_t berry_network_store_import_legacy(const char *ssid,const char *password) {
    berry_network_list_t list={0};esp_err_t rc=berry_network_store_load(&list);
    if(rc==ESP_OK){memset(&list,0,sizeof(list));return ESP_OK;}
    if(rc!=ESP_ERR_NVS_NOT_FOUND)return rc;
    if(!berry_network_put(&list,ssid,password))return ESP_ERR_INVALID_ARG;
    rc=berry_network_store_save(&list);memset(&list,0,sizeof(list));return rc;
}

esp_err_t berry_network_store_forget(const char *ssid){
 if(!ssid || !*ssid)return ESP_ERR_INVALID_ARG;
 berry_network_list_t list={0};esp_err_t rc=berry_network_store_load(&list);
 if(rc!=ESP_OK)return rc;
 if(!berry_network_remove(&list,ssid)){memset(&list,0,sizeof(list));return ESP_ERR_NVS_NOT_FOUND;}
 nvs_handle_t h;rc=nvs_open("berry_wifi",NVS_READWRITE,&h);
 if(rc==ESP_OK){
  char json[1025]={0};size_t len=sizeof(json);rc=nvs_get_str(h,"config",json,&len);
  if(rc==ESP_OK){
   cJSON *obj=cJSON_ParseWithOpts(json,NULL,true);memset(json,0,sizeof(json));
   const cJSON *name=obj?cJSON_GetObjectItemCaseSensitive(obj,"ssid"):NULL;
   cJSON *pass=obj?cJSON_GetObjectItemCaseSensitive(obj,"password"):NULL;
   if(!cJSON_IsString(name) || !cJSON_IsString(pass))rc=ESP_ERR_INVALID_STATE;
   else if(strcmp(name->valuestring,ssid)==0){
    if(!cJSON_SetValuestring(pass,""))rc=ESP_ERR_NO_MEM;
    else {char *encoded=cJSON_PrintUnformatted(obj);if(!encoded)rc=ESP_ERR_NO_MEM;else {rc=nvs_set_str(h,"config",encoded);memset(encoded,0,strlen(encoded));free(encoded);if(rc==ESP_OK)rc=nvs_commit(h);}}
   }
   cJSON_Delete(obj);
  } else memset(json,0,sizeof(json));
  nvs_close(h);
 }
 if(rc==ESP_OK)rc=berry_network_store_save(&list);
 memset(&list,0,sizeof(list));return rc;
}
