/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from the VIEWE UEDX24240013-MD50E-SDK-en ESP-IDF reference
 * (components/bsp/lcd_panel_gc9a01.h) -- see docs/display_diff.md and
 * docs/ARCHITECTURE.md for why this replaces the espressif registry
 * esp_lcd_gc9a01 driver. Private to components/display; not part of its
 * public include/display/ API.
 */
#pragma once

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create LCD panel for model gc9a01
 *
 * @param[in] io LCD panel IO handle
 * @param[in] panel_dev_config general panel device configuration. If
 *            panel_dev_config->vendor_config is non-NULL, it must point to a
 *            gc9a01_vendor_config_t (see esp_lcd_gc9a01.h) whose init_cmds
 *            table is used instead of this driver's own built-in default.
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_ERR_NO_MEM        if out of memory
 *          - ESP_OK                on success
 */
esp_err_t lcd_new_panel_gc9a01(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
