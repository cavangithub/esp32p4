/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2026 Oxeltech
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file
 * @brief ESP LCD driver for the Samsung AMS495QA04 4.95" qHD (960x544) AMOLED
 *        panel over MIPI-DSI on ESP32-P4.
 *
 * The panel is a MIPI command/video AMOLED driven by a Samsung MCS
 * (Manufacturer Command Set) init sequence. The driver wraps the standard
 * ESP-IDF DPI panel and overrides init/reset/del so the vendor sequence runs
 * and the DSI clock lane is forced to continuous high-speed (this AMOLED needs
 * it or its sampling PLL never locks).
 */

#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD panel vendor configuration.
 *
 * @note  This structure must be passed to `esp_lcd_panel_dev_config_t::vendor_config`.
 */
typedef struct {
    struct {
        esp_lcd_dsi_bus_handle_t          dsi_bus;     /*!< MIPI-DSI bus handle */
        const esp_lcd_dpi_panel_config_t *dpi_config;  /*!< MIPI-DPI panel configuration */
        uint8_t                           lane_num;    /*!< Number of MIPI-DSI data lanes */
    } mipi_config;
} ams495qa04_vendor_config_t;

/**
 * @brief Create LCD panel for the Samsung AMS495QA04 AMOLED.
 *
 * @param[in]  io                LCD panel IO handle (MIPI DBI control channel)
 * @param[in]  panel_dev_config  General panel device configuration. The
 *                               `vendor_config` field must point at an
 *                               ams495qa04_vendor_config_t.
 * @param[out] ret_panel         Returned LCD panel handle
 * @return
 *      - ESP_ERR_INVALID_ARG   if a parameter is invalid
 *      - ESP_OK                on success
 *      - Otherwise             on failure
 */
esp_err_t esp_lcd_new_panel_ams495qa04_mipi(const esp_lcd_panel_io_handle_t io,
                                            const esp_lcd_panel_dev_config_t *panel_dev_config,
                                            esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief MIPI-DSI bus configuration (2 data lanes, 500 Mbps/lane).
 */
// Don't need to specify .phy_clk_src in v5.5.x, now the default is MIPI_DSI_PHY_CLK_SRC_PLL_F240M
// .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT
#define AMS495QA04_PANEL_BUS_DSI_2CH_CONFIG()   \
    {                                           \
        .bus_id             = 0,                \
        .num_data_lanes     = 2,                \
        .lane_bit_rate_mbps = 500,              \
    }

/**
 * @brief MIPI-DBI panel IO configuration (8-bit command/parameter).
 */
#define AMS495QA04_PANEL_IO_DBI_CONFIG()    \
    {                                       \
        .virtual_channel = 0,               \
        .lcd_cmd_bits    = 8,               \
        .lcd_param_bits  = 8,               \
    }

/**
 * @brief MIPI-DPI panel configuration for the 960x544 AMOLED.
 *
 * Working timing values from the panel bring-up: 20 MHz DPI clock off the
 * 240 MHz PLL, small H/V shifts to center the active area. The clock/timing
 * are panel-specific and were tuned on hardware; do not treat them as generic.
 *
 * @param[in] px_format Pixel format (use LCD_COLOR_PIXEL_FORMAT_RGB888).
 */
#define AMS495QA04_960_544_PANEL_DPI_CONFIG(px_format)          \
    {                                                           \
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_PLL_F240M,   \
        .dpi_clock_freq_mhz = 20,                               \
        .virtual_channel    = 0,                                \
        .pixel_format       = px_format,                        \
        .num_fbs            = 1,                                \
        .video_timing = {                                       \
            .h_size            = 960,                           \
            .v_size            = 544,                           \
            .hsync_pulse_width = 2,                             \
            .hsync_back_porch  = 16,   /* 10 base + 6 H shift */\
            .hsync_front_porch = 10,                            \
            .vsync_pulse_width = 2,                             \
            .vsync_back_porch  = 10,   /* 10 base + 0 V shift */\
            .vsync_front_porch = 10,                            \
        },                                                      \
        .flags.use_dma2d    = true,                             \
    }

#ifdef __cplusplus
}
#endif

#endif /* SOC_MIPI_DSI_SUPPORTED */
