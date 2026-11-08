#include "transport.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "meter_config.h"
#include "sdkconfig.h"
#include "transport_internal.h"
#include "transport_manager_policy.h"

/*
 * These objects are created before NimBLE reserves controller memory. The
 * callbacks copy bounded snapshots into the app model. Multi-account snapshots
 * are roughly 1 KiB, so keep both the queue storage and the dispatch receive
 * buffer in PSRAM instead of permanently inflating the task's internal stack.
 */
#define MANAGER_EVENT_QUEUE_LENGTH 4
#define MANAGER_EVENT_TASK_STACK 3072
#define MANAGER_EVENT_TASK_PRIORITY 6

static const char *TAG = "meter_transport";

typedef struct {
    meter_transport_source_t source;
} source_context_t;

typedef enum {
    MANAGER_EVENT_SNAPSHOT = 0,
    MANAGER_EVENT_LINK,
    MANAGER_EVENT_SETUP,
} manager_event_type_t;

typedef struct {
    manager_event_type_t type;
    union {
        usage_snapshot_t snapshot;
        meter_link_state_t link;
        struct {
            char ssid[sizeof(((usage_snapshot_t *)0)->setup_ssid)];
            char password[sizeof(((usage_snapshot_t *)0)->setup_psk)];
        } setup;
    } data;
} manager_event_t;

static const source_context_t s_wifi_context = {.source = METER_SOURCE_WIFI};
static const source_context_t s_ble_context = {.source = METER_SOURCE_BLE};
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_event_queue;
static StaticQueue_t s_event_queue_control;
static uint8_t *s_event_queue_storage;
static manager_event_t *s_dispatch_event;
static meter_transport_callbacks_t s_callbacks;
static meter_transport_policy_t s_policy = {
    .source_state = {
        [METER_SOURCE_NONE] = METER_LINK_OFFLINE,
        [METER_SOURCE_WIFI] = METER_LINK_OFFLINE,
        [METER_SOURCE_BLE] = METER_LINK_OFFLINE,
    },
};
static bool s_source_started[3];
static bool s_source_starting[3];
static usage_snapshot_t s_current_snapshot;
static meter_link_state_t s_last_published_link = METER_LINK_OFFLINE;
static bool s_setup_credentials_available;
static char s_setup_ssid[sizeof(((usage_snapshot_t *)0)->setup_ssid)];
static char s_setup_password[sizeof(((usage_snapshot_t *)0)->setup_psk)];

static void dispatch_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (xQueueReceive(s_event_queue, s_dispatch_event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (s_dispatch_event->type) {
        case MANAGER_EVENT_SNAPSHOT:
            s_callbacks.on_snapshot(&s_dispatch_event->data.snapshot,
                                    s_callbacks.context);
            break;
        case MANAGER_EVENT_LINK:
            s_callbacks.on_link(s_dispatch_event->data.link, s_callbacks.context);
            break;
        case MANAGER_EVENT_SETUP:
            if (s_callbacks.on_setup) {
                s_callbacks.on_setup(s_dispatch_event->data.setup.ssid,
                                     s_dispatch_event->data.setup.password,
                                     s_callbacks.context);
            }
            break;
        }
    }
}

/* Called with s_lock held so decisions and downstream ordering stay identical. */
static void enqueue_event_locked(const manager_event_t *event)
{
    if (xQueueSend(s_event_queue, event, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to enqueue transport event");
    }
}

static bool valid_source(meter_transport_source_t source)
{
    return source == METER_SOURCE_WIFI || source == METER_SOURCE_BLE;
}

static meter_link_state_t aggregate_link_locked(void)
{
    return meter_transport_policy_aggregate(&s_policy);
}

static void enqueue_link_or_setup_locked(meter_link_state_t link)
{
    manager_event_t event;
    if (link == METER_LINK_SETUP && s_setup_credentials_available) {
        event.type = MANAGER_EVENT_SETUP;
        snprintf(event.data.setup.ssid, sizeof(event.data.setup.ssid), "%s",
                 s_setup_ssid);
        snprintf(event.data.setup.password, sizeof(event.data.setup.password), "%s",
                 s_setup_password);
    } else {
        event.type = MANAGER_EVENT_LINK;
        event.data.link = link;
    }
    enqueue_event_locked(&event);
}

static bool snapshot_is_newer_locked(const usage_snapshot_t *snapshot,
                                     meter_transport_source_t source)
{
    if (!s_policy.has_snapshot) {
        return true;
    }
    if (snapshot->captured_at > s_current_snapshot.captured_at) {
        return true;
    }
    if (snapshot->captured_at < s_current_snapshot.captured_at) {
        return false;
    }

    /* Keep the active source stable for identical host cache generations. */
    return s_current_snapshot.source == source;
}

static void source_snapshot(const usage_snapshot_t *snapshot, void *context)
{
    const source_context_t *source_context = context;
    if (!snapshot || !source_context || !valid_source(source_context->source)) {
        return;
    }

    usage_snapshot_t accepted;
    bool publish_snapshot = false;
    bool publish_link = false;
    meter_link_state_t link;
    manager_event_t event;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    meter_transport_source_t source = source_context->source;
    s_policy.source_state[source] = snapshot->link_state;
    if (source == METER_SOURCE_WIFI) {
        /* Wi-Fi publishes a snapshot only after it has stopped the setup AP. */
        s_policy.setup_active = false;
    }
    if (snapshot_is_newer_locked(snapshot, source)) {
        accepted = *snapshot;
        accepted.source = source;
        if (s_policy.has_snapshot &&
            accepted.captured_at == s_current_snapshot.captured_at) {
            accepted.freshness_tick = s_current_snapshot.freshness_tick;
        }
        s_policy.has_snapshot = true;
        link = aggregate_link_locked();
        accepted.link_state = link;
        s_current_snapshot = accepted;
        s_last_published_link = link;
        publish_snapshot = true;
    } else {
        link = aggregate_link_locked();
        if (link != s_last_published_link) {
            s_last_published_link = link;
            publish_link = true;
        }
    }
    if (publish_snapshot) {
        event.type = MANAGER_EVENT_SNAPSHOT;
        event.data.snapshot = accepted;
        enqueue_event_locked(&event);
    } else if (publish_link) {
        enqueue_link_or_setup_locked(link);
    }
    xSemaphoreGive(s_lock);
}

static void source_link(meter_link_state_t state, void *context)
{
    const source_context_t *source_context = context;
    if (!source_context || !valid_source(source_context->source)) {
        return;
    }

    bool publish = false;
    meter_link_state_t aggregate;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_policy.source_state[source_context->source] = state;
    if (state == METER_LINK_SETUP &&
        source_context->source == METER_SOURCE_WIFI) {
        /*
         * provisioning_ready() delivers the link marker immediately before
         * its AP credentials. Let source_setup() publish the single complete
         * event so the UI never flashes OFFLINE between those callbacks.
         */
        xSemaphoreGive(s_lock);
        return;
    }
    aggregate = aggregate_link_locked();
    if (aggregate != s_last_published_link) {
        s_last_published_link = aggregate;
        publish = true;
    }
    if (publish) {
        enqueue_link_or_setup_locked(aggregate);
    }
    xSemaphoreGive(s_lock);
}

static void source_setup(const char *ssid, const char *password, void *context)
{
    (void)context;
    meter_link_state_t aggregate;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_policy.source_state[METER_SOURCE_WIFI] = METER_LINK_SETUP;
    s_policy.setup_active = true;
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), "%s", ssid ? ssid : "");
    snprintf(s_setup_password, sizeof(s_setup_password), "%s",
             password ? password : "");
    s_setup_credentials_available = true;
    aggregate = aggregate_link_locked();
    if (aggregate == METER_LINK_SETUP) {
        s_last_published_link = aggregate;
        /* Always refresh credentials, even when setup was already visible. */
        enqueue_link_or_setup_locked(aggregate);
    }
    xSemaphoreGive(s_lock);
}

#if CONFIG_METER_TRANSPORT_BLE
static bool ble_only_needs_initial_wifi_setup(void)
{
    meter_config_t config;
    esp_err_t result = meter_config_load(&config);
    bool needs_setup = result != ESP_OK || config.pairing_token[0] == '\0';
    memset(&config, 0, sizeof(config));
    return needs_setup;
}
#endif

static meter_transport_callbacks_t callbacks_for_source(
    meter_transport_source_t source)
{
    const source_context_t *context =
        source == METER_SOURCE_WIFI ? &s_wifi_context : &s_ble_context;
    const meter_transport_callbacks_t callbacks = {
        .on_snapshot = source_snapshot,
        .on_link = source_link,
        .on_setup = source == METER_SOURCE_WIFI ? source_setup : NULL,
        .context = (void *)context,
    };
    return callbacks;
}

static esp_err_t start_source(meter_transport_source_t source)
{
    const meter_transport_callbacks_t callbacks = callbacks_for_source(source);

    return source == METER_SOURCE_WIFI ? meter_wifi_transport_start(&callbacks)
                                       : meter_ble_transport_start(&callbacks);
}

static esp_err_t start_wifi_setup_source(void)
{
    const meter_transport_callbacks_t callbacks =
        callbacks_for_source(METER_SOURCE_WIFI);
    return meter_wifi_transport_start_setup(&callbacks);
}

esp_err_t meter_transport_start(const meter_transport_callbacks_t *callbacks)
{
    if (!callbacks || !callbacks->on_snapshot || !callbacks->on_link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    const size_t event_queue_bytes =
        MANAGER_EVENT_QUEUE_LENGTH * sizeof(manager_event_t);
    /* Multi-account snapshots are intentionally copied through the queue. Put
     * that bounded data storage in byte-addressable PSRAM so display startup
     * does not leave NimBLE without a large enough internal-RAM block. Queue
     * control and the dispatch task remain in internal RAM. */
    s_event_queue_storage = heap_caps_malloc(
        event_queue_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_event_queue_storage) {
        ESP_LOGE(TAG, "Unable to allocate %u-byte event queue in PSRAM",
                 (unsigned)event_queue_bytes);
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_dispatch_event = heap_caps_malloc(sizeof(*s_dispatch_event),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dispatch_event) {
        ESP_LOGE(TAG, "Unable to allocate %u-byte dispatch buffer in PSRAM",
                 (unsigned)sizeof(*s_dispatch_event));
        heap_caps_free(s_event_queue_storage);
        s_event_queue_storage = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_event_queue = xQueueCreateStatic(
        MANAGER_EVENT_QUEUE_LENGTH, sizeof(manager_event_t),
        s_event_queue_storage, &s_event_queue_control);
    if (!s_event_queue) {
        heap_caps_free(s_dispatch_event);
        s_dispatch_event = NULL;
        heap_caps_free(s_event_queue_storage);
        s_event_queue_storage = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_callbacks = *callbacks;
    if (xTaskCreate(dispatch_task, "transport_events", MANAGER_EVENT_TASK_STACK, NULL,
                    MANAGER_EVENT_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        heap_caps_free(s_dispatch_event);
        s_dispatch_event = NULL;
        heap_caps_free(s_event_queue_storage);
        s_event_queue_storage = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_METER_TRANSPORT_WIFI
    bool start_wifi = true;
    const bool start_ble = false;
#elif CONFIG_METER_TRANSPORT_BLE
    /*
     * BLE-only skips Wi-Fi after pairing, but a factory-fresh device still
     * needs the protected setup AP once to receive its pairing token.
     */
    bool start_wifi = ble_only_needs_initial_wifi_setup();
    const bool start_ble = true;
#else
    /* AUTO and BOTH both keep the local Wi-Fi pull path and BLE push path alive. */
    bool start_wifi = true;
    const bool start_ble = true;
#endif

    s_policy.source_enabled[METER_SOURCE_WIFI] = start_wifi;
    s_policy.source_enabled[METER_SOURCE_BLE] = start_ble;

    esp_err_t ble_result = ESP_ERR_NOT_SUPPORTED;
    esp_err_t wifi_result = ESP_ERR_NOT_SUPPORTED;
    if (start_ble) {
        ble_result = start_source(METER_SOURCE_BLE);
        s_source_started[METER_SOURCE_BLE] = ble_result == ESP_OK;
        if (!s_source_started[METER_SOURCE_BLE]) {
            ESP_LOGW(TAG, "BLE transport unavailable: %s", esp_err_to_name(ble_result));
            s_policy.source_enabled[METER_SOURCE_BLE] = false;
        }
    }
    if (start_wifi) {
        wifi_result = start_source(METER_SOURCE_WIFI);
        s_source_started[METER_SOURCE_WIFI] =
            wifi_result == ESP_OK || wifi_result == ESP_ERR_INVALID_STATE;
        if (!s_source_started[METER_SOURCE_WIFI]) {
            ESP_LOGW(TAG, "Wi-Fi transport unavailable: %s", esp_err_to_name(wifi_result));
            s_policy.source_enabled[METER_SOURCE_WIFI] = false;
        }
    }

    if ((start_ble && ble_result == ESP_OK) ||
        (start_wifi && (wifi_result == ESP_OK || wifi_result == ESP_ERR_INVALID_STATE))) {
        return ESP_OK;
    }
    return start_wifi ? wifi_result : ble_result;
}

esp_err_t meter_transport_request_setup(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_source_starting[METER_SOURCE_WIFI]) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool already_started = s_source_started[METER_SOURCE_WIFI];
    if (!already_started) {
        s_source_starting[METER_SOURCE_WIFI] = true;
        s_policy.source_enabled[METER_SOURCE_WIFI] = true;
    }
    xSemaphoreGive(s_lock);

    esp_err_t result = already_started ? meter_wifi_transport_request_setup()
                                       : start_wifi_setup_source();
    if (already_started) {
        return result;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_source_starting[METER_SOURCE_WIFI] = false;
    s_source_started[METER_SOURCE_WIFI] = result == ESP_OK;
    if (result != ESP_OK) {
        s_policy.source_enabled[METER_SOURCE_WIFI] = false;
        s_policy.source_state[METER_SOURCE_WIFI] = METER_LINK_OFFLINE;
    }
    xSemaphoreGive(s_lock);
    return result;
}
