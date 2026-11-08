#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { BERRY_BUTTON_NONE=0, BERRY_BUTTON_NEXT, BERRY_BUTTON_PREVIOUS, BERRY_BUTTON_PANEL } berry_button_action_t;
typedef struct { bool down, long_fired, pending, second; uint32_t down_at, released_at; } berry_button_policy_t;
berry_button_action_t berry_button_update(berry_button_policy_t *state,bool down,uint32_t now);

typedef struct { bool armed,down; uint32_t down_at; } berry_boot_button_t;
berry_button_action_t berry_boot_button_update(berry_boot_button_t *state,bool down,uint32_t now);
