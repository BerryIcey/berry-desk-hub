#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { int percent; int64_t timestamp; } berry_battery_estimate_t;
int berry_battery_estimate(berry_battery_estimate_t *s,int percent,bool discharging,int64_t now_seconds);
