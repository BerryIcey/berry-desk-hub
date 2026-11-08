#include "berry_pickup.h"
#include "berry_pickup_policy.h"
#include "berry_power.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "qmi8658.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static qmi8658_dev_t sensor;
static int start_status=ESP_ERR_INVALID_STATE;
static unsigned samples,flags,wakes;
static int readings[3];
static unsigned polls,raw_stamp;
static uint32_t stationary_sample_at,stationary_epoch;
static int last_read;
static portMUX_TYPE diagnostic_lock=portMUX_INITIALIZER_UNLOCKED;

static void pickup_task(void *unused) {
    (void)unused;
    berry_pickup_policy_t policy={0};
    uint32_t epoch=berry_power_user_epoch();
    uint32_t last_sample=UINT32_MAX;
    for(;;) {
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        uint32_t current_epoch=berry_power_user_epoch();
        if(epoch!=current_epoch){berry_pickup_reset(&policy);epoch=current_epoch;}
        /* Timestamp, two temperature bytes, then the three little-endian axes. */
        uint8_t raw[11]={0};
        esp_err_t rc=qmi8658_read_register(&sensor,QMI8658_TIMESTAMP_L,raw,sizeof(raw));
        uint32_t sample=(uint32_t)raw[0]|(uint32_t)raw[1]<<8|(uint32_t)raw[2]<<16;
        float scale=1000.f/sensor.accel_lsb_div;
        float x=(int16_t)(raw[5]|raw[6]<<8)*scale,y=(int16_t)(raw[7]|raw[8]<<8)*scale,z=(int16_t)(raw[9]|raw[10]<<8)*scale;
        float magnitude=x*x+y*y+z*z;
        taskENTER_CRITICAL(&diagnostic_lock);
        polls++;last_read=rc;raw_stamp=sample;
        if(rc==ESP_OK){readings[0]=(int)x;readings[1]=(int)y;readings[2]=(int)z;}
        taskEXIT_CRITICAL(&diagnostic_lock);
        if(rc==ESP_OK&&sample!=last_sample&&magnitude>=10000&&magnitude<=15210000) {
            last_sample=sample;
            bool wake=berry_pickup_sample(&policy,x,y,z,now,berry_power_screen_off());
            taskENTER_CRITICAL(&diagnostic_lock);
            samples++;flags=(policy.armed?1:0)|(policy.candidate?4:0);
            stationary_sample_at=now;stationary_epoch=epoch;
            taskEXIT_CRITICAL(&diagnostic_lock);
            if(wake&&esp_lv_adapter_lock(100)==ESP_OK) {
                if(epoch==berry_power_user_epoch()&&berry_power_brief_wake_locked()==ESP_OK) {
                    taskENTER_CRITICAL(&diagnostic_lock);wakes++;taskEXIT_CRITICAL(&diagnostic_lock);
                }
                esp_lv_adapter_unlock();
            }
        } else if(rc!=ESP_OK||now-policy.last>200) {
            policy.sampled=false;policy.armed=false;policy.candidate=false;
            taskENTER_CRITICAL(&diagnostic_lock);flags=0;taskEXIT_CRITICAL(&diagnostic_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool berry_pickup_stationary(void) {
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    taskENTER_CRITICAL(&diagnostic_lock);
    bool stationary=(flags&1) && stationary_epoch==berry_power_user_epoch() &&
        now-stationary_sample_at<=200;
    taskEXIT_CRITICAL(&diagnostic_lock);
    return stationary;
}

esp_err_t berry_pickup_start(void) {
    esp_err_t rc=bsp_i2c_init();
    if(rc==ESP_OK)rc=qmi8658_init(&sensor,bsp_i2c_get_handle(),QMI8658_ADDRESS_HIGH);
    /* The board vendor resets the IMU separately; an ESP restart leaves its rail on. */
    if(rc==ESP_OK)rc=qmi8658_write_register(&sensor,0x60,0xb0);
    if(rc==ESP_OK) {
        uint8_t reset_status=0;
        for(unsigned retry=0;retry<12&&reset_status!=0x80;retry++) {
            vTaskDelay(pdMS_TO_TICKS(20));
            rc=qmi8658_read_register(&sensor,0x4d,&reset_status,1);
        }
        if(rc==ESP_OK&&reset_status!=0x80)rc=ESP_ERR_INVALID_RESPONSE;
    }
    if(rc==ESP_OK)rc=qmi8658_enable_sensors(&sensor,QMI8658_DISABLE_ALL);
    if(rc==ESP_OK)rc=qmi8658_write_register(&sensor,QMI8658_CTRL1,0x40);
    if(rc==ESP_OK)rc=qmi8658_write_register(&sensor,QMI8658_CTRL8,0x80);
    if(rc==ESP_OK)rc=qmi8658_set_accel_range(&sensor,QMI8658_ACCEL_RANGE_4G);
    if(rc==ESP_OK)rc=qmi8658_set_accel_odr(&sensor,QMI8658_ACCEL_ODR_1000HZ);
    if(rc==ESP_OK)rc=qmi8658_write_register(&sensor,QMI8658_CTRL5,0x01);
    if(rc==ESP_OK)rc=qmi8658_enable_sensors(&sensor,QMI8658_ENABLE_ACCEL);
    if(rc==ESP_OK&&xTaskCreate(pickup_task,"berry_pickup",3072,NULL,3,NULL)!=pdPASS)rc=ESP_ERR_NO_MEM;
    start_status=rc;return rc;
}

void berry_pickup_diagnostics(int *status,unsigned *count,unsigned *wake_count,unsigned *state,int *x,int *y,int *z,unsigned *poll_count,int *read_status,unsigned *stamp) {
    taskENTER_CRITICAL(&diagnostic_lock);
    *status=start_status;*count=samples;*wake_count=wakes;*state=flags;
    *x=readings[0];*y=readings[1];*z=readings[2];
    *poll_count=polls;*read_status=last_read;*stamp=raw_stamp;
    taskEXIT_CRITICAL(&diagnostic_lock);
}
