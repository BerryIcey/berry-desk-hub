#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
uint32_t berry_power_user_epoch(void);
void berry_power_user_activity_locked(void);
esp_err_t berry_power_brief_wake_locked(void);
void berry_power_brief_tick_locked(void);
void berry_power_restore_brightness_locked(void);
int berry_power_brightness_locked(void);
esp_err_t berry_power_set_brightness_locked(int level);
void berry_power_save_brightness_locked(void);
esp_err_t berry_power_start(void);
esp_err_t berry_power_toggle(void);
bool berry_power_screen_off(void);
extern volatile int berry_power_start_status;
extern volatile unsigned berry_power_event_count;

/* Caller must hold the LVGL adapter lock. */
esp_err_t berry_power_set_screen_off_locked(bool value);
