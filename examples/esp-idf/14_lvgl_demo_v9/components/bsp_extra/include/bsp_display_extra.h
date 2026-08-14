/*
 * SPDX-FileCopyrightText: 2026 Oxeltech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the LCD selected in menuconfig and bring up LVGL.
 *
 * Waveshare DSI panels use the BSP path (including backlight and GT911 touch).
 * AMS495QA04 uses the local MIPI-DSI driver: VDD/VCI power rails, no backlight,
 * no touch.
 *
 * @return Pointer to LVGL display, or NULL on failure
 */
lv_display_t *bsp_extra_display_start(void);

#ifdef __cplusplus
}
#endif
