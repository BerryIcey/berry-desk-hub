#include "berry_power.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_lv_adapter.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define PMU_ADDR 0x34
#define PMU_IRQ_ENABLE2 0x41
#define PMU_IRQ_STATUS2 0x49
#define PMU_SHORT_KEY (1U<<3)
static i2c_master_dev_handle_t pmu;
static volatile bool off;
static int saved_brightness=80;
static volatile uint32_t user_epoch;
static uint32_t brief_woke_at;
uint32_t berry_power_user_epoch(void){return user_epoch;}
void berry_power_user_activity_locked(void){brief_woke_at=0;user_epoch++;}
volatile int berry_power_start_status=-1;
volatile unsigned berry_power_event_count;
static esp_err_t read_reg(uint8_t reg,uint8_t *value){return i2c_master_transmit_receive(pmu,&reg,1,value,1,100);}
static esp_err_t write_reg(uint8_t reg,uint8_t value){uint8_t msg[2]={reg,value};return i2c_master_transmit(pmu,msg,2,100);}
bool berry_power_screen_off(void){return off;}
esp_err_t berry_power_set_screen_off_locked(bool value){
 if(off==value)return ESP_OK;
 esp_err_t rc=bsp_display_brightness_set(value?0:saved_brightness);
 if(rc==ESP_OK){off=value;if(off)brief_woke_at=0;if(!off)lv_display_trigger_activity(lv_display_get_default());}
 return rc;
}
esp_err_t berry_power_brief_wake_locked(void){
 if(!off)return ESP_ERR_INVALID_STATE;
 esp_err_t rc=berry_power_set_screen_off_locked(false);
 if(rc==ESP_OK)brief_woke_at=lv_tick_get();
 return rc;
}
void berry_power_brief_tick_locked(void){
 if(!brief_woke_at||off)return;
 uint32_t elapsed=lv_tick_get()-brief_woke_at;
 uint32_t inactive=lv_display_get_inactive_time(lv_display_get_default());
 if(inactive+250<elapsed){brief_woke_at=0;return;}
 if(elapsed>=8000)(void)berry_power_set_screen_off_locked(true);
}
esp_err_t berry_power_toggle(void){
 if(esp_lv_adapter_lock(1000)!=ESP_OK)return ESP_ERR_TIMEOUT;
 esp_err_t rc=berry_power_set_screen_off_locked(!off);
 if(rc==ESP_OK)berry_power_user_activity_locked();
 esp_lv_adapter_unlock();return rc;
}
int berry_power_brightness_locked(void) {
 return saved_brightness;
}
esp_err_t berry_power_set_brightness_locked(int level) {
 if(off || level<5 || level>100)return ESP_ERR_INVALID_ARG;
 esp_err_t rc=bsp_display_brightness_set(level);
 if(rc==ESP_OK)saved_brightness=level;
 return rc;
}
void berry_power_restore_brightness_locked(void) {
 nvs_handle_t handle;
 uint8_t level=80;
 if(nvs_open("berry_ui",NVS_READONLY,&handle)==ESP_OK){
  uint8_t saved;
  if(nvs_get_u8(handle,"brightness",&saved)==ESP_OK && saved>=5 && saved<=100)level=saved;
  nvs_close(handle);
 }
 (void)berry_power_set_brightness_locked(level);
}
void berry_power_save_brightness_locked(void) {
 nvs_handle_t handle;
 if(nvs_open("berry_ui",NVS_READWRITE,&handle)!=ESP_OK)return;
 uint8_t previous;
 if(nvs_get_u8(handle,"brightness",&previous)!=ESP_OK || previous!=saved_brightness){
  if(nvs_set_u8(handle,"brightness",(uint8_t)saved_brightness)==ESP_OK)(void)nvs_commit(handle);
 }
 nvs_close(handle);
}
static void power_task(void *unused){(void)unused;for(;;){uint8_t status=0;if(read_reg(PMU_IRQ_STATUS2,&status)==ESP_OK && (status&PMU_SHORT_KEY)){
 if(write_reg(PMU_IRQ_STATUS2,PMU_SHORT_KEY)==ESP_OK){berry_power_event_count++;(void)berry_power_toggle();}
 }vTaskDelay(pdMS_TO_TICKS(100));}}
esp_err_t berry_power_start(void){
 esp_err_t rc=bsp_i2c_init();if(rc!=ESP_OK)return rc;i2c_master_bus_handle_t bus=bsp_i2c_get_handle();if(!bus)return ESP_ERR_INVALID_STATE;
 i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=PMU_ADDR,.scl_speed_hz=CONFIG_BSP_I2C_CLK_SPEED_HZ};
 rc=i2c_master_bus_add_device(bus,&cfg,&pmu);if(rc!=ESP_OK)return rc;
 uint8_t id=0,enabled=0;rc=read_reg(0x03,&id);if(rc==ESP_OK && id!=0x4a)rc=ESP_ERR_NOT_FOUND;
 if(rc==ESP_OK)rc=read_reg(PMU_IRQ_ENABLE2,&enabled);
 if(rc==ESP_OK)rc=write_reg(PMU_IRQ_STATUS2,PMU_SHORT_KEY);
 bool added=!(enabled&PMU_SHORT_KEY);if(rc==ESP_OK && added)rc=write_reg(PMU_IRQ_ENABLE2,enabled|PMU_SHORT_KEY);
 uint8_t verify=0;if(rc==ESP_OK)rc=read_reg(PMU_IRQ_ENABLE2,&verify);if(rc==ESP_OK && !(verify&PMU_SHORT_KEY))rc=ESP_FAIL;
 if(rc==ESP_OK && xTaskCreate(power_task,"berry_power",3072,NULL,4,NULL)!=pdPASS)rc=ESP_ERR_NO_MEM;
 if(rc!=ESP_OK){if(added && read_reg(PMU_IRQ_ENABLE2,&verify)==ESP_OK)(void)write_reg(PMU_IRQ_ENABLE2,verify&~PMU_SHORT_KEY);(void)i2c_master_bus_rm_device(pmu);pmu=NULL;}return rc;
}
