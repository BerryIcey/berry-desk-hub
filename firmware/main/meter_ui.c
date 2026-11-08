#include "meter_ui.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/display.h"
#include "freertos/task.h"
#include "meter_activity_policy.h"
#include "meter_battery_policy.h"

#define COLOR_BACKGROUND 0x000000
#define COLOR_CARD 0x1C1C1E
#define COLOR_TRACK 0x3A3A3C
#define COLOR_PRIMARY_TEXT 0xF5F5F7
#define COLOR_SECONDARY_TEXT 0xA1A1A6
#define COLOR_TERTIARY_TEXT 0x636366
#define COLOR_HEALTHY 0x30D158
#define COLOR_ATTENTION 0xFF9F0A
#define COLOR_CRITICAL 0xFF453A
#define COLOR_BLUE 0x0A84FF

#define METER_SCREEN_WIDTH 480
#define METER_HEADER_RIGHT_MARGIN 24
#define METER_HEADER_X 248
#define METER_HEADER_Y 18
#define METER_HEADER_WIDTH 208
#define METER_HEADER_HEIGHT 36
#define METER_HEADER_BORDER_WIDTH 1
#define METER_HEADER_RIGHT_GUTTER 12
#define METER_HEADER_BATTERY_X 10
#define METER_HEADER_BATTERY_WIDTH 76
#define METER_HEADER_SEPARATOR_X 94
#define METER_HEADER_HALO_X 99
#define METER_HEADER_DOT_X 107
#define METER_HEADER_STATUS_X 125
#define METER_HEADER_STATUS_WIDTH 70
#define METER_MOTION_APPLY_PERIOD_MS 50

_Static_assert(METER_HEADER_X + METER_HEADER_WIDTH ==
                   METER_SCREEN_WIDTH - METER_HEADER_RIGHT_MARGIN,
               "Header must keep the screen edge margin");
_Static_assert(METER_HEADER_BORDER_WIDTH + METER_HEADER_STATUS_X +
                       METER_HEADER_STATUS_WIDTH + METER_HEADER_RIGHT_GUTTER <=
                   METER_HEADER_WIDTH,
               "Header status needs a safe right gutter");

_Static_assert(CONFIG_METER_URGENT_REMAINING < CONFIG_METER_CRITICAL_REMAINING,
               "Almost-out threshold must be lower than low threshold");
_Static_assert(CONFIG_METER_CRITICAL_REMAINING < CONFIG_METER_WARNING_REMAINING,
               "Low threshold must be lower than attention threshold");
_Static_assert(CONFIG_METER_BATTERY_CRITICAL_PERCENT <
                   CONFIG_METER_BATTERY_LOW_PERCENT,
               "Critical battery threshold must be lower than low threshold");

static const char *TAG = "meter_ui";

typedef enum {
    QUOTA_LEVEL_UNKNOWN = -1,
    QUOTA_LEVEL_PLENTY = 0,
    QUOTA_LEVEL_ATTENTION,
    QUOTA_LEVEL_LOW,
    QUOTA_LEVEL_URGENT,
} quota_level_t;

typedef struct {
    lv_obj_t *overview_page;
    lv_obj_t *details_page;

    lv_obj_t *header;
    lv_obj_t *header_battery;
    lv_obj_t *header_separator;
    lv_obj_t *header_status_halo;
    lv_obj_t *header_status_dot;
    lv_obj_t *header_status;

    lv_obj_t *plan;
    lv_obj_t *overview_account;
    lv_obj_t *ring;
    lv_obj_t *remaining;
    lv_obj_t *remaining_caption;
    lv_obj_t *health;
    lv_obj_t *window;
    lv_obj_t *reset;
    lv_obj_t *sync;
    lv_obj_t *overview_page_dot;

    lv_obj_t *detail_window;
    lv_obj_t *details_account;
    lv_obj_t *detail_remaining;
    lv_obj_t *detail_used;
    lv_obj_t *detail_reset;
    lv_obj_t *detail_bar;
    lv_obj_t *extra_name;
    lv_obj_t *extra_remaining;
    lv_obj_t *extra_used;
    lv_obj_t *extra_reset;
    lv_obj_t *extra_bar;
    lv_obj_t *transport;
    lv_obj_t *link_dot;
    lv_obj_t *link_state;
    lv_obj_t *host_sync;
    lv_obj_t *details_page_dot;
} meter_ui_t;

static meter_ui_t s_ui;
static quota_level_t s_primary_level = QUOTA_LEVEL_UNKNOWN;
static quota_level_t s_extra_level = QUOTA_LEVEL_UNKNOWN;
static int s_applied_primary_level = -2;
static int s_applied_extra_level = -2;
static int s_applied_link_state = -1;
static int s_applied_battery_state = -1;
static uint8_t s_active_page;
static meter_activity_policy_t s_activity_policy;
static bool s_press_tracked;
static meter_activity_action_t s_press_action;
static meter_ui_recovery_callback_t s_recovery_callback;
static void *s_recovery_context;
static meter_ui_motion_query_t s_motion_query;
static void *s_motion_context;

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_label_text_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[96];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    set_label_text(label, text);
}

static void render_account_identity(const usage_snapshot_t *snapshot)
{
    const char *label = "";
    unsigned selected = 0;
    unsigned count = 0;
    if (snapshot->account_count > 0 &&
        snapshot->account_count <= METER_MAX_ACCOUNTS &&
        snapshot->selected_account < snapshot->account_count) {
        selected = (unsigned)snapshot->selected_account + 1U;
        count = snapshot->account_count;
        label = snapshot->accounts[snapshot->selected_account].label;
    }

    if (count > 1) {
        set_label_text_fmt(s_ui.overview_account, "%s  |  %u/%u", label, selected,
                           count);
        set_label_text_fmt(s_ui.details_account, "%s  |  %u/%u", label, selected,
                           count);
    } else {
        set_label_text(s_ui.overview_account, label);
        set_label_text(s_ui.details_account, label);
    }
}

static void format_duration(int seconds, char *target, size_t size)
{
    if (seconds < 0) {
        snprintf(target, size, "--");
    } else if (seconds >= 86400) {
        snprintf(target, size, "%dd %dh", seconds / 86400, (seconds % 86400) / 3600);
    } else if (seconds >= 3600) {
        snprintf(target, size, "%dh %dm", seconds / 3600, (seconds % 3600) / 60);
    } else {
        snprintf(target, size, "%dm", seconds / 60);
    }
}

static const char *configured_transport_name(void)
{
#if CONFIG_METER_TRANSPORT_BLE
    return "BLE";
#elif CONFIG_METER_TRANSPORT_WIFI
    return "Wi-Fi";
#else
    return "Wi-Fi + BLE";
#endif
}

static const char *transport_source_name(meter_transport_source_t source)
{
    if (source == METER_SOURCE_BLE) {
        return "BLE";
    }
    if (source == METER_SOURCE_WIFI) {
        return "Wi-Fi";
    }
    return configured_transport_name();
}

static void format_window(int minutes, char *target, size_t size)
{
    if (minutes <= 0) {
        snprintf(target, size, "CODEX LIMIT");
    } else if (minutes % 1440 == 0) {
        snprintf(target, size, "%d-DAY LIMIT", minutes / 1440);
    } else if (minutes % 60 == 0) {
        snprintf(target, size, "%d-HOUR LIMIT", minutes / 60);
    } else {
        snprintf(target, size, "%d-MIN LIMIT", minutes);
    }
}

static quota_level_t raw_quota_level(int remaining)
{
    if (remaining >= CONFIG_METER_WARNING_REMAINING) {
        return QUOTA_LEVEL_PLENTY;
    }
    if (remaining >= CONFIG_METER_CRITICAL_REMAINING) {
        return QUOTA_LEVEL_ATTENTION;
    }
    if (remaining >= CONFIG_METER_URGENT_REMAINING) {
        return QUOTA_LEVEL_LOW;
    }
    return QUOTA_LEVEL_URGENT;
}

static quota_level_t stable_quota_level(quota_level_t current, int remaining)
{
    quota_level_t raw = raw_quota_level(remaining);
    if (current == QUOTA_LEVEL_UNKNOWN || raw >= current) {
        return raw;
    }

    int recovery_threshold = 0;
    if (current == QUOTA_LEVEL_ATTENTION) {
        recovery_threshold =
            CONFIG_METER_WARNING_REMAINING + CONFIG_METER_THEME_HYSTERESIS;
    } else if (current == QUOTA_LEVEL_LOW) {
        recovery_threshold =
            CONFIG_METER_CRITICAL_REMAINING + CONFIG_METER_THEME_HYSTERESIS;
    } else if (current == QUOTA_LEVEL_URGENT) {
        recovery_threshold =
            CONFIG_METER_URGENT_REMAINING + CONFIG_METER_THEME_HYSTERESIS;
    }

    return remaining >= recovery_threshold ? raw : current;
}

static lv_color_t quota_color(quota_level_t level)
{
    if (level == QUOTA_LEVEL_PLENTY) {
        return lv_color_hex(COLOR_HEALTHY);
    }
    if (level == QUOTA_LEVEL_ATTENTION) {
        return lv_color_hex(COLOR_ATTENTION);
    }
    if (level == QUOTA_LEVEL_LOW || level == QUOTA_LEVEL_URGENT) {
        return lv_color_hex(COLOR_CRITICAL);
    }
    return lv_color_hex(COLOR_BLUE);
}

static const char *quota_copy(quota_level_t level)
{
    if (level == QUOTA_LEVEL_PLENTY) {
        return "PLENTY";
    }
    if (level == QUOTA_LEVEL_ATTENTION) {
        return "PAY ATTENTION";
    }
    if (level == QUOTA_LEVEL_LOW) {
        return "RUNNING LOW";
    }
    if (level == QUOTA_LEVEL_URGENT) {
        return "ALMOST OUT";
    }
    return "WAITING FOR DATA";
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *make_page(lv_obj_t *screen)
{
    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, 480, 480);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    return page;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    return card;
}

static lv_obj_t *make_dot(lv_obj_t *parent, int x, bool active)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_pos(dot, x, 452);
    lv_obj_set_size(dot, active ? 18 : 7, 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        dot, active ? lv_color_hex(COLOR_BLUE) : lv_color_hex(COLOR_TERTIARY_TEXT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int width)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, width, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static lv_color_t quota_value_color(quota_level_t level)
{
    return level == QUOTA_LEVEL_UNKNOWN ? lv_color_hex(COLOR_TERTIARY_TEXT)
                                        : lv_color_hex(COLOR_PRIMARY_TEXT);
}

static void status_opa_cb(void *var, int32_t value)
{
    lv_obj_t *object = (lv_obj_t *)var;
    if (object) {
        lv_obj_set_style_opa(object, (lv_opa_t)value, LV_PART_MAIN);
    }
}

static void reset_status_animation(lv_obj_t *object, lv_opa_t opacity)
{
    if (!object) {
        return;
    }
    lv_anim_delete(object, status_opa_cb);
    lv_obj_set_style_opa(object, opacity, LV_PART_MAIN);
}

static void start_status_animation(lv_obj_t *object, int32_t start, int32_t end,
                                   uint32_t duration)
{
    if (!object) {
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, status_opa_cb);
    lv_anim_set_values(&animation, start, end);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_reverse_duration(&animation, duration);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

static void set_active_page(uint8_t page)
{
    s_active_page = page > 0 ? 1 : 0;
    if (s_active_page == 0) {
        lv_obj_remove_flag(s_ui.overview_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.details_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.overview_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.details_page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(lv_obj_get_screen(s_ui.overview_page));
}

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool apply_brightness_action(meter_activity_action_t action)
{
    int brightness;
    if (action == METER_ACTIVITY_WAKE) {
        brightness = CONFIG_METER_BRIGHTNESS;
    } else if (action == METER_ACTIVITY_DIM) {
        brightness = CONFIG_METER_IDLE_BRIGHTNESS;
    } else {
        return true;
    }

    esp_err_t result = bsp_display_brightness_set(brightness);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not apply display brightness: %s",
                 esp_err_to_name(result));
        return false;
    }

    meter_activity_policy_commit(&s_activity_policy, action);
    return true;
}

static void apply_pending_motion_activity(void)
{
    if (!s_motion_query || !s_motion_query(s_motion_context)) {
        return;
    }

    const meter_activity_action_t action =
        meter_activity_policy_on_motion(&s_activity_policy, monotonic_ms());
    if (action == METER_ACTIVITY_WAKE && apply_brightness_action(action)) {
        ESP_LOGI(TAG, "%s", "Display restored by QMI8658 motion");
    }
}

static void page_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        /* Motion that predates this gesture must make the same gesture interactive. */
        apply_pending_motion_activity();
        s_press_action =
            meter_activity_policy_on_touch(&s_activity_policy, monotonic_ms());
        s_press_tracked = true;
        if (s_press_action == METER_ACTIVITY_WAKE) {
            (void)apply_brightness_action(s_press_action);
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        const meter_activity_action_t action =
            meter_activity_policy_on_touch(&s_activity_policy, monotonic_ms());
        if (action == METER_ACTIVITY_WAKE) {
            (void)apply_brightness_action(action);
        }
        s_press_tracked = false;
        s_press_action = METER_ACTIVITY_NONE;
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED && s_recovery_callback) {
        const meter_activity_action_t action =
            meter_activity_policy_on_touch(&s_activity_policy, monotonic_ms());
        if (action == METER_ACTIVITY_WAKE) {
            (void)apply_brightness_action(action);
        }
        s_press_tracked = false;
        s_press_action = METER_ACTIVITY_NONE;
        lv_indev_t *input = lv_indev_active();
        if (input) {
            lv_indev_wait_release(input);
        }
        ESP_LOGW(TAG, "%s", "Physical setup recovery requested");
        s_recovery_callback(s_recovery_context);
        return;
    }
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    meter_activity_action_t gesture_action;
    if (s_press_tracked) {
        gesture_action = s_press_action;
        const meter_activity_action_t current_action =
            meter_activity_policy_on_touch(&s_activity_policy, monotonic_ms());
        if (current_action == METER_ACTIVITY_WAKE) {
            (void)apply_brightness_action(current_action);
        }
    } else {
        /* Keep programmatic/simulator clicks correct even without PRESSED. */
        apply_pending_motion_activity();
        gesture_action =
            meter_activity_policy_on_touch(&s_activity_policy, monotonic_ms());
        if (gesture_action == METER_ACTIVITY_WAKE) {
            (void)apply_brightness_action(gesture_action);
        }
    }
    s_press_tracked = false;
    s_press_action = METER_ACTIVITY_NONE;

    if (gesture_action == METER_ACTIVITY_WAKE) {
        return;
    }
    set_active_page(s_active_page == 0 ? 1 : 0);
    ESP_LOGI(TAG, "Showing %s page", s_active_page == 0 ? "overview" : "details");
}

static void idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    apply_pending_motion_activity();
    if (CONFIG_METER_IDLE_DIM_SECONDS == 0 ||
        CONFIG_METER_IDLE_BRIGHTNESS >= CONFIG_METER_BRIGHTNESS ||
        s_press_tracked) {
        return;
    }

    const meter_activity_action_t action = meter_activity_policy_on_timer(
        &s_activity_policy, monotonic_ms(),
        (uint32_t)CONFIG_METER_IDLE_DIM_SECONDS * 1000U);
    if (action == METER_ACTIVITY_DIM && apply_brightness_action(action)) {
        ESP_LOGI(TAG, "Display dimmed after %d seconds without touch or motion",
                 CONFIG_METER_IDLE_DIM_SECONDS);
    }
}

static void motion_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    apply_pending_motion_activity();
}

static void apply_primary_level(quota_level_t level)
{
    if (s_applied_primary_level == (int)level) {
        return;
    }

    lv_color_t accent = quota_color(level);
    lv_color_t value_color = quota_value_color(level);
    lv_obj_set_style_arc_color(s_ui.ring, accent, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_ui.remaining, value_color, 0);
    lv_obj_set_style_text_color(s_ui.detail_remaining, value_color, 0);
    lv_obj_set_style_text_color(s_ui.health, accent, 0);
    lv_obj_set_style_bg_color(s_ui.detail_bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.overview_page_dot, accent, 0);
    lv_obj_set_style_bg_color(s_ui.details_page_dot, accent, 0);
    s_applied_primary_level = (int)level;
}

static void apply_extra_level(quota_level_t level)
{
    if (s_applied_extra_level == (int)level) {
        return;
    }

    lv_color_t accent = quota_color(level);
    lv_obj_set_style_text_color(s_ui.extra_remaining, quota_value_color(level), 0);
    lv_obj_set_style_bg_color(s_ui.extra_bar, accent, LV_PART_INDICATOR);
    s_applied_extra_level = (int)level;
}

static void clear_quota_presentation(void)
{
    s_primary_level = QUOTA_LEVEL_UNKNOWN;
    s_extra_level = QUOTA_LEVEL_UNKNOWN;
    apply_primary_level(s_primary_level);
    apply_extra_level(s_extra_level);

    if (lv_arc_get_value(s_ui.ring) != 0) {
        lv_arc_set_value(s_ui.ring, 0);
    }
    if (lv_bar_get_value(s_ui.detail_bar) != 0) {
        lv_bar_set_value(s_ui.detail_bar, 0, LV_ANIM_OFF);
    }
    if (lv_bar_get_value(s_ui.extra_bar) != 0) {
        lv_bar_set_value(s_ui.extra_bar, 0, LV_ANIM_OFF);
    }

    set_label_text(s_ui.remaining, "--%");
    set_label_text(s_ui.detail_remaining, "--%");
    set_label_text(s_ui.detail_used, "-- used");
    set_label_text(s_ui.health, "WAITING FOR DATA");
    set_label_text(s_ui.window, "CODEX LIMIT");
    set_label_text(s_ui.detail_window, "CODEX LIMIT");
    set_label_text(s_ui.reset, "Waiting for usage data");
    set_label_text(s_ui.detail_reset, "Resets in --");
    set_label_text(s_ui.plan, "");
    set_label_text(s_ui.extra_name, "MODEL LIMIT");
    set_label_text(s_ui.extra_remaining, "--%");
    set_label_text(s_ui.extra_used, "-- used");
    set_label_text(s_ui.extra_reset, "Waiting for usage data");
}

static void apply_link_state(meter_link_state_t state)
{
    if (s_applied_link_state == (int)state) {
        return;
    }

    const char *text = "SYNCING";
    lv_color_t color = lv_color_hex(COLOR_BLUE);

    if (state == METER_LINK_SETUP) {
        text = "SETUP";
    } else if (state == METER_LINK_DISCOVERING) {
        text = "FINDING";
    } else if (state == METER_LINK_LIVE) {
        text = "LIVE";
        color = lv_color_hex(COLOR_HEALTHY);
    } else if (state == METER_LINK_STALE) {
        text = "CACHED";
        color = lv_color_hex(COLOR_ATTENTION);
    } else if (state == METER_LINK_OFFLINE) {
        text = "OFFLINE";
        color = lv_color_hex(COLOR_SECONDARY_TEXT);
    } else if (state == METER_LINK_AUTH_REQUIRED) {
        text = "PAIRING";
        color = lv_color_hex(COLOR_ATTENTION);
    }

    set_label_text(s_ui.header_status, text);
    set_label_text(s_ui.link_state, text);
    lv_obj_set_style_bg_color(s_ui.header_status_dot, color, 0);
    lv_obj_set_style_bg_color(s_ui.header_status_halo, color, 0);
    lv_obj_set_style_text_color(s_ui.header_status, color, 0);
    lv_obj_set_style_bg_color(s_ui.link_dot, color, 0);
    lv_obj_set_style_text_color(s_ui.link_state, color, 0);

    reset_status_animation(s_ui.header_status_dot, 220);
    reset_status_animation(s_ui.header_status_halo, LV_OPA_TRANSP);
    reset_status_animation(s_ui.link_dot, 220);

    if (state == METER_LINK_LIVE) {
        reset_status_animation(s_ui.header_status_dot, 120);
        reset_status_animation(s_ui.header_status_halo, 12);
        start_status_animation(s_ui.header_status_dot, 120, 255, 1600);
        start_status_animation(s_ui.header_status_halo, 12, 82, 1600);
    } else if (state == METER_LINK_CONNECTING || state == METER_LINK_SETUP ||
               state == METER_LINK_DISCOVERING || state == METER_LINK_AUTH_REQUIRED) {
        reset_status_animation(s_ui.header_status_dot, 96);
        reset_status_animation(s_ui.header_status_halo, 8);
        start_status_animation(s_ui.header_status_dot, 96, 235, 760);
        start_status_animation(s_ui.header_status_halo, 8, 62, 760);
    }

    s_applied_link_state = (int)state;
}

static const char *battery_symbol(meter_battery_icon_t icon)
{
    switch (icon) {
    case METER_BATTERY_ICON_QUARTER:
        return LV_SYMBOL_BATTERY_1;
    case METER_BATTERY_ICON_HALF:
        return LV_SYMBOL_BATTERY_2;
    case METER_BATTERY_ICON_THREE_QUARTERS:
        return LV_SYMBOL_BATTERY_3;
    case METER_BATTERY_ICON_FULL:
        return LV_SYMBOL_BATTERY_FULL;
    case METER_BATTERY_ICON_EMPTY:
    default:
        return LV_SYMBOL_BATTERY_EMPTY;
    }
}

static void apply_battery_state(const meter_battery_snapshot_t *battery)
{
    const meter_battery_policy_config_t config = {
        .critical_percent = CONFIG_METER_BATTERY_CRITICAL_PERCENT,
        .low_percent = CONFIG_METER_BATTERY_LOW_PERCENT,
        .stale_after_ms =
            (uint32_t)CONFIG_METER_BATTERY_POLL_SECONDS * 6U * 1000U,
    };
    const meter_battery_presentation_t presentation =
        meter_battery_policy_present(
            battery,
            (uint32_t)(esp_timer_get_time() / 1000ULL),
            &config);

    char text[24];
    if (presentation.state == METER_BATTERY_DISPLAY_EXTERNAL_POWER) {
        snprintf(text, sizeof(text), "USB");
    } else if (presentation.state == METER_BATTERY_DISPLAY_UNKNOWN) {
        snprintf(text, sizeof(text), "--%%");
    } else if (presentation.state == METER_BATTERY_DISPLAY_CHARGING) {
        snprintf(text, sizeof(text), "%s %d%%", LV_SYMBOL_CHARGE,
                 presentation.percent);
    } else {
        snprintf(text, sizeof(text), "%s %d%%",
                 battery_symbol(presentation.icon), presentation.percent);
    }
    set_label_text(s_ui.header_battery, text);

    if (s_applied_battery_state == (int)presentation.state) {
        return;
    }

    lv_color_t color = lv_color_hex(COLOR_SECONDARY_TEXT);
    if (presentation.state == METER_BATTERY_DISPLAY_UNKNOWN) {
        color = lv_color_hex(COLOR_TERTIARY_TEXT);
    } else if (presentation.state == METER_BATTERY_DISPLAY_CHARGING ||
               presentation.state == METER_BATTERY_DISPLAY_FULL) {
        color = lv_color_hex(COLOR_HEALTHY);
    } else if (presentation.state == METER_BATTERY_DISPLAY_LOW) {
        color = lv_color_hex(COLOR_ATTENTION);
    } else if (presentation.state == METER_BATTERY_DISPLAY_CRITICAL) {
        color = lv_color_hex(COLOR_CRITICAL);
    }
    lv_obj_set_style_text_color(s_ui.header_battery, color, 0);
    s_applied_battery_state = (int)presentation.state;
}

static void create_header(lv_obj_t *screen)
{
    s_ui.header = lv_obj_create(screen);
    lv_obj_set_pos(s_ui.header, METER_HEADER_X, METER_HEADER_Y);
    lv_obj_set_size(s_ui.header, METER_HEADER_WIDTH, METER_HEADER_HEIGHT);
    lv_obj_set_style_radius(s_ui.header, METER_HEADER_HEIGHT / 2, 0);
    lv_obj_set_style_bg_color(s_ui.header, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_ui.header, 180, 0);
    lv_obj_set_style_border_width(s_ui.header, METER_HEADER_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(s_ui.header, lv_color_hex(COLOR_TRACK), 0);
    lv_obj_set_style_border_opa(s_ui.header, 150, 0);
    lv_obj_set_style_pad_all(s_ui.header, 0, 0);
    lv_obj_remove_flag(s_ui.header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.header, LV_OBJ_FLAG_CLICKABLE);

    s_ui.header_battery = make_label(s_ui.header, "--%", &lv_font_montserrat_16,
                                     lv_color_hex(COLOR_TERTIARY_TEXT));
    lv_obj_set_width(s_ui.header_battery, METER_HEADER_BATTERY_WIDTH);
    lv_obj_set_style_text_align(s_ui.header_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ui.header_battery, METER_HEADER_BATTERY_X, 8);

    s_ui.header_separator = lv_obj_create(s_ui.header);
    lv_obj_set_pos(s_ui.header_separator, METER_HEADER_SEPARATOR_X, 10);
    lv_obj_set_size(s_ui.header_separator, 1, 16);
    lv_obj_set_style_bg_color(s_ui.header_separator, lv_color_hex(COLOR_TRACK), 0);
    lv_obj_set_style_bg_opa(s_ui.header_separator, 180, 0);
    lv_obj_set_style_border_width(s_ui.header_separator, 0, 0);
    lv_obj_set_style_pad_all(s_ui.header_separator, 0, 0);
    lv_obj_remove_flag(s_ui.header_separator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.header_separator, LV_OBJ_FLAG_CLICKABLE);

    s_ui.header_status_halo = lv_obj_create(s_ui.header);
    lv_obj_set_pos(s_ui.header_status_halo, METER_HEADER_HALO_X, 5);
    lv_obj_set_size(s_ui.header_status_halo, 24, 24);
    lv_obj_set_style_radius(s_ui.header_status_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.header_status_halo, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_bg_opa(s_ui.header_status_halo, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_ui.header_status_halo, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.header_status_halo, 0, 0);
    lv_obj_set_style_pad_all(s_ui.header_status_halo, 0, 0);
    lv_obj_remove_flag(s_ui.header_status_halo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.header_status_halo, LV_OBJ_FLAG_CLICKABLE);

    s_ui.header_status_dot = lv_obj_create(s_ui.header);
    lv_obj_set_pos(s_ui.header_status_dot, METER_HEADER_DOT_X, 13);
    lv_obj_set_size(s_ui.header_status_dot, 8, 8);
    lv_obj_set_style_radius(s_ui.header_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.header_status_dot, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_bg_opa(s_ui.header_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_ui.header_status_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.header_status_dot, 0, 0);
    lv_obj_set_style_pad_all(s_ui.header_status_dot, 0, 0);
    lv_obj_remove_flag(s_ui.header_status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.header_status_dot, LV_OBJ_FLAG_CLICKABLE);

    s_ui.header_status = make_label(s_ui.header, "SYNCING", &lv_font_montserrat_14,
                                    lv_color_hex(COLOR_BLUE));
    lv_obj_set_width(s_ui.header_status, METER_HEADER_STATUS_WIDTH);
    lv_obj_set_style_text_align(s_ui.header_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ui.header_status, METER_HEADER_STATUS_X, 9);
}

static void create_overview_page(lv_obj_t *screen)
{
    s_ui.overview_page = make_page(screen);
    lv_obj_add_event_cb(s_ui.overview_page, page_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.overview_page, page_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_ui.overview_page, page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.overview_page, page_event_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *title = make_label(s_ui.overview_page, "Codex", &lv_font_montserrat_24,
                                 lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_pos(title, 28, 23);

    s_ui.plan = make_label(s_ui.overview_page, "", &lv_font_montserrat_12,
                            lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.plan, 110, 32);
    lv_obj_set_width(s_ui.plan, 126);
    lv_label_set_long_mode(s_ui.plan, LV_LABEL_LONG_DOT);

    s_ui.overview_account =
        make_label(s_ui.overview_page, "", &lv_font_montserrat_12,
                   lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.overview_account, 28, 54);
    lv_obj_set_width(s_ui.overview_account, 424);
    lv_label_set_long_mode(s_ui.overview_account, LV_LABEL_LONG_SCROLL_CIRCULAR);

    s_ui.ring = lv_arc_create(s_ui.overview_page);
    lv_obj_set_pos(s_ui.ring, 108, 72);
    lv_obj_set_size(s_ui.ring, 264, 264);
    lv_arc_set_range(s_ui.ring, 0, 100);
    lv_arc_set_bg_angles(s_ui.ring, 0, 360);
    lv_arc_set_rotation(s_ui.ring, 270);
    lv_arc_set_value(s_ui.ring, 0);
    lv_obj_set_style_arc_width(s_ui.ring, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ui.ring, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ui.ring, 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ui.ring, lv_color_hex(COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_remove_style(s_ui.ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ui.ring, LV_OBJ_FLAG_CLICKABLE);

    s_ui.remaining = make_label(s_ui.overview_page, "--%", &lv_font_montserrat_48,
                                lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_width(s_ui.remaining, 220);
    lv_obj_set_style_text_align(s_ui.remaining, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.remaining, 130, 145);

    s_ui.remaining_caption =
        make_label(s_ui.overview_page, "REMAINING", &lv_font_montserrat_16,
                   lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_width(s_ui.remaining_caption, 180);
    lv_obj_set_style_text_align(s_ui.remaining_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.remaining_caption, 150, 204);

    s_ui.health = make_label(s_ui.overview_page, "WAITING FOR DATA", &lv_font_montserrat_14,
                             lv_color_hex(COLOR_BLUE));
    lv_obj_set_width(s_ui.health, 220);
    lv_obj_set_style_text_align(s_ui.health, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.health, 130, 235);

    s_ui.window = make_label(s_ui.overview_page, "CODEX LIMIT", &lv_font_montserrat_14,
                             lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_width(s_ui.window, 200);
    lv_obj_set_style_text_align(s_ui.window, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.window, 140, 276);

    s_ui.reset = make_label(s_ui.overview_page, "Waiting for usage data",
                            &lv_font_montserrat_20, lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_width(s_ui.reset, 420);
    lv_obj_set_style_text_align(s_ui.reset, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.reset, 30, 354);

    s_ui.sync = make_label(s_ui.overview_page, "Connecting to host",
                           &lv_font_montserrat_14, lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_width(s_ui.sync, 420);
    lv_obj_set_style_text_align(s_ui.sync, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.sync, 30, 397);

    s_ui.overview_page_dot = make_dot(s_ui.overview_page, 223, true);
    make_dot(s_ui.overview_page, 250, false);
}

static void create_details_page(lv_obj_t *screen)
{
    s_ui.details_page = make_page(screen);
    lv_obj_add_event_cb(s_ui.details_page, page_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.details_page, page_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_ui.details_page, page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.details_page, page_event_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *title = make_label(s_ui.details_page, "Usage details", &lv_font_montserrat_24,
                                  lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_pos(title, 28, 23);

    s_ui.details_account =
        make_label(s_ui.details_page, "", &lv_font_montserrat_12,
                   lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.details_account, 28, 54);
    lv_obj_set_width(s_ui.details_account, 424);
    lv_label_set_long_mode(s_ui.details_account, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_t *main_card = make_card(s_ui.details_page, 24, 70, 432, 132);
    s_ui.detail_window = make_label(main_card, "CODEX LIMIT", &lv_font_montserrat_14,
                                    lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.detail_window, 20, 16);

    s_ui.detail_remaining = make_label(main_card, "--%", &lv_font_montserrat_28,
                                       lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_pos(s_ui.detail_remaining, 20, 44);

    s_ui.detail_used = make_label(main_card, "-- used", &lv_font_montserrat_14,
                                  lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.detail_used, 21, 78);

    s_ui.detail_reset = make_label(main_card, "Resets in --", &lv_font_montserrat_14,
                                   lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_width(s_ui.detail_reset, 210);
    lv_obj_set_style_text_align(s_ui.detail_reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ui.detail_reset, 202, 50);

    s_ui.detail_bar = make_bar(main_card, 20, 105, 392);

    lv_obj_t *extra_card = make_card(s_ui.details_page, 24, 214, 432, 132);
    s_ui.extra_name = make_label(extra_card, "ADDITIONAL LIMIT", &lv_font_montserrat_14,
                                 lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.extra_name, 20, 15);
    lv_obj_set_width(s_ui.extra_name, 300);
    lv_label_set_long_mode(s_ui.extra_name, LV_LABEL_LONG_DOT);

    s_ui.extra_remaining = make_label(extra_card, "--%", &lv_font_montserrat_28,
                                      lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_pos(s_ui.extra_remaining, 20, 44);

    s_ui.extra_used = make_label(extra_card, "-- used", &lv_font_montserrat_14,
                                 lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.extra_used, 21, 78);

    s_ui.extra_reset = make_label(extra_card, "No separate model limit",
                                  &lv_font_montserrat_14,
                                  lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_width(s_ui.extra_reset, 210);
    lv_obj_set_style_text_align(s_ui.extra_reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ui.extra_reset, 202, 50);

    s_ui.extra_bar = make_bar(extra_card, 20, 105, 392);

    lv_obj_t *link_card = make_card(s_ui.details_page, 24, 358, 432, 70);
    s_ui.transport = make_label(
        link_card, configured_transport_name(), &lv_font_montserrat_18,
        lv_color_hex(COLOR_PRIMARY_TEXT));
    lv_obj_set_pos(s_ui.transport, 20, 13);

    s_ui.link_dot = lv_obj_create(link_card);
    lv_obj_set_pos(s_ui.link_dot, 321, 20);
    lv_obj_set_size(s_ui.link_dot, 9, 9);
    lv_obj_set_style_radius(s_ui.link_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.link_dot, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_bg_opa(s_ui.link_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.link_dot, 0, 0);
    lv_obj_set_style_pad_all(s_ui.link_dot, 0, 0);
    lv_obj_remove_flag(s_ui.link_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.link_dot, LV_OBJ_FLAG_CLICKABLE);

    s_ui.link_state = make_label(link_card, "SYNCING", &lv_font_montserrat_14,
                                 lv_color_hex(COLOR_BLUE));
    lv_obj_set_width(s_ui.link_state, 78);
    lv_obj_set_style_text_align(s_ui.link_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ui.link_state, 334, 12);

    s_ui.host_sync = make_label(link_card, "Waiting for host", &lv_font_montserrat_12,
                                lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_obj_set_pos(s_ui.host_sync, 20, 43);

    make_dot(s_ui.details_page, 223, false);
    s_ui.details_page_dot = make_dot(s_ui.details_page, 241, true);
}

void meter_ui_create(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    create_overview_page(screen);
    create_details_page(screen);
    create_header(screen);
    set_active_page(0);
    apply_primary_level(QUOTA_LEVEL_UNKNOWN);
    apply_extra_level(QUOTA_LEVEL_UNKNOWN);
    apply_link_state(METER_LINK_CONNECTING);
    apply_battery_state(NULL);

    meter_activity_policy_init(&s_activity_policy, monotonic_ms());
    s_press_tracked = false;
    s_press_action = METER_ACTIVITY_NONE;
    if (!lv_timer_create(idle_timer_cb, 1000, NULL)) {
        ESP_LOGW(TAG, "%s", "Could not create display idle timer");
    }
    if (s_motion_query) {
        if (!lv_timer_create(
                motion_timer_cb, METER_MOTION_APPLY_PERIOD_MS, NULL)) {
            ESP_LOGW(TAG, "%s", "Could not create motion activity timer");
        }
    }
}

void meter_ui_render(
    const usage_snapshot_t *snapshot,
    const meter_battery_snapshot_t *battery)
{
    apply_battery_state(battery);
    if (!snapshot) {
        return;
    }

    render_account_identity(snapshot);
    apply_link_state(snapshot->link_state);

    const char *source = transport_source_name(snapshot->source);
    set_label_text(s_ui.transport, source);

    TickType_t now = xTaskGetTickCount();
    int counter_elapsed =
        snapshot->valid ? (int)((now - snapshot->received_tick) / configTICK_RATE_HZ) : 0;
    int freshness_elapsed =
        snapshot->valid ? (int)((now - snapshot->freshness_tick) / configTICK_RATE_HZ) : 0;
    int reset_seconds =
        snapshot->resets_in >= 0 ? snapshot->resets_in - counter_elapsed : -1;
    int extra_reset_seconds =
        snapshot->extra_resets_in >= 0 ? snapshot->extra_resets_in - counter_elapsed : -1;
    if (reset_seconds < 0 && snapshot->resets_in >= 0) {
        reset_seconds = 0;
    }
    if (extra_reset_seconds < 0 && snapshot->extra_resets_in >= 0) {
        extra_reset_seconds = 0;
    }

    if (!snapshot->valid) {
        clear_quota_presentation();
        if (snapshot->link_state == METER_LINK_SETUP && snapshot->setup_ssid[0] != '\0') {
            set_label_text(s_ui.transport, "Wi-Fi setup");
            set_label_text_fmt(s_ui.sync, "Join %s  /  SETUP", snapshot->setup_ssid);
            set_label_text_fmt(s_ui.host_sync, "192.168.4.1  /  Key %s",
                               snapshot->setup_psk);
        } else if (snapshot->link_state == METER_LINK_AUTH_REQUIRED) {
            set_label_text(s_ui.sync, "Pairing required  /  Host");
            set_label_text(s_ui.host_sync, "Reopen setup to update the token");
        } else if (snapshot->link_state == METER_LINK_DISCOVERING) {
            set_label_text(s_ui.transport, "Wi-Fi");
            set_label_text(s_ui.sync, "Finding paired host  /  mDNS");
            set_label_text(s_ui.host_sync, "Manual host remains available");
        } else {
            set_label_text_fmt(s_ui.sync, "Connecting to host  /  %s", source);
            if (strcmp(source, "BLE") == 0) {
                set_label_text(s_ui.host_sync, "Waiting for paired BLE host");
            } else if (strcmp(source, "Wi-Fi") == 0) {
                set_label_text(s_ui.host_sync, "Waiting for network");
            } else {
                set_label_text(s_ui.host_sync, "Waiting for either transport");
            }
        }
        return;
    }

    char plan[sizeof(snapshot->plan)];
    snprintf(plan, sizeof(plan), "%s", snapshot->plan);
    for (size_t index = 0; plan[index] != '\0'; ++index) {
        plan[index] = (char)toupper((unsigned char)plan[index]);
    }
    set_label_text(s_ui.plan, plan);

    s_primary_level = stable_quota_level(s_primary_level, snapshot->remaining);
    apply_primary_level(s_primary_level);
    set_label_text(s_ui.health, quota_copy(s_primary_level));

    if (lv_arc_get_value(s_ui.ring) != snapshot->remaining) {
        lv_arc_set_value(s_ui.ring, snapshot->remaining);
    }
    if (lv_bar_get_value(s_ui.detail_bar) != snapshot->remaining) {
        lv_bar_set_value(s_ui.detail_bar, snapshot->remaining, LV_ANIM_OFF);
    }

    set_label_text_fmt(s_ui.remaining, "%d%%", snapshot->remaining);
    set_label_text_fmt(s_ui.detail_remaining, "%d%%", snapshot->remaining);
    set_label_text_fmt(s_ui.detail_used, "%d%% used", snapshot->used);

    char window[32];
    format_window(snapshot->window_mins, window, sizeof(window));
    set_label_text(s_ui.window, window);
    set_label_text(s_ui.detail_window, window);

    char duration[24];
    format_duration(reset_seconds, duration, sizeof(duration));
    set_label_text_fmt(s_ui.reset, "Resets in %s", duration);
    set_label_text_fmt(s_ui.detail_reset, "Resets in %s", duration);

    const char *freshness =
        snapshot->link_state == METER_LINK_LIVE ? "Updated" : "Cached";
    if (freshness_elapsed < 60) {
        set_label_text_fmt(s_ui.sync, "%s just now  /  %s", freshness, source);
    } else {
        set_label_text_fmt(s_ui.sync, "%s %dm ago  /  %s", freshness,
                           freshness_elapsed / 60, source);
    }
    if (snapshot->source == METER_SOURCE_BLE) {
        set_label_text(s_ui.host_sync, "Authenticated host push");
    } else {
        set_label_text_fmt(s_ui.host_sync, "Device cache every %ds",
                           CONFIG_METER_REFRESH_SECONDS);
    }

    if (snapshot->extra_remaining >= 0) {
        set_label_text(s_ui.extra_name, snapshot->extra_name);
        s_extra_level = stable_quota_level(s_extra_level, snapshot->extra_remaining);
        apply_extra_level(s_extra_level);
        if (lv_bar_get_value(s_ui.extra_bar) != snapshot->extra_remaining) {
            lv_bar_set_value(s_ui.extra_bar, snapshot->extra_remaining, LV_ANIM_OFF);
        }
        set_label_text_fmt(s_ui.extra_remaining, "%d%%", snapshot->extra_remaining);
        int extra_used = 100 - snapshot->extra_remaining;
        if (extra_used < 0) {
            extra_used = 0;
        } else if (extra_used > 100) {
            extra_used = 100;
        }
        set_label_text_fmt(s_ui.extra_used, "%d%% used", extra_used);
        format_duration(extra_reset_seconds, duration, sizeof(duration));
        set_label_text_fmt(s_ui.extra_reset, "Resets in %s", duration);
    } else {
        set_label_text(s_ui.extra_name, "MODEL LIMIT");
        s_extra_level = QUOTA_LEVEL_UNKNOWN;
        apply_extra_level(s_extra_level);
        if (lv_bar_get_value(s_ui.extra_bar) != 0) {
            lv_bar_set_value(s_ui.extra_bar, 0, LV_ANIM_OFF);
        }
        set_label_text(s_ui.extra_remaining, "--%");
        set_label_text(s_ui.extra_used, "-- used");
        set_label_text(s_ui.extra_reset, "Not separately metered");
    }
}

#ifdef METER_UI_SIMULATOR
void meter_ui_simulator_set_page(uint8_t page)
{
    set_active_page(page);
}
#endif

void meter_ui_set_recovery_callback(meter_ui_recovery_callback_t callback,
                                    void *context)
{
    s_recovery_callback = callback;
    s_recovery_context = context;
}

void meter_ui_set_motion_query(meter_ui_motion_query_t query, void *context)
{
    s_motion_query = query;
    s_motion_context = context;
}
