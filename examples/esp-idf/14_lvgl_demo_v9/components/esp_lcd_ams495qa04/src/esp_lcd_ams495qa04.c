/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2026 Oxeltech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver for the Samsung AMS495QA04 4.95" qHD (960x544) AMOLED panel via
 * MIPI-DSI on ESP32-P4. Extracted from the panel bring-up example; contains
 * only the reusable panel driver (constructor + init/reset/del + Samsung MCS
 * init sequence). Board power rails, DSI PHY LDO, and the LVGL bring-up live in
 * the application.
 */

#include "esp_lcd_ams495qa04.h"

#if SOC_MIPI_DSI_SUPPORTED

#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_mipi_dsi.h"
#include "hal/mipi_dsi_host_ll.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "ams495qa04";

/* DCS standard commands used for status reads. */
#define LCD_CMD_SWRESET     0x01
#define LCD_CMD_RDDID       0x04
#define LCD_CMD_RDDST       0x09
#define LCD_CMD_RDDPM       0x0A
#define LCD_CMD_RDDMADCTL   0x0B
#define LCD_CMD_RDDCOLMOD   0x0C
#define LCD_CMD_RDDIM       0x0D
#define LCD_CMD_RDDSM       0x0E
#define LCD_CMD_RDDSDR      0x0F

/* COLMOD encoding: 0x55 = RGB565, 0x66 = RGB666, 0x77 = RGB888.
 * NOTE: the DPI/LVGL path runs RGB888 while COLMOD (0x3A) is programmed 0x55
 * below; this matches the validated bring-up on this panel. Revisit if colors
 * look wrong. */
#define COLMOD_RGB888       0x77
#define COLMOD_RGB565       0x55
#define COLMOD_RGB666       0x66

/* ---------- Internal panel state ----------------------------------------- */
typedef struct {
    esp_lcd_panel_io_handle_t io;
    int                       reset_gpio_num;
    bool                      reset_active_high;

    esp_err_t (*dpi_init)(esp_lcd_panel_t *panel);
    esp_err_t (*dpi_del)(esp_lcd_panel_t *panel);
} ams495qa04_panel_t;

/* ========================================================================
 *  Helpers: pretty-print panel status registers (diagnostics)
 * ====================================================================== */
static void decode_power_mode(uint8_t v)
{
    ESP_LOGI(TAG, "  Power Mode (0Ah) = 0x%02X  [Booster:%d  Idle:%d  Partial:%d  "
                  "SleepOUT:%d  NormalMode:%d  DisplayON:%d]",
             v, !!(v & 0x80), !!(v & 0x40), !!(v & 0x20),
             !!(v & 0x10), !!(v & 0x08), !!(v & 0x04));
}

static void decode_signal_mode(uint8_t v)
{
    ESP_LOGI(TAG, "  Signal Mode (0Eh) = 0x%02X  [TE:%d  HSYNC:%d  VSYNC:%d  PCLK:%d]",
             v, !!(v & 0x80), !!(v & 0x20), !!(v & 0x10), !!(v & 0x08));
}

static void decode_self_diag(uint8_t v)
{
    ESP_LOGI(TAG, "  Self-Diag (0Fh)   = 0x%02X  [RegLoad:%s  Functionality:%s]",
             v, (v & 0x80) ? "OK" : "FAIL", (v & 0x40) ? "OK" : "FAIL");
}

static esp_err_t read_and_log(esp_lcd_panel_io_handle_t io, uint8_t cmd,
                              size_t nbytes, const char *name)
{
    uint8_t buf[22] = {0};
    if (nbytes > sizeof(buf)) nbytes = sizeof(buf);

    esp_err_t err = esp_lcd_panel_io_rx_param(io, cmd, buf, nbytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  %-22s (cmd 0x%02X): READ FAILED (%s)",
                 name, cmd, esp_err_to_name(err));
        return err;
    }

    char hex[3 * 22 + 1] = {0};
    for (size_t i = 0; i < nbytes; ++i) {
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", buf[i]);
    }
    ESP_LOGI(TAG, "  %-22s (cmd 0x%02X): %s", name, cmd, hex);

    if (cmd == LCD_CMD_RDDPM)  decode_power_mode(buf[0]);
    if (cmd == LCD_CMD_RDDSM)  decode_signal_mode(buf[0]);
    if (cmd == LCD_CMD_RDDSDR) decode_self_diag(buf[0]);

    return ESP_OK;
}

static void dump_panel_status(esp_lcd_panel_io_handle_t io, const char *when)
{
    ESP_LOGI(TAG, "===== Panel status %s =====", when);
    read_and_log(io, LCD_CMD_RDDID,     3, "Display ID");
    read_and_log(io, LCD_CMD_RDDST,     4, "Display Status");
    read_and_log(io, LCD_CMD_RDDPM,     1, "Power Mode");
    read_and_log(io, LCD_CMD_RDDMADCTL, 1, "MADCTL");
    read_and_log(io, LCD_CMD_RDDCOLMOD, 1, "Pixel Format");
    read_and_log(io, LCD_CMD_RDDIM,     1, "Image Mode");
    read_and_log(io, LCD_CMD_RDDSM,     1, "Signal Mode");
    read_and_log(io, LCD_CMD_RDDSDR,    1, "Self-Diagnostic");
    ESP_LOGI(TAG, "==========================================");
}

/* ========================================================================
 *  Reset: pulse RESETB low then release and wait for the panel to boot.
 * ====================================================================== */
static esp_err_t panel_ams495qa04_reset(esp_lcd_panel_t *panel)
{
    ams495qa04_panel_t *p = (ams495qa04_panel_t *)panel->user_data;
    ESP_RETURN_ON_FALSE(p, ESP_ERR_INVALID_STATE, TAG, "panel context not set");

    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(p->reset_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(120));   /* >= t_S before first DCS command */
    } else if (p->io) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, LCD_CMD_SWRESET, NULL, 0),
                            TAG, "SWRESET failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

/* ========================================================================
 *  Init: Samsung datasheet 9-2-1 power-on sequence.
 *
 *  1. MCS access password (F0/F1 = 5A 5A)
 *  2. Analog power condition set (F4, F5), source amp (F8)
 *  3. Gamma register set (F9: store with P1=01h, commit with P1=00h)
 *  4. ELVSS condition set (B1, B2)
 *  5. COLMOD (3Ah)
 *  6. Start the DPI video stream, force continuous-HS clock lane
 *  7. Exit sleep (11h) -> wait -> Display ON (29h)
 * ====================================================================== */
static esp_err_t panel_ams495qa04_init(esp_lcd_panel_t *panel)
{
    ams495qa04_panel_t *p = (ams495qa04_panel_t *)panel->user_data;
    ESP_RETURN_ON_FALSE(p,     ESP_ERR_INVALID_STATE, TAG, "panel context not set");
    ESP_RETURN_ON_FALSE(p->io, ESP_ERR_INVALID_STATE, TAG, "io handle missing");

    esp_lcd_panel_io_handle_t io = p->io;

    /* 1. MCS access password unlock. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF0, (uint8_t[]){0x5A, 0x5A}, 2),
                        TAG, "F0h unlock failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF1, (uint8_t[]){0x5A, 0x5A}, 2),
                        TAG, "F1h unlock failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF7, (uint8_t[]){0x20}, 1),
                        TAG, "F7 failed");

    /* 2. Source amplifier / analog power. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF8,
                            (uint8_t[]){0x7F, 0x7A, 0x89, 0x67, 0x26, 0x38, 0x00, 0x00, 0x09,
                                        0x67, 0x70, 0x88, 0x7A, 0x76, 0x05, 0x09, 0x23, 0x23,
                                        0x23, 0x00}, 20),
                        TAG, "F8h failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF4,
                            (uint8_t[]){0x33, 0x42, 0x00, 0x08}, 4),
                        TAG, "F4h failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF5,
                            (uint8_t[]){0x00, 0x06, 0x26, 0x35, 0x03}, 5),
                        TAG, "F5h failed");

    /* 3. Gamma register set (139 cd/m^2 column). P1=0x01 stores, P1=0x00 commits. */
    static const uint8_t gamma_139[22] = {
        0x01,                 /* P1: gamma update DISABLE (store) */
        0x9F, 0x9F, 0xBE,     /* V255  R/G/B */
        0xCF, 0xD7, 0xC9,     /* V171  R/G/B */
        0xC2, 0xCB, 0xBB,     /* V87   R/G/B */
        0xE1, 0xE3, 0xDE,     /* V59   R/G/B */
        0xD6, 0xD0, 0xD3,     /* V35   R/G/B */
        0xFA, 0xED, 0xE6,     /* V15   R/G/B */
        0x2F, 0x00, 0x2F,     /* V1    R/G/B */
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF9, gamma_139, sizeof(gamma_139)),
                        TAG, "F9h gamma store failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF9, (uint8_t[]){0x00}, 1),
                        TAG, "F9h gamma commit failed");
    ESP_LOGI(TAG, "Gamma set");

    /* 4. ELVSS condition set. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB1, (uint8_t[]){0x07, 0x00, 0x00}, 3),
                        TAG, "B1h ELVSS_CON failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t[]){0x12, 0x12, 0x12, 0x12}, 4),
                        TAG, "B2h TEMP_SWIRE failed");

    /* 5. Pixel format. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]){COLMOD_RGB565}, 1),
                        TAG, "COLMOD failed");

    /* 6. Start the DPI video stream BEFORE Exit_Sleep. */
    if (p->dpi_init) {
        ESP_RETURN_ON_ERROR(p->dpi_init(panel), TAG, "MIPI DPI panel init failed");
    }
    mipi_dsi_host_ll_dpi_enable_frame_ack(MIPI_DSI_LL_GET_HOST(0), false);

    /* Force a CONTINUOUS high-speed clock lane. dpi_init leaves the clock lane
     * in AUTO (non-continuous) mode; this AMOLED needs the clock lane held in
     * HS continuously or its sampling PLL never locks and it shows garbage. LP
     * escape-mode commands on the data lanes are unaffected. */
    vTaskDelay(pdMS_TO_TICKS(100));
    mipi_dsi_host_ll_set_clock_lane_state(MIPI_DSI_LL_GET_HOST(0),
                                          MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    ESP_LOGI(TAG, "DSI clock lane forced to continuous HS");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 7. Exit sleep, wait, Display ON. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0), TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(500));   /* datasheet: >= 250 ms before DISPON */

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0), TAG, "DISPON failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "AMS495QA04 init sequence complete");
    dump_panel_status(io, "AFTER init");
    mipi_dsi_host_ll_enable_video_mode(MIPI_DSI_LL_GET_HOST(0), true);

    return ESP_OK;
}

/* ========================================================================
 *  Display on/off: DCS Set_Display_ON (0x29) / Set_Display_OFF (0x28).
 *  The base DPI panel does not implement this, so provide it here (used to
 *  blank the AMOLED before light sleep and to re-light it on wake).
 * ====================================================================== */
static esp_err_t panel_ams495qa04_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    ams495qa04_panel_t *p = (ams495qa04_panel_t *)panel->user_data;
    ESP_RETURN_ON_FALSE(p && p->io, ESP_ERR_INVALID_STATE, TAG, "io handle missing");
    mipi_dsi_host_ll_set_clock_lane_state(MIPI_DSI_LL_GET_HOST(0),
                                          MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    vTaskDelay(pdMS_TO_TICKS(50));
    return esp_lcd_panel_io_tx_param(p->io, on ? 0x29 : 0x28, NULL, 0);
}

/* ========================================================================
 *  swap_xy / mirror: the panel is native landscape and LVGL applies no
 *  rotation, so these are no-ops. They exist only so lvgl_port's rotation
 *  setup (which always calls them, even for a 0-degree config) does not log
 *  "not supported by this panel" errors. Reject any real transform request.
 * ====================================================================== */
static esp_err_t panel_ams495qa04_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    (void)panel;
    return swap_axes ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

static esp_err_t panel_ams495qa04_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    (void)panel;
    return (mirror_x || mirror_y) ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

/* ========================================================================
 *  del
 * ====================================================================== */
static esp_err_t panel_ams495qa04_del(esp_lcd_panel_t *panel)
{
    ams495qa04_panel_t *p = (ams495qa04_panel_t *)panel->user_data;
    esp_err_t ret = ESP_OK;

    if (p && p->dpi_del) {
        ret = p->dpi_del(panel);
    }
    if (p && p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ret;
}

/* ========================================================================
 *  Public constructor
 * ====================================================================== */
esp_err_t esp_lcd_new_panel_ams495qa04_mipi(const esp_lcd_panel_io_handle_t io,
                                            const esp_lcd_panel_dev_config_t *panel_dev_config,
                                            esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel,
                        ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    ams495qa04_vendor_config_t *vc =
        (ams495qa04_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vc && vc->mipi_config.dpi_config && vc->mipi_config.dsi_bus,
                        ESP_ERR_INVALID_ARG, TAG, "invalid vendor config");

    esp_err_t ret = ESP_OK;
    ams495qa04_panel_t *p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no mem");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "RST gpio_config failed");
        /* Hold the panel in reset from boot. */
        gpio_set_level(panel_dev_config->reset_gpio_num,
                       panel_dev_config->flags.reset_active_high ? 1 : 0);
    }

    p->io                = io;
    p->reset_gpio_num    = panel_dev_config->reset_gpio_num;
    p->reset_active_high = panel_dev_config->flags.reset_active_high;

    esp_lcd_panel_handle_t dpi_panel = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vc->mipi_config.dsi_bus,
                                            vc->mipi_config.dpi_config,
                                            &dpi_panel),
                      err, TAG, "create MIPI DPI panel failed");

    /* Stash the DPI init/del, then override the vtable with our sequence. */
    p->dpi_init           = dpi_panel->init;
    p->dpi_del            = dpi_panel->del;
    dpi_panel->init        = panel_ams495qa04_init;
    dpi_panel->reset       = panel_ams495qa04_reset;
    dpi_panel->del         = panel_ams495qa04_del;
    dpi_panel->disp_on_off = panel_ams495qa04_disp_on_off;
    dpi_panel->swap_xy     = panel_ams495qa04_swap_xy;   /* no-op: native landscape */
    dpi_panel->mirror      = panel_ams495qa04_mirror;    /* no-op: no rotation       */
    dpi_panel->user_data   = p;

    *ret_panel = dpi_panel;
    ESP_LOGI(TAG, "AMS495QA04 panel created @%p (ctx %p)", dpi_panel, p);
    return ESP_OK;

err:
    free(p);
    return ret;
}

#endif /* SOC_MIPI_DSI_SUPPORTED */
