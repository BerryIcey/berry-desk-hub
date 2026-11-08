#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"
typedef esp_err_t (*berry_prov_commit_fn)(const wifi_sta_config_t *candidate);
/* Caller pauses its own reconnect loop, supplies a random proof, and owns recovery. */
esp_err_t berry_prov_start(const char *device_name,const char *proof,berry_prov_commit_fn commit);
void berry_prov_stop(void);
void berry_prov_process_commit(void);
esp_err_t berry_prov_last_commit(void);
extern volatile int berry_prov_checkpoint;
