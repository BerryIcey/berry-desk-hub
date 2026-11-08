#include "berry_pairing.h"
#include "berry_ble_provision.h"
#include "berry_network_commit.h"
#include "berry_wifi.h"
#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static bool active,cancelled;
static char pairing_name[32],pairing_proof[17];
static esp_err_t save_candidate(const wifi_sta_config_t *candidate){return berry_network_commit_candidate(candidate);}
bool berry_pairing_view(char *name,size_t name_size,char *proof,size_t proof_size){
    bool show;portENTER_CRITICAL(&mux);show=active;
    if(show){snprintf(name,name_size,"%s",pairing_name);snprintf(proof,proof_size,"%s",pairing_proof);}
    portEXIT_CRITICAL(&mux);return show;
}
static void pair_task(void *arg){
    (void)arg;uint8_t mac[6],random[12];esp_read_mac(mac,ESP_MAC_BT);esp_fill_random(random,sizeof(random));
    char name[32],proof[17];snprintf(name,sizeof(name),"PROV_Berry-%02X%02X%02X",mac[3],mac[4],mac[5]);
    static const char alphabet[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";for(size_t i=0;i<12;i++)proof[i]=alphabet[random[i]&31];proof[12]=0;memset(random,0,sizeof(random));
    berry_wifi_pause_reconnect(true);
    esp_err_t rc=berry_prov_start(name,proof,save_candidate);
    if(rc==ESP_OK){
        portENTER_CRITICAL(&mux);snprintf(pairing_name,sizeof(pairing_name),"%s",name);snprintf(pairing_proof,sizeof(pairing_proof),"%s",proof);portEXIT_CRITICAL(&mux);
        for(unsigned i=0;i<180;i++){
            vTaskDelay(pdMS_TO_TICKS(1000));
            berry_prov_process_commit();
            if(berry_prov_last_commit()==ESP_OK){vTaskDelay(pdMS_TO_TICKS(3000));esp_restart();}
            portENTER_CRITICAL(&mux);bool stop=cancelled;portEXIT_CRITICAL(&mux);if(stop)break;
        }
        berry_prov_stop();vTaskDelay(pdMS_TO_TICKS(2000));
    }
    esp_wifi_disconnect();
    berry_wifi_reload_networks();berry_wifi_pause_reconnect(false);
    portENTER_CRITICAL(&mux);active=false;cancelled=false;memset(pairing_name,0,sizeof(pairing_name));memset(pairing_proof,0,sizeof(pairing_proof));portEXIT_CRITICAL(&mux);
    memset(proof,0,sizeof(proof));vTaskDelete(NULL);
}
void berry_pairing_request(void){
    portENTER_CRITICAL(&mux);bool start=!active;if(start){active=true;cancelled=false;}portEXIT_CRITICAL(&mux);
    if(start && xTaskCreate(pair_task,"berry_pair",6144,NULL,4,NULL)!=pdPASS){portENTER_CRITICAL(&mux);active=false;portEXIT_CRITICAL(&mux);}
}
void berry_pairing_cancel(void){portENTER_CRITICAL(&mux);cancelled=true;portEXIT_CRITICAL(&mux);}
