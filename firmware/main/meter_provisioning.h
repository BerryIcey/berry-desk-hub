#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "meter_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*meter_provisioning_ready_cb_t)(const char *setup_ssid, const char *setup_psk,
                                              void *context);

/**
 * @brief Start the local SoftAP configuration portal.
 *
 * The caller must initialize esp-netif, the default event loop, and the Wi-Fi
 * driver before calling this function. When @p apsta is true, the station
 * interface remains enabled while the setup access point is active.
 */
esp_err_t meter_provisioning_start(const meter_config_t *current, bool apsta,
                                   meter_provisioning_ready_cb_t on_ready, void *context);

/**
 * @brief Stop the HTTP portal and restore the Wi-Fi mode active before start.
 *
 * Calling this function while the portal is stopped is safe.
 */
esp_err_t meter_provisioning_stop(void);

bool meter_provisioning_is_running(void);

#ifdef __cplusplus
}
#endif
