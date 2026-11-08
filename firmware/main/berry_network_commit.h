#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"
esp_err_t berry_network_commit_candidate(const wifi_sta_config_t *candidate);
