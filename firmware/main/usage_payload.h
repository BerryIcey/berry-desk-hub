#pragma once

#include <stdbool.h>

#include "usage_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Parse and validate the host's version-1 usage JSON payload. */
bool meter_usage_payload_parse(const char *json, usage_snapshot_t *result);

#ifdef __cplusplus
}
#endif
