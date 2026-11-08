#include "transport_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mbedtls/md.h"
#include "meter_config.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "usage_payload.h"

#define BLE_STREAM_MAX 2048
#define BLE_STREAM_MIN 34
#define BLE_CHUNK_HEADER_SIZE 12
#define BLE_CHUNK_VALUE_MAX 512
#define BLE_FLAG_FIRST 0x01
#define BLE_FLAG_LAST 0x02
#define BLE_PROTOCOL_VERSION 1
#define BLE_CHALLENGE_SIZE 16
#define BLE_MAC_SIZE 32
#define BLE_HELLO_SIZE 52
#define BLE_ACK_SIZE 8
#define BLE_CONN_NONE BLE_HS_CONN_HANDLE_NONE
#define BLE_AUTH_DOMAIN "codex-meter-ble-v1\n"
#define BLE_DEVICE_DOMAIN "codex-meter-ble-device-v1\n"

typedef enum {
    BLE_ACK_OK = 0,
    BLE_ACK_BAD_FRAME = 1,
    BLE_ACK_AUTH_FAILED = 2,
    BLE_ACK_REPLAY = 3,
    BLE_ACK_JSON_INVALID = 4,
    BLE_ACK_TOO_LARGE = 5,
    BLE_ACK_BUSY = 6,
} ble_ack_status_t;

typedef struct {
    uint8_t *stream;
    uint16_t total;
    uint16_t received;
    uint32_t sequence;
    uint16_t connection_handle;
    uint32_t connection_epoch;
    uint8_t challenge[BLE_CHALLENGE_SIZE];
} ble_reassembly_t;

typedef struct {
    uint8_t *stream;
    uint16_t length;
    uint32_t sequence;
    uint16_t connection_handle;
    uint32_t connection_epoch;
    uint8_t challenge[BLE_CHALLENGE_SIZE];
} ble_message_t;

static const char *TAG = "meter_ble";

/* UUIDs are encoded least-significant byte first for BLE_UUID128_INIT. */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x01, 0xd1, 0x28, 0x16, 0xf9, 0x94, 0x7a, 0x8c,
                     0x46, 0x4d, 0x88, 0x2b, 0x91, 0xcb, 0x2f, 0x7c);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x02, 0xd1, 0x28, 0x16, 0xf9, 0x94, 0x7a, 0x8c,
                     0x46, 0x4d, 0x88, 0x2b, 0x91, 0xcb, 0x2f, 0x7c);
static const ble_uuid128_t s_ack_uuid =
    BLE_UUID128_INIT(0x03, 0xd1, 0x28, 0x16, 0xf9, 0x94, 0x7a, 0x8c,
                     0x46, 0x4d, 0x88, 0x2b, 0x91, 0xcb, 0x2f, 0x7c);

static meter_transport_callbacks_t s_callbacks;
static meter_config_t s_config;
static QueueHandle_t s_message_queue;
static SemaphoreHandle_t s_connection_mutex;
static ble_reassembly_t s_reassembly;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_challenge[BLE_CHALLENGE_SIZE];
static uint16_t s_connection_handle = BLE_CONN_NONE;
static uint32_t s_connection_epoch;
static uint16_t s_ack_value_handle;
static bool s_ack_subscribed;
static bool s_message_in_flight;
static uint16_t s_in_flight_connection_handle = BLE_CONN_NONE;
static uint32_t s_in_flight_connection_epoch;
static bool s_has_snapshot;
static uint8_t s_own_address_type;
static char s_device_name[20];

static void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal free=%u largest=%u, PSRAM free=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT));
}

static int gatt_access(uint16_t connection_handle, uint16_t attribute_handle,
                       struct ble_gatt_access_ctxt *context, void *argument);
static int gap_event(struct ble_gap_event *event, void *argument);

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access,
                /* WRITE (without WRITE_NO_RSP) requires a GATT write response. */
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_ack_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ack_value_handle,
            },
            {0},
        },
    },
    {0},
};

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t index = 0; index < length; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

static bool hmac_parts(const char *domain, const uint8_t *challenge,
                       const uint8_t *sequence, const uint8_t *body, size_t body_length,
                       uint8_t output[BLE_MAC_SIZE])
{
    bool success = false;
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    size_t key_length = strlen(s_config.pairing_token);

    if (info && key_length > 0 &&
        mbedtls_md_setup(&context, info, 1) == 0 &&
        mbedtls_md_hmac_starts(&context, (const uint8_t *)s_config.pairing_token,
                               key_length) == 0 &&
        mbedtls_md_hmac_update(&context, (const uint8_t *)domain, strlen(domain)) == 0 &&
        mbedtls_md_hmac_update(&context, challenge, BLE_CHALLENGE_SIZE) == 0 &&
        (!sequence || mbedtls_md_hmac_update(&context, sequence, sizeof(uint32_t)) == 0) &&
        (!body || mbedtls_md_hmac_update(&context, body, body_length) == 0) &&
        mbedtls_md_hmac_finish(&context, output) == 0) {
        success = true;
    }
    mbedtls_md_free(&context);
    return success;
}

static void rotate_challenge(void)
{
    uint8_t next[BLE_CHALLENGE_SIZE];
    esp_fill_random(next, sizeof(next));
    portENTER_CRITICAL(&s_state_lock);
    memcpy(s_challenge, next, sizeof(s_challenge));
    portEXIT_CRITICAL(&s_state_lock);
    memset(next, 0, sizeof(next));
}

static uint32_t next_connection_epoch(uint32_t epoch)
{
    ++epoch;
    return epoch == 0 ? 1 : epoch;
}

static void activate_connection(uint16_t connection_handle)
{
    uint8_t next[BLE_CHALLENGE_SIZE];
    esp_fill_random(next, sizeof(next));
    portENTER_CRITICAL(&s_state_lock);
    s_connection_epoch = next_connection_epoch(s_connection_epoch);
    s_connection_handle = connection_handle;
    s_ack_subscribed = false;
    memcpy(s_challenge, next, sizeof(s_challenge));
    portEXIT_CRITICAL(&s_state_lock);
    memset(next, 0, sizeof(next));
}

static void invalidate_connection(uint16_t connection_handle, bool require_match)
{
    uint8_t next[BLE_CHALLENGE_SIZE];
    esp_fill_random(next, sizeof(next));
    portENTER_CRITICAL(&s_state_lock);
    if (!require_match || s_connection_handle == connection_handle) {
        s_connection_epoch = next_connection_epoch(s_connection_epoch);
        s_connection_handle = BLE_CONN_NONE;
        s_ack_subscribed = false;
        memcpy(s_challenge, next, sizeof(s_challenge));
    }
    portEXIT_CRITICAL(&s_state_lock);
    memset(next, 0, sizeof(next));
}

static bool capture_connection(uint16_t connection_handle, uint32_t *epoch,
                               uint8_t challenge[BLE_CHALLENGE_SIZE])
{
    bool current;
    portENTER_CRITICAL(&s_state_lock);
    current = s_connection_handle == connection_handle;
    if (current) {
        *epoch = s_connection_epoch;
        if (challenge) {
            memcpy(challenge, s_challenge, BLE_CHALLENGE_SIZE);
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return current;
}

static bool connection_is_current(uint16_t connection_handle, uint32_t epoch)
{
    bool current;
    portENTER_CRITICAL(&s_state_lock);
    current = s_connection_handle == connection_handle && s_connection_epoch == epoch;
    portEXIT_CRITICAL(&s_state_lock);
    return current;
}

static void clear_reassembly(void)
{
    free(s_reassembly.stream);
    memset(&s_reassembly, 0, sizeof(s_reassembly));
}

static bool message_in_flight(void)
{
    bool value;
    portENTER_CRITICAL(&s_state_lock);
    value = s_message_in_flight;
    portEXIT_CRITICAL(&s_state_lock);
    return value;
}

static void set_message_in_flight(bool value, uint16_t connection_handle,
                                  uint32_t connection_epoch)
{
    portENTER_CRITICAL(&s_state_lock);
    s_message_in_flight = value;
    if (value) {
        s_in_flight_connection_handle = connection_handle;
        s_in_flight_connection_epoch = connection_epoch;
    } else {
        s_in_flight_connection_handle = BLE_CONN_NONE;
        s_in_flight_connection_epoch = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void clear_message_in_flight_if_owner(uint16_t connection_handle,
                                             uint32_t connection_epoch)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_message_in_flight &&
        s_in_flight_connection_handle == connection_handle &&
        s_in_flight_connection_epoch == connection_epoch) {
        s_message_in_flight = false;
        s_in_flight_connection_handle = BLE_CONN_NONE;
        s_in_flight_connection_epoch = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static bool has_snapshot(void)
{
    bool value;
    portENTER_CRITICAL(&s_state_lock);
    value = s_has_snapshot;
    portEXIT_CRITICAL(&s_state_lock);
    return value;
}

static void set_has_snapshot(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_has_snapshot = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static void publish_link(meter_link_state_t state)
{
    if (s_callbacks.on_link) {
        s_callbacks.on_link(state, s_callbacks.context);
    }
}

static void notify_ack(ble_ack_status_t status, uint32_t sequence,
                       uint16_t expected_connection_handle,
                       uint32_t expected_connection_epoch)
{
    uint8_t ack[BLE_ACK_SIZE] = {'A', 'K', BLE_PROTOCOL_VERSION, (uint8_t)status};
    write_u32_le(ack + 4, sequence);

    bool current_and_subscribed;
    portENTER_CRITICAL(&s_state_lock);
    current_and_subscribed =
        s_connection_handle == expected_connection_handle &&
        s_connection_epoch == expected_connection_epoch && s_ack_subscribed;
    portEXIT_CRITICAL(&s_state_lock);
    if (!current_and_subscribed || expected_connection_handle == BLE_CONN_NONE ||
        s_ack_value_handle == 0) {
        return;
    }

    struct os_mbuf *buffer = ble_hs_mbuf_from_flat(ack, sizeof(ack));
    if (!buffer) {
        ESP_LOGW(TAG, "Unable to allocate ACK notification");
        return;
    }
    int result = ble_gatts_notify_custom(expected_connection_handle, s_ack_value_handle,
                                         buffer);
    if (result != 0) {
        ESP_LOGW(TAG, "ACK notification failed: %d", result);
    }
}

static bool valid_utf8(const uint8_t *data, size_t length)
{
    size_t index = 0;
    while (index < length) {
        uint8_t first = data[index++];
        if (first <= 0x7f) {
            if (first == 0) {
                return false;
            }
            continue;
        }

        unsigned continuation_count;
        uint8_t second_min = 0x80;
        uint8_t second_max = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            if (first == 0xe0) {
                second_min = 0xa0;
            } else if (first == 0xed) {
                second_max = 0x9f;
            }
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            if (first == 0xf0) {
                second_min = 0x90;
            } else if (first == 0xf4) {
                second_max = 0x8f;
            }
        } else {
            return false;
        }

        if (index + continuation_count > length || data[index] < second_min ||
            data[index] > second_max) {
            return false;
        }
        ++index;
        for (unsigned count = 1; count < continuation_count; ++count, ++index) {
            if (data[index] < 0x80 || data[index] > 0xbf) {
                return false;
            }
        }
    }
    return true;
}

static void finish_message(ble_ack_status_t status, uint32_t sequence,
                           uint16_t connection_handle, uint32_t connection_epoch)
{
    uint8_t next[BLE_CHALLENGE_SIZE];
    esp_fill_random(next, sizeof(next));
    bool current;
    portENTER_CRITICAL(&s_state_lock);
    current = s_connection_handle == connection_handle &&
              s_connection_epoch == connection_epoch;
    if (current) {
        /* A completed message consumes exactly one challenge for this connection. */
        memcpy(s_challenge, next, sizeof(s_challenge));
    }
    portEXIT_CRITICAL(&s_state_lock);
    memset(next, 0, sizeof(next));
    clear_message_in_flight_if_owner(connection_handle, connection_epoch);

    if (!current) {
        return;
    }
    notify_ack(status, sequence, connection_handle, connection_epoch);
}

static void message_task(void *argument)
{
    (void)argument;
    ble_message_t message;
    for (;;) {
        if (xQueueReceive(s_message_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ble_ack_status_t status = BLE_ACK_BAD_FRAME;
        if (message.length >= BLE_STREAM_MIN) {
            const uint8_t *provided_mac = message.stream;
            const uint8_t *json_bytes = message.stream + BLE_MAC_SIZE;
            size_t json_length = message.length - BLE_MAC_SIZE;
            uint8_t sequence_bytes[sizeof(uint32_t)];
            uint8_t expected_mac[BLE_MAC_SIZE];
            write_u32_le(sequence_bytes, message.sequence);

            if (!hmac_parts(BLE_AUTH_DOMAIN, message.challenge, sequence_bytes,
                            json_bytes, json_length, expected_mac) ||
                !constant_time_equal(provided_mac, expected_mac, sizeof(expected_mac))) {
                status = BLE_ACK_AUTH_FAILED;
            } else if (!valid_utf8(json_bytes, json_length)) {
                status = BLE_ACK_JSON_INVALID;
            } else {
                char *json = malloc(json_length + 1);
                if (!json) {
                    status = BLE_ACK_BUSY;
                } else {
                    memcpy(json, json_bytes, json_length);
                    json[json_length] = '\0';
                    usage_snapshot_t snapshot;
                    if (!meter_usage_payload_parse(json, &snapshot)) {
                        status = BLE_ACK_JSON_INVALID;
                    } else {
                        snapshot.source = METER_SOURCE_BLE;
                        xSemaphoreTake(s_connection_mutex, portMAX_DELAY);
                        if (connection_is_current(message.connection_handle,
                                                  message.connection_epoch)) {
                            set_has_snapshot();
                            if (s_callbacks.on_snapshot) {
                                s_callbacks.on_snapshot(&snapshot,
                                                        s_callbacks.context);
                            }
                        }
                        xSemaphoreGive(s_connection_mutex);
                        status = BLE_ACK_OK;
                    }
                    memset(json, 0, json_length + 1);
                    free(json);
                }
            }
            memset(expected_mac, 0, sizeof(expected_mac));
        }

        memset(message.stream, 0, message.length);
        free(message.stream);
        finish_message(status, message.sequence, message.connection_handle,
                       message.connection_epoch);
    }
}

static void send_protocol_error(ble_ack_status_t status, uint32_t sequence,
                                uint16_t connection_handle)
{
    uint32_t epoch = 0;
    if (capture_connection(connection_handle, &epoch, NULL)) {
        notify_ack(status, sequence, connection_handle, epoch);
    }
}

static int receive_chunk(uint16_t connection_handle, struct os_mbuf *buffer)
{
    uint16_t packet_length = OS_MBUF_PKTLEN(buffer);
    uint8_t chunk[BLE_CHUNK_VALUE_MAX];
    uint16_t copied = 0;
    if (packet_length < BLE_CHUNK_HEADER_SIZE || packet_length > sizeof(chunk) ||
        ble_hs_mbuf_to_flat(buffer, chunk, sizeof(chunk), &copied) != 0 ||
        copied != packet_length) {
        clear_reassembly();
        send_protocol_error(BLE_ACK_BAD_FRAME, 0, connection_handle);
        return 0;
    }

    uint8_t flags = chunk[3];
    uint32_t sequence = read_u32_le(chunk + 4);
    uint16_t offset = read_u16_le(chunk + 8);
    uint16_t total = read_u16_le(chunk + 10);
    const uint8_t *payload = chunk + BLE_CHUNK_HEADER_SIZE;
    uint16_t payload_length = packet_length - BLE_CHUNK_HEADER_SIZE;
    bool first = (flags & BLE_FLAG_FIRST) != 0;
    bool last = (flags & BLE_FLAG_LAST) != 0;

    if (chunk[0] != 'C' || chunk[1] != 'M' || chunk[2] != BLE_PROTOCOL_VERSION ||
        (flags & ~(BLE_FLAG_FIRST | BLE_FLAG_LAST)) != 0 || payload_length == 0) {
        clear_reassembly();
        send_protocol_error(BLE_ACK_BAD_FRAME, sequence, connection_handle);
        return 0;
    }

    if (first) {
        if (message_in_flight()) {
            send_protocol_error(BLE_ACK_BUSY, sequence, connection_handle);
            return 0;
        }
        clear_reassembly();
        if (offset != 0) {
            send_protocol_error(BLE_ACK_BAD_FRAME, sequence, connection_handle);
            return 0;
        }
        if (total < BLE_STREAM_MIN || total > BLE_STREAM_MAX) {
            send_protocol_error(total > BLE_STREAM_MAX ? BLE_ACK_TOO_LARGE
                                                       : BLE_ACK_BAD_FRAME,
                                sequence, connection_handle);
            return 0;
        }
        s_reassembly.stream = malloc(total);
        if (!s_reassembly.stream) {
            send_protocol_error(BLE_ACK_BUSY, sequence, connection_handle);
            return 0;
        }
        s_reassembly.total = total;
        s_reassembly.sequence = sequence;
        s_reassembly.connection_handle = connection_handle;
        if (!capture_connection(connection_handle, &s_reassembly.connection_epoch,
                                s_reassembly.challenge)) {
            clear_reassembly();
            send_protocol_error(BLE_ACK_BAD_FRAME, sequence, connection_handle);
            return 0;
        }
    } else if (!s_reassembly.stream || sequence != s_reassembly.sequence ||
               total != s_reassembly.total || offset != s_reassembly.received ||
               connection_handle != s_reassembly.connection_handle ||
               !connection_is_current(connection_handle,
                                      s_reassembly.connection_epoch)) {
        clear_reassembly();
        send_protocol_error(BLE_ACK_BAD_FRAME, sequence, connection_handle);
        return 0;
    }

    if (!s_reassembly.stream || offset != s_reassembly.received ||
        payload_length > s_reassembly.total - s_reassembly.received) {
        clear_reassembly();
        send_protocol_error(BLE_ACK_BAD_FRAME, sequence, connection_handle);
        return 0;
    }

    memcpy(s_reassembly.stream + s_reassembly.received, payload, payload_length);
    s_reassembly.received += payload_length;
    if (last != (s_reassembly.received == s_reassembly.total)) {
        if (last || s_reassembly.received == s_reassembly.total) {
            clear_reassembly();
            send_protocol_error(BLE_ACK_BAD_FRAME, sequence, connection_handle);
        }
        return 0;
    }
    if (!last) {
        return 0;
    }

    ble_message_t message = {
        .stream = s_reassembly.stream,
        .length = s_reassembly.total,
        .sequence = s_reassembly.sequence,
        .connection_handle = s_reassembly.connection_handle,
        .connection_epoch = s_reassembly.connection_epoch,
    };
    memcpy(message.challenge, s_reassembly.challenge, sizeof(message.challenge));
    memset(&s_reassembly, 0, sizeof(s_reassembly));
    set_message_in_flight(true, message.connection_handle,
                          message.connection_epoch);
    if (xQueueSend(s_message_queue, &message, 0) != pdTRUE) {
        clear_message_in_flight_if_owner(message.connection_handle,
                                         message.connection_epoch);
        memset(message.stream, 0, message.length);
        free(message.stream);
        send_protocol_error(BLE_ACK_BUSY, sequence, connection_handle);
    }
    return 0;
}

static int read_hello(uint16_t connection_handle, struct os_mbuf *buffer)
{
    uint8_t hello[BLE_HELLO_SIZE] = {'C', 'H', BLE_PROTOCOL_VERSION, 0};
    uint32_t epoch = 0;
    if (!capture_connection(connection_handle, &epoch, hello + 4)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!hmac_parts(BLE_DEVICE_DOMAIN, hello + 4, NULL, NULL, 0, hello + 20)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int result = os_mbuf_append(buffer, hello, sizeof(hello));
    memset(hello, 0, sizeof(hello));
    return result == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t connection_handle, uint16_t attribute_handle,
                       struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)argument;
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(context->chr->uuid, &s_rx_uuid.u) == 0) {
        return receive_chunk(connection_handle, context->om);
    }
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        attribute_handle == s_ack_value_handle) {
        return read_hello(connection_handle, context->om);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        return result;
    }

    struct ble_hs_adv_fields response = {0};
    response.name = (uint8_t *)s_device_name;
    response.name_len = strlen(s_device_name);
    response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&response);
    if (result != 0) {
        return result;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(s_own_address_type, NULL, BLE_HS_FOREVER,
                               &parameters, gap_event, NULL);
    if (result == 0) {
        publish_link(METER_LINK_CONNECTING);
    }
    return result;
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE connection failed: %d", event->connect.status);
            (void)start_advertising();
            return 0;
        }
        xSemaphoreTake(s_connection_mutex, portMAX_DELAY);
        clear_reassembly();
        activate_connection(event->connect.conn_handle);
        xSemaphoreGive(s_connection_mutex);
        publish_link(has_snapshot() ? METER_LINK_STALE : METER_LINK_CONNECTING);
        ESP_LOGI(TAG, "BLE host connected");
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(s_connection_mutex, portMAX_DELAY);
        clear_reassembly();
        invalidate_connection(event->disconnect.conn.conn_handle, true);
        xSemaphoreGive(s_connection_mutex);
        publish_link(has_snapshot() ? METER_LINK_STALE : METER_LINK_OFFLINE);
        ESP_LOGI(TAG, "BLE host disconnected: %d", event->disconnect.reason);
        (void)start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ack_value_handle) {
            portENTER_CRITICAL(&s_state_lock);
            if (s_connection_handle == event->subscribe.conn_handle) {
                s_ack_subscribed = event->subscribe.cur_notify != 0;
            }
            portEXIT_CRITICAL(&s_state_lock);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        (void)start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE ATT MTU updated to %u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void host_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset: %d", reason);
    xSemaphoreTake(s_connection_mutex, portMAX_DELAY);
    clear_reassembly();
    invalidate_connection(BLE_CONN_NONE, false);
    xSemaphoreGive(s_connection_mutex);
    publish_link(METER_LINK_OFFLINE);
}

static void host_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &s_own_address_type);
    }
    if (result == 0) {
        result = start_advertising();
    }
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to start BLE advertising: %d", result);
        publish_link(METER_LINK_OFFLINE);
    } else {
        ESP_LOGI(TAG, "Advertising as %s", s_device_name);
    }
}

static void host_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t register_services(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int result = ble_svc_gap_device_name_set(s_device_name);
    if (result == 0) {
        result = ble_gatts_count_cfg(s_gatt_services);
    }
    if (result == 0) {
        result = ble_gatts_add_svcs(s_gatt_services);
    }
    return result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t meter_ble_transport_start(const meter_transport_callbacks_t *callbacks)
{
    if (!callbacks || !callbacks->on_snapshot || !callbacks->on_link) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = meter_config_load(&s_config);
    if (result != ESP_OK || s_config.pairing_token[0] == '\0') {
        ESP_LOGW(TAG, "Pairing token is not configured; BLE transport remains disabled");
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    s_callbacks = *callbacks;

    log_heap_state("BLE start");
    s_connection_mutex = xSemaphoreCreateMutex();
    if (!s_connection_mutex) {
        ESP_LOGE(TAG, "Connection mutex allocation failed");
        log_heap_state("BLE mutex failure");
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6];
    result = esp_read_mac(mac, ESP_MAC_BT);
    if (result != ESP_OK) {
        vSemaphoreDelete(s_connection_mutex);
        s_connection_mutex = NULL;
        return result;
    }
    snprintf(s_device_name, sizeof(s_device_name), "Codex Meter-%02X%02X", mac[4], mac[5]);
    rotate_challenge();

    log_heap_state("Before NimBLE init");
    result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(result));
        log_heap_state("NimBLE init failure");
        vSemaphoreDelete(s_connection_mutex);
        s_connection_mutex = NULL;
        return result;
    }
    log_heap_state("After NimBLE init");
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    result = register_services();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "GATT service registration failed: %s",
                 esp_err_to_name(result));
        log_heap_state("GATT registration failure");
        (void)nimble_port_deinit();
        vSemaphoreDelete(s_connection_mutex);
        s_connection_mutex = NULL;
        return result;
    }

    s_message_queue = xQueueCreate(1, sizeof(ble_message_t));
    if (!s_message_queue) {
        ESP_LOGE(TAG, "BLE message queue allocation failed");
        log_heap_state("BLE queue failure");
        (void)nimble_port_deinit();
        vSemaphoreDelete(s_connection_mutex);
        s_connection_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(message_task, "ble_usage", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "BLE message task allocation failed");
        log_heap_state("BLE task failure");
        vQueueDelete(s_message_queue);
        s_message_queue = NULL;
        (void)nimble_port_deinit();
        vSemaphoreDelete(s_connection_mutex);
        s_connection_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    log_heap_state("Before NimBLE host task");
    nimble_port_freertos_init(host_task);
    log_heap_state("BLE transport ready");
    return ESP_OK;
}
