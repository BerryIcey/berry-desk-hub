#include "meter_provisioning.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#define FORM_BODY_LIMIT 2048
#define SETUP_RESTART_DELAY_US 750000
#define SETUP_AP_SUBNET 0xC0A80400UL
#define SETUP_AP_NETMASK 0xFFFFFF00UL
#define SETUP_AP_ADDRESS 0xC0A80401UL
#define SETUP_KEY_PLACEHOLDER "__METER_SETUP_KEY__"

#if CONFIG_METER_TRANSPORT_BLE
#define SETUP_INTRO_TEXT \
    "Enter the pairing token shown by the host. Wi-Fi and host fields are optional in BLE-only mode."
#define SETUP_WIFI_LEGEND "Wi-Fi (optional)"
#define SETUP_HOST_LEGEND "Codex host (optional)"
#define SETUP_SSID_REQUIRED ""
#define SETUP_HOST_HINT \
    "BLE-only needs only the pairing token; these fields are kept for a future transport change."
#else
#define SETUP_INTRO_TEXT "Add Wi-Fi and the pairing details shown by the host dashboard."
#define SETUP_WIFI_LEGEND "Wi-Fi"
#define SETUP_HOST_LEGEND "Codex host"
#define SETUP_SSID_REQUIRED "required"
#define SETUP_HOST_HINT \
    "The meter uses automatic discovery first, then this address as a fallback."
#endif

static const char *TAG = "meter_setup";

static const char SETUP_PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Codex Meter Setup</title><style>"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:28px 18px;"
    "font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;"
    "color:#1d1d1f;background:#f5f5f7}"
    "main{width:min(100%,560px);margin:auto}.brand{margin:10px 4px 22px}"
    ".eyebrow{display:inline-flex;align-items:center;gap:7px;padding:7px 11px;"
    "border-radius:999px;background:#e8f5ec;color:#17743b;font-size:12px;"
    "font-weight:700;letter-spacing:.03em}.dot{width:7px;height:7px;border-radius:50%;"
    "background:#34c759;box-shadow:0 0 0 4px rgba(52,199,89,.14)}"
    "h1{margin:16px 0 7px;font-size:34px;line-height:1.08;letter-spacing:-.035em}"
    ".intro{margin:0;color:#6e6e73;font-size:15px;line-height:1.5}"
    ".card{padding:25px;border:1px solid rgba(0,0,0,.06);border-radius:24px;"
    "background:rgba(255,255,255,.92);box-shadow:0 18px 50px rgba(0,0,0,.08)}"
    "fieldset{margin:0 0 24px;padding:0;border:0}legend{width:100%;padding:0 0 12px;"
    "font-size:13px;font-weight:700;color:#6e6e73;text-transform:uppercase;"
    "letter-spacing:.06em}.field{margin:0 0 14px}label{display:block;margin:0 0 7px;"
    "font-size:14px;font-weight:650}input{width:100%;height:48px;padding:0 14px;"
    "border:1px solid #d2d2d7;border-radius:12px;background:#fff;color:#1d1d1f;"
    "font:inherit;outline:0;transition:border-color .15s,box-shadow .15s}"
    "input:focus{border-color:#0071e3;box-shadow:0 0 0 4px rgba(0,113,227,.13)}"
    ".hint{display:block;margin:6px 2px 0;color:#86868b;font-size:12px;line-height:1.4}"
    ".grid{display:grid;grid-template-columns:1fr 122px;gap:12px}"
    "button{width:100%;height:50px;border:0;border-radius:13px;background:#0071e3;"
    "color:#fff;font:inherit;font-weight:700;cursor:pointer}"
    "button:active{transform:scale(.99);background:#0065cc}"
    ".privacy{margin:15px 3px 0;color:#86868b;font-size:12px;line-height:1.45}"
    "@media(max-width:480px){h1{font-size:30px}.card{padding:21px}.grid{grid-template-columns:1fr}}"
    "</style></head><body><main><header class=\"brand\">"
    "<span class=\"eyebrow\"><span class=\"dot\"></span>Private setup connection</span>"
    "<h1>Connect your meter.</h1>"
    "<p class=\"intro\">" SETUP_INTRO_TEXT "</p>"
    "</header><form class=\"card\" action=\"/api/config\" method=\"post\" "
    "enctype=\"application/x-www-form-urlencoded\">"
    "<input type=\"hidden\" name=\"setupKey\" value=\"" SETUP_KEY_PLACEHOLDER "\">"
    "<fieldset><legend>" SETUP_WIFI_LEGEND "</legend><div class=\"field\">"
    "<label for=\"ssid\">Network name</label>"
    "<input id=\"ssid\" name=\"ssid\" maxlength=\"32\" autocomplete=\"off\" "
    SETUP_SSID_REQUIRED ">"
    "</div><div class=\"field\"><label for=\"password\">Password</label>"
    "<input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\" "
    "autocomplete=\"current-password\">"
    "<span class=\"hint\">Leave blank only for an open network.</span></div></fieldset>"
    "<fieldset><legend>" SETUP_HOST_LEGEND "</legend><div class=\"field\">"
    "<label for=\"hostId\">Host ID</label>"
    "<input id=\"hostId\" name=\"hostId\" maxlength=\"64\" autocomplete=\"off\">"
    "<span class=\"hint\">Used for automatic discovery. A manual host can be used instead.</span>"
    "</div><div class=\"field\"><label for=\"token\">Pairing token</label>"
    "<input id=\"token\" name=\"token\" type=\"password\" maxlength=\"64\" "
    "autocomplete=\"off\" required></div><div class=\"grid\"><div class=\"field\">"
    "<label for=\"manualHost\">Manual host (optional)</label>"
    "<input id=\"manualHost\" name=\"manualHost\" maxlength=\"63\" "
    "placeholder=\"192.168.1.20\" autocomplete=\"off\"></div><div class=\"field\">"
    "<label for=\"port\">Port</label><input id=\"port\" name=\"port\" type=\"number\" "
    "min=\"1\" max=\"65535\" value=\"8787\" inputmode=\"numeric\" required>"
    "</div></div><span class=\"hint\">" SETUP_HOST_HINT "</span></fieldset>"
    "<button type=\"submit\">Save and restart</button>"
    "<p class=\"privacy\">Credentials are saved on this device and are never written to "
    "diagnostic logs.</p></form></main></body></html>";

static const char SUCCESS_PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Setup complete</title><style>"
    "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;"
    "font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;"
    "color:#1d1d1f;background:#f5f5f7}.card{max-width:440px;padding:34px;"
    "border-radius:24px;background:#fff;box-shadow:0 18px 50px rgba(0,0,0,.08);"
    "text-align:center}.check{display:grid;place-items:center;width:52px;height:52px;"
    "margin:0 auto 18px;border-radius:50%;background:#e8f5ec;color:#188442;"
    "font-size:28px;font-weight:800}h1{margin:0 0 9px;font-size:28px;"
    "letter-spacing:-.03em}p{margin:0;color:#6e6e73;line-height:1.5}</style>"
    "</head><body><main class=\"card\"><div class=\"check\">&#10003;</div>"
    "<h1>Configuration saved</h1><p>The meter is restarting. You can close this page "
    "and return to your normal Wi-Fi network.</p></main></body></html>";

typedef struct {
    bool setup_key;
    bool ssid;
    bool password;
    bool host_id;
    bool token;
    bool manual_host;
    bool port;
} form_fields_t;

static httpd_handle_t s_server;
static meter_config_t s_current;
static wifi_mode_t s_previous_mode = WIFI_MODE_NULL;
static bool s_wifi_was_started;
static char s_setup_ssid[33];
static char s_setup_psk[16];
static bool s_running;
static bool s_restart_pending;
static esp_timer_handle_t s_restart_timer;

static void clear_sensitive(void *data, size_t length)
{
    volatile unsigned char *cursor = data;
    while (length-- > 0) {
        *cursor++ = 0;
    }
}

static void set_response_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
                       "base-uri 'none'; frame-ancestors 'none'");
}

static esp_err_t send_text(httpd_req_t *request, const char *status, const char *message)
{
    set_response_headers(request);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static bool request_has_setup_authority(httpd_req_t *request)
{
    char host[32];
    size_t host_length = httpd_req_get_hdr_value_len(request, "Host");
    if (host_length == 0 || host_length >= sizeof(host) ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host, "192.168.4.1") != 0 && strcmp(host, "192.168.4.1:80") != 0)) {
        return false;
    }

    size_t origin_length = httpd_req_get_hdr_value_len(request, "Origin");
    if (origin_length == 0) {
        return true;
    }

    char origin[40];
    return origin_length < sizeof(origin) &&
           httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) == ESP_OK &&
           (strcmp(origin, "http://192.168.4.1") == 0 ||
            strcmp(origin, "http://192.168.4.1:80") == 0);
}

static bool request_came_from_setup_ap(httpd_req_t *request)
{
    int socket_fd = httpd_req_to_sockfd(request);
    if (socket_fd < 0) {
        return false;
    }

    struct sockaddr_storage peer = {0};
    socklen_t peer_length = sizeof(peer);
    if (getpeername(socket_fd, (struct sockaddr *)&peer, &peer_length) != 0 ||
        peer.ss_family != AF_INET) {
        return false;
    }

    const struct sockaddr_in *peer_ipv4 = (const struct sockaddr_in *)&peer;
    uint32_t peer_address = ntohl(peer_ipv4->sin_addr.s_addr);
    if ((peer_address & SETUP_AP_NETMASK) != SETUP_AP_SUBNET) {
        return false;
    }

    struct sockaddr_storage local = {0};
    socklen_t local_length = sizeof(local);
    if (getsockname(socket_fd, (struct sockaddr *)&local, &local_length) != 0 ||
        local.ss_family != AF_INET) {
        return false;
    }
    const struct sockaddr_in *local_ipv4 = (const struct sockaddr_in *)&local;
    return ntohl(local_ipv4->sin_addr.s_addr) == SETUP_AP_ADDRESS;
}

static esp_err_t require_setup_ap_client(httpd_req_t *request)
{
    if (request_came_from_setup_ap(request) && request_has_setup_authority(request)) {
        return ESP_OK;
    }
    send_text(request, "403 Forbidden", "Configuration is available only through the setup Wi-Fi.");
    return ESP_ERR_NOT_ALLOWED;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool url_decode_in_place(char *value)
{
    char *read_cursor = value;
    char *write_cursor = value;

    while (*read_cursor) {
        unsigned char decoded = (unsigned char)*read_cursor++;
        if (decoded == '+') {
            decoded = ' ';
        } else if (decoded == '%') {
            int high = hex_value(read_cursor[0]);
            int low = read_cursor[0] ? hex_value(read_cursor[1]) : -1;
            if (high < 0 || low < 0) {
                return false;
            }
            decoded = (unsigned char)((high << 4) | low);
            read_cursor += 2;
            if (decoded == '\0') {
                return false;
            }
        }
        *write_cursor++ = (char)decoded;
    }
    *write_cursor = '\0';
    return true;
}

static bool copy_form_value(char *target, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity) {
        return false;
    }
    memcpy(target, value, length + 1);
    return true;
}

static bool constant_time_string_equal(const char *left, const char *right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    if (left_length != right_length) {
        return false;
    }

    unsigned difference = 0;
    for (size_t index = 0; index < left_length; ++index) {
        difference |= (unsigned char)left[index] ^ (unsigned char)right[index];
    }
    return difference == 0;
}

static bool parse_port(const char *value, uint16_t *port)
{
    if (!value[0]) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

static bool parse_form(char *body, meter_config_t *next)
{
    form_fields_t fields = {0};
#if CONFIG_METER_TRANSPORT_BLE
    /* Preserve optional network fields when a token-only BLE form is posted. */
    memset(next->pairing_token, 0, sizeof(next->pairing_token));
#else
    memset(next->wifi_ssid, 0, sizeof(next->wifi_ssid));
    memset(next->wifi_password, 0, sizeof(next->wifi_password));
    memset(next->host_id, 0, sizeof(next->host_id));
    memset(next->pairing_token, 0, sizeof(next->pairing_token));
    memset(next->manual_host, 0, sizeof(next->manual_host));
    next->port = 0;
#endif

    char *cursor = body;
    while (cursor && *cursor) {
        char *next_pair = strchr(cursor, '&');
        if (next_pair) {
            *next_pair++ = '\0';
        }

        char *separator = strchr(cursor, '=');
        if (!separator) {
            return false;
        }
        *separator++ = '\0';
        if (!url_decode_in_place(cursor) || !url_decode_in_place(separator)) {
            return false;
        }

        if (strcmp(cursor, "setupKey") == 0 && !fields.setup_key) {
            fields.setup_key = constant_time_string_equal(separator, s_setup_psk);
            if (!fields.setup_key) {
                return false;
            }
        } else if (strcmp(cursor, "ssid") == 0 && !fields.ssid) {
            fields.ssid = copy_form_value(next->wifi_ssid, sizeof(next->wifi_ssid), separator);
            if (!fields.ssid) {
                return false;
            }
        } else if (strcmp(cursor, "password") == 0 && !fields.password) {
            fields.password =
                copy_form_value(next->wifi_password, sizeof(next->wifi_password), separator);
            if (!fields.password) {
                return false;
            }
        } else if (strcmp(cursor, "hostId") == 0 && !fields.host_id) {
            fields.host_id = copy_form_value(next->host_id, sizeof(next->host_id), separator);
            if (!fields.host_id) {
                return false;
            }
        } else if (strcmp(cursor, "token") == 0 && !fields.token) {
            fields.token =
                copy_form_value(next->pairing_token, sizeof(next->pairing_token), separator);
            if (!fields.token) {
                return false;
            }
        } else if (strcmp(cursor, "manualHost") == 0 && !fields.manual_host) {
            fields.manual_host =
                copy_form_value(next->manual_host, sizeof(next->manual_host), separator);
            if (!fields.manual_host) {
                return false;
            }
        } else if (strcmp(cursor, "port") == 0 && !fields.port) {
            fields.port = parse_port(separator, &next->port);
            if (!fields.port) {
                return false;
            }
        } else {
            return false;
        }

        cursor = next_pair;
    }

#if CONFIG_METER_TRANSPORT_BLE
    return fields.setup_key && fields.token;
#else
    return fields.setup_key && fields.ssid && fields.password && fields.host_id && fields.token &&
           fields.manual_host && fields.port;
#endif
}

static bool is_form_content_type(httpd_req_t *request)
{
    size_t length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (length == 0 || length >= 80) {
        return false;
    }

    char content_type[80];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK) {
        return false;
    }

    static const char expected[] = "application/x-www-form-urlencoded";
    size_t expected_length = sizeof(expected) - 1;
    return strncasecmp(content_type, expected, expected_length) == 0 &&
           (content_type[expected_length] == '\0' || content_type[expected_length] == ';');
}

static void restart_timer_callback(void *argument)
{
    (void)argument;
    esp_restart();
}

static esp_err_t schedule_restart(void)
{
    if (s_restart_pending) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_timer_create_args_t timer_config = {
        .callback = restart_timer_callback,
        .name = "meter_restart",
    };
    esp_err_t result = esp_timer_create(&timer_config, &s_restart_timer);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_timer_start_once(s_restart_timer, SETUP_RESTART_DELAY_US);
    if (result != ESP_OK) {
        esp_timer_delete(s_restart_timer);
        s_restart_timer = NULL;
        return result;
    }

    s_restart_pending = true;
    return ESP_OK;
}

static esp_err_t setup_page_handler(httpd_req_t *request)
{
    if (require_setup_ap_client(request) != ESP_OK) {
        return ESP_OK;
    }

    set_response_headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    const char *placeholder = strstr(SETUP_PAGE, SETUP_KEY_PLACEHOLDER);
    if (!placeholder) {
        return httpd_resp_send_500(request);
    }
    esp_err_t result =
        httpd_resp_send_chunk(request, SETUP_PAGE, placeholder - SETUP_PAGE);
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, s_setup_psk, HTTPD_RESP_USE_STRLEN);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(
            request, placeholder + strlen(SETUP_KEY_PLACEHOLDER),
            HTTPD_RESP_USE_STRLEN);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, NULL, 0);
    }
    return result;
}

static esp_err_t config_post_handler(httpd_req_t *request)
{
    if (require_setup_ap_client(request) != ESP_OK) {
        return ESP_OK;
    }
    if (s_restart_pending) {
        return send_text(request, "409 Conflict", "Configuration was already saved.");
    }
    if (!is_form_content_type(request)) {
        return send_text(request, "415 Unsupported Media Type",
                         "Expected application/x-www-form-urlencoded.");
    }
    if (request->content_len == 0) {
        return send_text(request, "411 Length Required", "The request body is required.");
    }
    if (request->content_len > FORM_BODY_LIMIT) {
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "Form is too large.");
    }

    char *body = calloc(1, request->content_len + 1);
    if (!body) {
        return httpd_resp_send_500(request);
    }

    size_t offset = 0;
    while (offset < request->content_len) {
        int received =
            httpd_req_recv(request, body + offset, request->content_len - offset);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            clear_sensitive(body, request->content_len + 1);
            free(body);
            return httpd_resp_send_408(request);
        }
        if (received <= 0) {
            clear_sensitive(body, request->content_len + 1);
            free(body);
            return ESP_FAIL;
        }
        offset += (size_t)received;
    }
    body[offset] = '\0';

    meter_config_t next = s_current;
    bool parsed = parse_form(body, &next);
    clear_sensitive(body, request->content_len + 1);
    free(body);
    if (!parsed || meter_config_validate(&next) != ESP_OK || !meter_config_is_complete(&next)) {
        clear_sensitive(&next, sizeof(next));
        return send_text(request, "422 Unprocessable Content",
                         "Check every field and try again.");
    }

    esp_err_t result = meter_config_save(&next);
    clear_sensitive(&next, sizeof(next));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to save configuration: %s", esp_err_to_name(result));
        return httpd_resp_send_500(request);
    }

    result = schedule_restart();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to schedule restart: %s", esp_err_to_name(result));
        return send_text(request, "500 Internal Server Error",
                         "Configuration was saved. Restart the meter manually.");
    }

    set_response_headers(request);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, SUCCESS_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = 80;
    server_config.max_uri_handlers = 4;
    server_config.stack_size = 6144;
    server_config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t result = httpd_start(&server, &server_config);
    if (result != ESP_OK) {
        return result;
    }

    const httpd_uri_t setup_page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setup_page_handler,
    };
    result = httpd_register_uri_handler(server, &setup_page);
    if (result != ESP_OK) {
        httpd_stop(server);
        return result;
    }

    const httpd_uri_t config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    result = httpd_register_uri_handler(server, &config_post);
    if (result != ESP_OK) {
        httpd_stop(server);
        return result;
    }

    s_server = server;
    return ESP_OK;
}

static esp_err_t configure_setup_ap(bool apsta)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif && !esp_netif_create_default_wifi_ap()) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6];
    esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (result != ESP_OK) {
        return result;
    }
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), "Codex-Meter-%02X%02X%02X", mac[3], mac[4],
             mac[5]);

    wifi_config_t ap_config = {0};
    size_t setup_ssid_length = strlen(s_setup_ssid);
    size_t setup_psk_length = strlen(s_setup_psk);
    if (setup_ssid_length == 0 || setup_ssid_length > sizeof(ap_config.ap.ssid) ||
        setup_psk_length >= sizeof(ap_config.ap.password)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(ap_config.ap.ssid, s_setup_ssid, setup_ssid_length);
    ap_config.ap.ssid_len = (uint8_t)setup_ssid_length;
    memcpy(ap_config.ap.password, s_setup_psk, setup_psk_length + 1);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = true;

    result = esp_wifi_get_mode(&s_previous_mode);
    if (result != ESP_OK) {
        return result;
    }

    int8_t current_tx_power = 0;
    result = esp_wifi_get_max_tx_power(&current_tx_power);
    if (result == ESP_OK) {
        s_wifi_was_started = true;
    } else if (result == ESP_ERR_WIFI_NOT_STARTED) {
        s_wifi_was_started = false;
    } else {
        return result;
    }

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_set_mode(apsta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (result != ESP_OK) {
        esp_wifi_set_mode(s_previous_mode);
        return result;
    }
    if (!s_wifi_was_started) {
        result = esp_wifi_start();
        if (result != ESP_OK) {
            esp_wifi_set_mode(s_previous_mode);
            return result;
        }
    }
    return ESP_OK;
}

static esp_err_t restore_wifi_mode(void)
{
    esp_err_t first_error = ESP_OK;
    if (!s_wifi_was_started) {
        first_error = esp_wifi_stop();
        if (first_error == ESP_ERR_WIFI_NOT_STARTED) {
            first_error = ESP_OK;
        }
    }

    esp_err_t result = esp_wifi_set_mode(s_previous_mode);
    if (first_error == ESP_OK && result != ESP_OK) {
        first_error = result;
    }
    return first_error;
}

esp_err_t meter_provisioning_start(const meter_config_t *current, bool apsta,
                                   meter_provisioning_ready_cb_t on_ready, void *context)
{
    if (!current) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        if (on_ready) {
            on_ready(s_setup_ssid, s_setup_psk, context);
        }
        return ESP_OK;
    }

    size_t psk_length = strnlen(current->setup_psk, sizeof(current->setup_psk));
    if (psk_length < 8 || psk_length >= sizeof(current->setup_psk)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_current = *current;
    memcpy(s_setup_psk, current->setup_psk, psk_length + 1);

    esp_err_t result = configure_setup_ap(apsta);
    if (result != ESP_OK) {
        memset(&s_current, 0, sizeof(s_current));
        memset(s_setup_psk, 0, sizeof(s_setup_psk));
        return result;
    }

    result = start_http_server();
    if (result != ESP_OK) {
        restore_wifi_mode();
        memset(&s_current, 0, sizeof(s_current));
        memset(s_setup_psk, 0, sizeof(s_setup_psk));
        return result;
    }

    s_running = true;
    if (on_ready) {
        on_ready(s_setup_ssid, s_setup_psk, context);
    }
    ESP_LOGI(TAG, "Setup portal started on the private SoftAP");
    return ESP_OK;
}

esp_err_t meter_provisioning_stop(void)
{
    if (s_restart_pending) {
        return ESP_OK;
    }
    if (!s_running && !s_server) {
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;
    if (s_server) {
        esp_err_t result = httpd_stop(s_server);
        if (result != ESP_OK) {
            first_error = result;
        }
        s_server = NULL;
    }

    if (s_restart_timer) {
        esp_timer_stop(s_restart_timer);
        esp_err_t result = esp_timer_delete(s_restart_timer);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
        s_restart_timer = NULL;
        s_restart_pending = false;
    }

    esp_err_t result = restore_wifi_mode();
    if (first_error == ESP_OK && result != ESP_OK) {
        first_error = result;
    }

    s_running = false;
    s_previous_mode = WIFI_MODE_NULL;
    s_wifi_was_started = false;
    memset(&s_current, 0, sizeof(s_current));
    memset(s_setup_ssid, 0, sizeof(s_setup_ssid));
    memset(s_setup_psk, 0, sizeof(s_setup_psk));
    return first_error;
}

bool meter_provisioning_is_running(void)
{
    return s_running;
}
