#pragma once
#include "berry_network_list.h"
#include "esp_err.h"
esp_err_t berry_network_store_load(berry_network_list_t *list);
esp_err_t berry_network_store_save(const berry_network_list_t *list);
esp_err_t berry_network_store_import_legacy(const char *ssid,const char *password);

esp_err_t berry_network_store_forget(const char *ssid);
