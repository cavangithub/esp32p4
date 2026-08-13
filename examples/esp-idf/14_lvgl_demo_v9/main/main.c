#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "time_service.h"
#include "ui_image.h"
#include "album_ctrl.h"
#include "msc_example.h"

static const char *TAG = "main";

extern int wifi_connected;

void init_wifi(void);

void app_main(void)
{
    init_wifi();
    if (wifi_connected) {
        time_service_init();
        time_service_wait_sync(30000);
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
    lv_obj_t *page = photo_page_create();
    lv_screen_load(page);
    album_ctrl_init();
    bsp_display_unlock();

    ESP_ERROR_CHECK(msc_start());

    /* Main owns UI updates: wait for USB events from msc_task */
    while (true) {
        msc_event_t evt;
        if (!msc_wait_event(&evt, portMAX_DELAY)) {
            continue;
        }

        ESP_LOGI(TAG, "MSC event %d, jpg_count=%u", (int)evt.id, (unsigned)evt.jpg_count);

        bsp_display_lock(-1);
        if (evt.id == MSC_EVENT_CONNECTED) {
            album_ctrl_on_usb_ready();
        } else if (evt.id == MSC_EVENT_DISCONNECTED) {
            album_ctrl_on_usb_removed();
        }
        bsp_display_unlock();
    }
}
