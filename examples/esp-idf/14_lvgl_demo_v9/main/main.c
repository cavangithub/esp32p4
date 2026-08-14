#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp_display_extra.h"
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

    /* LCD panel is selected in menuconfig: Example Configuration → LCD panel */
    if (!bsp_extra_display_start()) {
        ESP_LOGE(TAG, "display start failed");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

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
