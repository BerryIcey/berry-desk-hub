#include "meter_config_requirements.h"

bool meter_config_required_fields_present(const char *wifi_ssid,
                                          const char *pairing_token,
                                          const char *host_id,
                                          const char *manual_host,
                                          bool ble_only)
{
    if (!pairing_token || pairing_token[0] == '\0') {
        return false;
    }
    if (ble_only) {
        return true;
    }
    return wifi_ssid && wifi_ssid[0] != '\0' &&
           ((host_id && host_id[0] != '\0') ||
            (manual_host && manual_host[0] != '\0'));
}
