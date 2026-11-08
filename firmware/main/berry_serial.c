#include "berry_serial.h"
#include "berry_ui.h"
#include "berry_power.h"
#include "berry_pickup.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
extern volatile int berry_rotation_status;
#include "berry_wifi.h"
#include "berry_panel.h"
#include "berry_pairing.h"
#include "berry_ble_provision.h"
#include "berry_network_store.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <time.h>
extern volatile int berry_freshness_delta,berry_freshness_stage;
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
extern volatile unsigned berry_boot_presses;
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern volatile unsigned berry_diag_assoc,berry_diag_disconnect,berry_diag_ip,berry_diag_scan;
#define FRAME_SIZE 8192
#define SNAPSHOT_MAX_AGE_MS (10U * 60U * 1000U)
static EXT_RAM_BSS_ATTR char snapshot[FRAME_SIZE];
static EXT_RAM_BSS_ATTR char input_line[FRAME_SIZE];
static TickType_t received_at;
static bool ever_received;
static SemaphoreHandle_t snapshot_lock;
static bool handshake_ready;

static uint32_t checksum(const unsigned char *data, size_t len) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static void reply(const char *message) {
    usb_serial_jtag_write_bytes(message, strlen(message), pdMS_TO_TICKS(1000));
}


#include "sdmmc_cmd.h"
#include <dirent.h>
static void sd_info(void) {
    /* Never delegate to a BSP configured to format a user's card. */
#if CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
    reply("SDINFO format_config_rejected\n");
#else
    esp_err_t rc=bsp_sdcard?ESP_OK:bsp_sdcard_mount();
    int entries=-1;
    if(rc==ESP_OK) {
        DIR *dir=opendir(BSP_SD_MOUNT_POINT);
        if(dir) {
            entries=0;
            while(entries<512 && readdir(dir))entries++;
            closedir(dir);
        } else rc=ESP_FAIL;
    }
    char answer[100];
    snprintf(answer,sizeof(answer),"SDINFO %d %d %llu\n",(int)rc,entries,
        (unsigned long long)(bsp_sdcard?(uint64_t)bsp_sdcard->csd.capacity*bsp_sdcard->csd.sector_size:0));
    reply(answer);
#endif
}
static uint32_t sd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void sd_probe(void) {
    if(bsp_sdcard){reply("SDPROBE already_mounted\n");return;}
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk=BSP_SD_CLK; slot.cmd=BSP_SD_CMD; slot.d0=BSP_SD_D0;
    slot.width=1; slot.flags=SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card=calloc(1,sizeof(*card));
    uint8_t *sector=heap_caps_malloc(512,MALLOC_CAP_DMA);
    esp_err_t rc=ESP_ERR_NO_MEM;
    bool started=false, read_ok=false;
    uint64_t bytes=0; const char *fs="unknown";
    if(card && sector) {
        rc=sdmmc_host_init(); started=rc==ESP_OK;
        if(rc==ESP_OK)rc=sdmmc_host_init_slot(host.slot,&slot);
        if(rc==ESP_OK)rc=sdmmc_card_init(&host,card);
        if(rc==ESP_OK) {
            bytes=(uint64_t)card->csd.capacity*card->csd.sector_size;
            if(card->csd.sector_size!=512)rc=ESP_ERR_NOT_SUPPORTED;
            else rc=sdmmc_read_sectors(card,sector,0,1);
            if(rc==ESP_OK && sector[510]==0x55 && sector[511]==0xaa) {
                /* Only read sectors: no FAT mount, formatting or metadata writes. */
                if(memcmp(sector+82,"FAT32   ",8)!=0 && memcmp(sector+54,"FAT",3)!=0 && memcmp(sector+3,"EXFAT   ",8)!=0) {
                    uint32_t start=sd_le32(sector+454);
                    if(start>0 && start<card->csd.capacity)rc=sdmmc_read_sectors(card,sector,start,1);
                }
                if(rc==ESP_OK) {
                    read_ok=true;
                    if(memcmp(sector+82,"FAT32   ",8)==0)fs="FAT32";
                    else if(memcmp(sector+54,"FAT16   ",8)==0)fs="FAT16";
                    else if(memcmp(sector+3,"EXFAT   ",8)==0)fs="exFAT";
                }
            }
        }
    }
    if(started)sdmmc_host_deinit();
    free(card); free(sector);
    char answer[160];
    snprintf(answer,sizeof(answer),"SDPROBE %d %llu %d %s\n",(int)rc,(unsigned long long)bytes,read_ok,fs);
    reply(answer);
}

bool berry_snapshot_publish(const char *body) {
    if (!body || !snapshot_lock || strlen(body) >= sizeof(snapshot)) return false;
    cJSON *parsed = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *status = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "status") : NULL;
    bool valid = cJSON_IsString(status) && status->valuestring &&
        (strcmp(status->valuestring, "fresh") == 0 || strcmp(status->valuestring, "stale") == 0 || strcmp(status->valuestring, "unknown") == 0);
    if (valid && xSemaphoreTake(snapshot_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memcpy(snapshot, body, strlen(body) + 1);
        received_at = xTaskGetTickCount();ever_received=true;
        xSemaphoreGive(snapshot_lock);
    } else valid = false;
    cJSON_Delete(parsed);
    return valid;
}

#include "berry_render_probe.inc"
static void process_line(char *line) {
    const size_t length = strlen(line);
    if(strcmp(line,"RENDERRESET")==0||strcmp(line,"RENDERDIAG")==0){render_probe_command(strcmp(line,"RENDERRESET")==0);return;}
    if(strcmp(line,"PETDIAG")==0){
        extern void berry_ui_motion_diagnostics(unsigned *,unsigned *,unsigned *,unsigned *,int *);
        unsigned p,scene,expression,frames;int position;char answer[128];
        if(esp_lv_adapter_lock(1000)==ESP_OK){berry_ui_motion_diagnostics(&p,&scene,&expression,&frames,&position);esp_lv_adapter_unlock();
            snprintf(answer,sizeof(answer),"PETDIAG %u %u %u %u %d %u\n",p,scene,expression,frames,position,(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));reply(answer);
        }else reply("PETDIAG busy\n");return;
    }
    if(strcmp(line,"PICKUPDIAG")==0){
        int status,x,y,z,read_status;unsigned samples,wakes,flags,polls,stamp;char answer[160];
        berry_pickup_diagnostics(&status,&samples,&wakes,&flags,&x,&y,&z,&polls,&read_status,&stamp);
        snprintf(answer,sizeof(answer),"PICKUPDIAG %d %u %u %u %d %d %d %d %u %d %u\n",status,samples,wakes,flags,x,y,z,0,polls,read_status,stamp);reply(answer);return;
    }
    if(strcmp(line,"NOTICEDIAG")==0){
        extern void berry_ui_notice_diagnostics(unsigned *,unsigned *,unsigned *);
        unsigned events,shown,pending;char answer[90];
        if(esp_lv_adapter_lock(1000)==ESP_OK){berry_ui_notice_diagnostics(&events,&shown,&pending);esp_lv_adapter_unlock();
            snprintf(answer,sizeof(answer),"NOTICEDIAG %u %u %u\n",events,shown,pending);reply(answer);
        }else reply("NOTICEDIAG busy\n");return;
    }
    if(strcmp(line,"SDPROBE")==0){sd_probe();return;}
    if(strcmp(line,"SDINFO")==0){sd_info();return;}
    if (strcmp(line,"DIAG")==0) {
        unsigned ticks,updates,page;int quota,cpu;char response[96];
        berry_ui_diagnostics(&ticks,&updates,&page,&quota,&cpu);
        snprintf(response,sizeof(response),"DIAG %u %u %u %d %d\n",ticks,updates,page,quota,cpu);
        reply(response);return;
    }
    if(strcmp(line,"RADIODIAG")==0){char response[128];snprintf(response,sizeof(response),"RADIODIAG %u %u %u %u %u %u\n",berry_diag_assoc,berry_diag_disconnect,berry_diag_ip,berry_diag_scan,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));reply(response);return;}
    if(strcmp(line,"KEYTEST")==0){bool ok=false;if(esp_lv_adapter_lock(1000)==ESP_OK){ok=berry_ui_test_keyboard();esp_lv_adapter_unlock();}reply(ok?"KEYTEST OK\n":"KEYTEST ERROR\n");return;}
    if(strcmp(line,"WIFICLICK")==0){bool ok=false;if(esp_lv_adapter_lock(1000)==ESP_OK){ok=berry_ui_test_wifi_click();esp_lv_adapter_unlock();}reply(ok?"WIFICLICK OK\n":"WIFICLICK ERROR\n");return;}
    if(strcmp(line,"BOOTDIAG")==0){char answer[64];snprintf(answer,sizeof(answer),"BOOTDIAG %d %u\n",gpio_get_level(GPIO_NUM_0),berry_boot_presses);reply(answer);return;}
    if(strcmp(line,"BTNDIAG")==0){char answer[64];snprintf(answer,sizeof(answer),"BTNDIAG %d %d\n",berry_rotation_status,berry_ui_panel_state());reply(answer);return;}
    if(strcmp(line,"BTNNEXT")==0 || strcmp(line,"BTNPREV")==0 || strcmp(line,"BTNPANEL")==0){berry_button_action_t a=strcmp(line,"BTNNEXT")==0?BERRY_BUTTON_NEXT:strcmp(line,"BTNPREV")==0?BERRY_BUTTON_PREVIOUS:BERRY_BUTTON_PANEL;if(esp_lv_adapter_lock(1000)==ESP_OK){berry_ui_button_action(a);esp_lv_adapter_unlock();reply("BTNSIM OK\n");}else reply("BTNSIM ERROR\n");return;}
    if(strcmp(line,"POWERDIAG")==0){char answer[100];snprintf(answer,sizeof(answer),"POWERDIAG %d %u %d %d\n",berry_power_start_status,berry_power_event_count,berry_power_screen_off(),bsp_display_brightness_get());reply(answer);return;}
    if(strcmp(line,"POWERTEST")==0){esp_err_t rc=berry_power_toggle();reply(rc==ESP_OK?"POWERTEST OK\n":"POWERTEST ERROR\n");return;}
    if(strcmp(line,"CLOCKDIAG")==0){char answer[96];snprintf(answer,sizeof(answer),"CLOCKDIAG %lld %d %d\n",(long long)time(NULL),berry_freshness_delta,berry_freshness_stage);reply(answer);return;}
    if(strcmp(line,"PANELSCAN")==0){berry_panel_scan();reply("PANELSCAN OK\n");return;}
    if(strcmp(line,"PANELDIAG")==0){berry_panel_state_t panel;berry_panel_snapshot(&panel);char answer[100];snprintf(answer,sizeof(answer),"PANELDIAG %u %d %d %d %d %d\n",panel.count,panel.state,panel.result,panel.battery,panel.charging,panel.remaining_minutes);reply(answer);return;}
    if(strcmp(line,"PROVDIAG")==0){char response[64];snprintf(response,sizeof(response),"PROVDIAG %d %d\n",(int)esp_reset_reason(),(int)berry_prov_checkpoint);reply(response);return;}
    if(strcmp(line,"NETDIAG")==0){
        berry_network_list_t list={0};wifi_ap_record_t ap={0};int index=-1;
        esp_err_t rc=berry_network_store_load(&list);
        if(rc==ESP_OK && esp_wifi_sta_get_ap_info(&ap)==ESP_OK){
            for(size_t i=0;i<list.count;i++)if(strcmp(list.entries[i].ssid,(const char *)ap.ssid)==0){index=(int)i;break;}
        }
        char response[64];snprintf(response,sizeof(response),"NETDIAG %d %u %d\n",(int)rc,(unsigned)list.count,index);
        memset(&list,0,sizeof(list));reply(response);return;
    }
    if (strcmp(line,"PAIRTEST")==0) {berry_pairing_request();reply("PAIRTEST OK\n");return;}
    if (strcmp(line,"PAIRCANCEL")==0) {berry_pairing_cancel();reply("PAIRCANCEL OK\n");return;}
    if (strcmp(line,"WIFIDIAG")==0) {
        unsigned connected,clock_ready,ok,bad;char response[160];
        berry_wifi_diagnostics(&connected,&clock_ready,&ok,&bad);
        snprintf(response,sizeof(response),"WIFIDIAG %u %u %u %u %d %u %u %d %d %d %d %d %d\n",connected,clock_ready,ok,bad,(int)berry_wifi_start_status,berry_wifi_phase,berry_wifi_disconnect_reason,berry_http_error,berry_http_status,berry_http_stage,berry_tls_code,berry_tls_error,berry_tls_flags);
        reply(response);return;
    }
    if (length == 38 && strncmp(line, "HELLO ", 6) == 0) {
        for (size_t i = 6; i < length; ++i)
            if (!isdigit((unsigned char)line[i]) && !(line[i] >= 'a' && line[i] <= 'f')) return;
        char response[48];
        snprintf(response, sizeof(response), "BERRY-HUB/1 %.32s\n", line + 6);
        handshake_ready = true;
        reply(response);
        return;
    }
    if (handshake_ready && strcmp(line,"PAIRINFO")==0) {
        char name[32]={0},proof[17]={0},response[70]={0};handshake_ready=false;
        if(berry_pairing_view(name,sizeof(name),proof,sizeof(proof)) && name[0] && proof[0]){
            snprintf(response,sizeof(response),"PAIRINFO %s %s\n",name,proof);reply(response);
        }else reply("PAIRINFO WAIT\n");
        memset(proof,0,sizeof(proof));memset(response,0,sizeof(response));return;
    }
    if (handshake_ready && strncmp(line,"WIFICFG ",8)==0) {
        handshake_ready=false;
        bool ok=berry_wifi_configure(line+8);
        memset(line+8,0,length-8);
        reply(ok?"WIFICFG OK\n":"WIFICFG ERROR\n");
        if(ok){vTaskDelay(pdMS_TO_TICKS(300));esp_restart();}
        return;
    }
    if (length < 15 || strncmp(line, "BH1 ", 4) != 0 || line[12] != ' ' || !handshake_ready) return;
    handshake_ready = false;
    for (size_t i = 4; i < 12; ++i)
        if (!isdigit((unsigned char)line[i]) && !(line[i] >= 'a' && line[i] <= 'f')) return;
    char digest[9];
    memcpy(digest, line + 4, 8);
    digest[8] = '\0';
    const char *body = line + 13;
    if (checksum((const unsigned char *)body, strlen(body)) != strtoul(digest, NULL, 16)) return;
    if (berry_snapshot_publish(body)) {
        char response[16];
        snprintf(response, sizeof(response), "ACK %s\n", digest);
        reply(response);
    }
}

static void receiver(void *unused) {
    (void)unused;
    char *line=input_line;
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        unsigned char ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) <= 0) continue;
        if (ch == '\n') {
            if (!overflow) { line[used] = '\0'; process_line(line); }
            used = 0; overflow = false;
        } else if (!overflow) {
            if (used + 1 < FRAME_SIZE) line[used++] = (char)ch;
            else overflow = true;
        }
    }
}

bool berry_serial_get(char *destination, size_t capacity) {
    if (!destination || !capacity || !snapshot_lock) return false;
    if (xSemaphoreTake(snapshot_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool available = snapshot[0] && (xTaskGetTickCount() - received_at) <= pdMS_TO_TICKS(SNAPSHOT_MAX_AGE_MS) && strlen(snapshot) < capacity;
    if (available) memcpy(destination, snapshot, strlen(snapshot) + 1);
    xSemaphoreGive(snapshot_lock);
    return available;
}

esp_err_t berry_serial_start(void) {
    snapshot_lock = xSemaphoreCreateMutex();
    if (!snapshot_lock) return ESP_ERR_NO_MEM;
    usb_serial_jtag_driver_config_t config = { .tx_buffer_size = 8192, .rx_buffer_size = 8192 };
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK) return result;
    return xTaskCreate(receiver, "berry_usb", 5120, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

unsigned berry_serial_last_age_seconds(void){if(!snapshot_lock || xSemaphoreTake(snapshot_lock,pdMS_TO_TICKS(100))!=pdTRUE)return UINT32_MAX;unsigned result=ever_received?(unsigned)((xTaskGetTickCount()-received_at)/configTICK_RATE_HZ):UINT32_MAX;xSemaphoreGive(snapshot_lock);return result;}
