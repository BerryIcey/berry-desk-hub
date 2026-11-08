#pragma once
#include "lvgl.h"
#include "berry_button_policy.h"
void berry_ui_create(lv_display_t *display);
void berry_ui_diagnostics(unsigned *ticks, unsigned *updates, unsigned *page_index, int *quota, int *cpu);

void berry_ui_button_action(berry_button_action_t action);

int berry_ui_panel_state(void);

bool berry_ui_test_wifi_click(void);

bool berry_ui_test_keyboard(void);
