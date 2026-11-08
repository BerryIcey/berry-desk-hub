#pragma once
#include <stdbool.h>
#include "esp_err.h"
bool berry_wifi_configure(const char *json);
esp_err_t berry_wifi_start(void);

void berry_wifi_diagnostics(unsigned *connected, unsigned *clock_ready, unsigned *successes, unsigned *failures);

extern volatile esp_err_t berry_wifi_start_status;
extern volatile unsigned berry_wifi_phase;
extern volatile unsigned berry_wifi_disconnect_reason;
extern volatile int berry_http_error,berry_http_status,berry_http_stage;
extern volatile int berry_tls_error,berry_tls_flags,berry_tls_code;

void berry_wifi_pause_reconnect(bool value);
esp_err_t berry_wifi_reload_networks(void);

void berry_wifi_clear_connection(void);
