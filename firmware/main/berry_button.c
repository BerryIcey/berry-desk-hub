#include "berry_button.h"
#include "berry_power.h"
#include "berry_button_policy.h"
#include "berry_ui.h"
#include "esp_lv_adapter.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
volatile unsigned berry_boot_presses=0;
static void task(void *arg){
 (void)arg;berry_button_policy_t policy={0};int sampled=gpio_get_level(GPIO_NUM_18),stable=sampled;int boot_sampled=gpio_get_level(GPIO_NUM_0),boot_stable=boot_sampled;berry_boot_button_t boot={0};uint32_t boot_changed=(uint32_t)(esp_timer_get_time()/1000);uint32_t changed=(uint32_t)(esp_timer_get_time()/1000);
 for(;;){uint32_t now=(uint32_t)(esp_timer_get_time()/1000);int level=gpio_get_level(GPIO_NUM_18);if(level!=sampled){sampled=level;changed=now;}if(stable!=sampled && (uint32_t)(now-changed)>=40)stable=sampled;
  int boot_level=gpio_get_level(GPIO_NUM_0);if(boot_level!=boot_sampled){boot_sampled=boot_level;boot_changed=now;}if(boot_stable!=boot_sampled && (uint32_t)(now-boot_changed)>=40)boot_stable=boot_sampled;
  berry_button_action_t previous=berry_boot_button_update(&boot,boot_stable==0,now);if(previous==BERRY_BUTTON_PREVIOUS){berry_boot_presses++;if(esp_lv_adapter_lock(1000)==ESP_OK){berry_ui_button_action(previous);esp_lv_adapter_unlock();}}
  berry_button_action_t action=berry_button_update(&policy,stable==0,now);if(action!=BERRY_BUTTON_NONE && esp_lv_adapter_lock(1000)==ESP_OK){berry_ui_button_action(action);esp_lv_adapter_unlock();}vTaskDelay(pdMS_TO_TICKS(20));}
}
esp_err_t berry_button_start(void){gpio_config_t cfg={.pin_bit_mask=(1ULL<<18)|(1ULL<<0),.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};esp_err_t rc=gpio_config(&cfg);if(rc!=ESP_OK)return rc;return xTaskCreate(task,"berry_button",3072,NULL,4,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;}
