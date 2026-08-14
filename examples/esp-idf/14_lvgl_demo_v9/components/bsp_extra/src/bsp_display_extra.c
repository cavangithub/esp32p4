/*
 * SPDX-FileCopyrightText: 2026 Oxeltech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display bring-up for the LCD selected in menuconfig (Example Configuration).
 */

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp_display_extra.h"

#if CONFIG_EXAMPLE_LCD_PANEL_AMS495QA04
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_ams495qa04.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "bsp_display_extra";

#if CONFIG_EXAMPLE_LCD_PANEL_AMS495QA04

#define AMS495QA04_H_RES                    (960)
#define AMS495QA04_V_RES                    (544)
#define AMS495QA04_MIPI_DSI_LANE_NUM        (2)
#define AMS495QA04_MIPI_DSI_PHY_LDO_CHAN    (3)
#define AMS495QA04_MIPI_DSI_PHY_VOLTAGE_MV  (2500)

static void ams495qa04_panel_power_enable(void)
{
    const gpio_config_t en_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << CONFIG_EXAMPLE_AMS495QA04_VDD_EN_GPIO) |
                        (1ULL << CONFIG_EXAMPLE_AMS495QA04_VCI_EN_GPIO),
    };
    ESP_ERROR_CHECK(gpio_config(&en_cfg));

    gpio_set_level(CONFIG_EXAMPLE_AMS495QA04_VDD_EN_GPIO, 0);
    gpio_set_level(CONFIG_EXAMPLE_AMS495QA04_VCI_EN_GPIO, 0);

    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_EXAMPLE_AMS495QA04_VDD_EN_GPIO, 1);
    ESP_LOGI(TAG, "VDD enabled on GPIO%d", CONFIG_EXAMPLE_AMS495QA04_VDD_EN_GPIO);

    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_EXAMPLE_AMS495QA04_VCI_EN_GPIO, 1);
    ESP_LOGI(TAG, "VCI enabled on GPIO%d", CONFIG_EXAMPLE_AMS495QA04_VCI_EN_GPIO);
}

static lv_display_t *ams495qa04_display_start(void)
{
    ESP_LOGI(TAG, "Enable AMS495QA04 power rails");
    ams495qa04_panel_power_enable();

    ESP_LOGI(TAG, "Power on MIPI DSI PHY");
    static esp_ldo_channel_handle_t ldo_mipi_phy;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = AMS495QA04_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = AMS495QA04_MIPI_DSI_PHY_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));

    ESP_LOGI(TAG, "Init MIPI DSI bus + panel IO");
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = AMS495QA04_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = AMS495QA04_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    ESP_LOGI(TAG, "Install AMS495QA04 panel");
    esp_lcd_dpi_panel_config_t dpi_config = AMS495QA04_960_544_PANEL_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    ams495qa04_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = AMS495QA04_MIPI_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_EXAMPLE_AMS495QA04_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
        .flags.reset_active_high = false,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ams495qa04_mipi(dbi_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    /* Native landscape, single DPI framebuffer, partial LVGL draw buffers.
     * Triple-buffer anti-tear needs 3 FBs; this panel bring-up uses 1. */
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = dbi_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = AMS495QA04_H_RES,
            .ver_res = AMS495QA04_V_RES,
            .buffer_height = 50,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, NULL, TAG, "register AMS495QA04 display failed");

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_LOGI(TAG, "AMS495QA04 display started (touchless, no backlight)");
    return disp;
}

#endif /* CONFIG_EXAMPLE_LCD_PANEL_AMS495QA04 */

lv_display_t *bsp_extra_display_start(void)
{
#if CONFIG_EXAMPLE_LCD_PANEL_AMS495QA04
    return ams495qa04_display_start();
#else
    /*
     * If you need to use the three-cache anti-tear configuration, you need to
     * fix idf 5.5. Refer to:
     * https://github.com/espressif/esp-iot-solution/blob/da973d162cc88736a4e05e6582393e666f221c2a/components/display/tools/esp_lvgl_adapter/README.md?plain=1#L671-L709
     */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (disp) {
        bsp_display_backlight_on();
    }
    return disp;
#endif
}
