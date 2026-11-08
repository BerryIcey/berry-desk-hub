#pragma once

#include <stdbool.h>

/** Checks only which non-empty fields a transport mode requires. */
bool meter_config_required_fields_present(const char *wifi_ssid,
                                          const char *pairing_token,
                                          const char *host_id,
                                          const char *manual_host,
                                          bool ble_only);
