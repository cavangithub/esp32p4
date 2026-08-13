#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "lv_demos.h"
#include "time_service.h"
#include "ui_clock.h"

//-------------------------------------------------------------//
extern int wifi_connected;

//-------------------------------------------------------------//
void demo_widgets(void);
void init_wifi(void);
void msc_loop(void);


void app_main(void)
{
    init_wifi();
    if (wifi_connected) {
        time_service_init();
        time_service_wait_sync(
            30000
        );
    }

/*
If you need to use the three-cache anti-tear configuration, you need to fix idf 5.5. Refer to: https://github.com/espressif/esp-iot-solution/blob/da973d162cc88736a4e05e6582393e666f221c2a/components/display/tools/esp_lvgl_adapter/README.md?plain=1#L671-L709 
*/
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 0
        }};
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    ui_clock_create(lv_screen_active());
    bsp_display_unlock();

    // lv_demo_music();
    // lv_demo_benchmark();
    // lv_demo_widgets();
    // demo_widgets();
    msc_loop();
}

void demo_widgets(void)
{
    lv_obj_t *label;

    label = lv_label_create(
        lv_screen_active()
    );

    lv_label_set_text(
        label,
        "Hello ESP32-P4"
    );

    lv_obj_center(label);
}