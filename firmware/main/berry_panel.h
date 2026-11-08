#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { char ssid[33]; bool secured; bool saved; int rssi; } berry_ap_t;
typedef struct { berry_ap_t aps[16]; unsigned count; int state; int result; int battery; bool charging; bool external; int remaining_minutes; } berry_panel_state_t;
void berry_panel_start(void);
void berry_panel_snapshot(berry_panel_state_t *out);
void berry_panel_scan(void);
bool berry_panel_connect(const char *ssid,const char *password);
bool berry_panel_connect_saved(const char *ssid,bool secured);

bool berry_panel_forget(const char *ssid);
