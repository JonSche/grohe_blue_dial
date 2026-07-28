/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from the VIEWE UEDX24240013-MD50E-SDK-en ESP-IDF reference
 * (components/bsp/lcd_panel_gc9a01.c) -- the manufacturer's own bespoke
 * esp_lcd_panel_t implementation, replacing the espressif registry
 * esp_lcd_gc9a01 driver per docs/display_diff.md / docs/ARCHITECTURE.md.
 *
 * The only deliberate deviation from the original: panel_gc9a01_init() reads
 * its command table from panel_dev_config->vendor_config (our existing
 * gc9a01_vendor::kGc9a01VendorConfig, already verified byte-identical to
 * this file's own vendor_specific_init[] -- see docs/display_diff.md)
 * instead of a second hardcoded copy of the same 55 commands. The
 * ESP_IDF_VERSION < 5.0.0 branches are dropped since this project targets
 * idf: ">=5.3" unconditionally.
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_gc9a01.h"  // gc9a01_vendor_config_t / gc9a01_lcd_init_cmd_t (type defs only -- esp_lcd_new_panel_gc9a01() itself is never called)
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"  // TODO(debug): esp_timer_get_time() for draw_bitmap timestamps

#include "lcd_panel_gc9a01.h"

static const char *TAG = "lcd_panel.gc9a01";

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    unsigned int bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_cal; // save surrent value of LCD_CMD_COLMOD register
    const gc9a01_lcd_init_cmd_t *init_cmds; // external vendor table, or NULL for the built-in default below
    uint16_t init_cmds_size;
} gc9a01_panel_t;

esp_err_t lcd_new_panel_gc9a01(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    gc9a01_panel_t *gc9a01 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    gc9a01 = calloc(1, sizeof(gc9a01_panel_t));
    ESP_GOTO_ON_FALSE(gc9a01, ESP_ERR_NO_MEM, err, TAG, "no mem for gc9a01 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        gc9a01->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        gc9a01->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        gc9a01->colmod_cal = 0x55;
        break;
    case 18:
        gc9a01->colmod_cal = 0x66;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    if (panel_dev_config->vendor_config) {
        const gc9a01_vendor_config_t *vendor_config = (const gc9a01_vendor_config_t *)panel_dev_config->vendor_config;
        gc9a01->init_cmds = vendor_config->init_cmds;
        gc9a01->init_cmds_size = vendor_config->init_cmds_size;
    }

    gc9a01->io = io;
    gc9a01->bits_per_pixel = panel_dev_config->bits_per_pixel;
    gc9a01->reset_gpio_num = panel_dev_config->reset_gpio_num;
    gc9a01->reset_level = panel_dev_config->flags.reset_active_high;
    gc9a01->base.del = panel_gc9a01_del;
    gc9a01->base.reset = panel_gc9a01_reset;
    gc9a01->base.init = panel_gc9a01_init;
    gc9a01->base.draw_bitmap = panel_gc9a01_draw_bitmap;
    gc9a01->base.invert_color = panel_gc9a01_invert_color;
    gc9a01->base.set_gap = panel_gc9a01_set_gap;
    gc9a01->base.mirror = panel_gc9a01_mirror;
    gc9a01->base.swap_xy = panel_gc9a01_swap_xy;
    gc9a01->base.disp_on_off = panel_gc9a01_disp_on_off;
    *ret_panel = &(gc9a01->base);
    ESP_LOGD(TAG, "new gc9a01 panel @%p", gc9a01);

    return ESP_OK;

err:
    if (gc9a01) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(gc9a01);
    }
    return ret;
}

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    if (gc9a01->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9a01->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del gc9a01 panel @%p", gc9a01);
    free(gc9a01);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    // TODO(debug): remove -- traces every reset() call and which path it takes.
    ESP_LOGW(TAG, "reset() ENTER t=%lld us gpio=%d level=%d",
             (long long)esp_timer_get_time(), gc9a01->reset_gpio_num, gc9a01->reset_level);

    // perform hardware reset
    if (gc9a01->reset_gpio_num >= 0) {
        gpio_set_level(gc9a01->reset_gpio_num, gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9a01->reset_gpio_num, !gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5m before sending new command
    }

    // TODO(debug): remove -- see ENTER log above.
    ESP_LOGW(TAG, "reset() EXIT t=%lld us", (long long)esp_timer_get_time());

    return ESP_OK;
}

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes; // Length of data in above data array; 0xFF = end of cmds.
} lcd_init_cmd_t;

// Built-in default, used only if lcd_new_panel_gc9a01() is called with
// panel_dev_config->vendor_config == NULL. In this project every call site
// passes gc9a01_vendor::kGc9a01VendorConfig instead, so this table is dead
// code in practice -- kept for structural fidelity with the manufacturer's
// original driver and as a defensive default, matching how the registry
// esp_lcd_gc9a01 driver keeps its own built-in default alongside the
// override mechanism.
static const lcd_init_cmd_t vendor_specific_init[] = {
    // Enable Inter Register
    {0xfe, {0x00}, 0},
    {0xef, {0x00}, 0},
    {0xeb, {0x14}, 1},
    {0x84, {0x60}, 1},
    {0x85, {0xff}, 1},
    {0x86, {0xff}, 1},
    {0x87, {0xff}, 1},
    {0x8e, {0xff}, 1},
    {0x8f, {0xff}, 1},
    {0x88, {0x0a}, 1},
    {0x89, {0x21}, 1},
    {0x8a, {0x00}, 1},
    {0x8b, {0x80}, 1},
    {0x8c, {0x01}, 1},
    {0x8d, {0x03}, 1},
    {0xB5, {0x08, 0x09, 0x14, 0x08}, 4},
    {0xB6, {0x00, 0x00}, 2},
    {0x36, {0x48}, 1},
    {0x3a, {0x05}, 1},
    {0x90, {0x08, 0x08, 0x08, 0x08}, 4},
    {0xbd, {0x06}, 1},
    {0xba, {0x01}, 1},
    {0xbc, {0x00}, 1},
    {0xff, {0x60, 0x01, 0x04}, 3},
    {0xc3, {0x13}, 1},
    {0xc4, {0x13}, 1},
    {0xc9, {0x25}, 1},
    {0xbe, {0x11}, 1},
    {0xe1, {0x10, 0x0e}, 2},
    {0xdf, {0x21, 0x0c, 0x02}, 3},
    {0xf0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2a}, 6},
    {0xf1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6f}, 6},
    {0xf2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2a}, 6},
    {0xf3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6f}, 6},
    {0xed, {0x1b, 0x0b}, 2},
    {0xae, {0x77}, 1},
    {0xcd, {0x63}, 1},
    {0x70, {0x07, 0x07, 0x04, 0x0e, 0x0f, 0x09, 0x07, 0x08, 0x03}, 9},
    {0xe8, {0x04}, 1},
    {0x62, {0x18, 0x0d, 0x71, 0xed, 0x70, 0x70, 0x18, 0x0f, 0x71, 0xef, 0x70, 0x70}, 12},
    {0x63, {0x18, 0x11, 0x71, 0xf1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xf3, 0x70, 0x70}, 12},
    {0x64, {0x28, 0x29, 0xf1, 0x01, 0xf1, 0x00, 0x07}, 7},
    {0x66, {0x3c, 0x00, 0xcd, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10},
    {0x67, {0x00, 0x3c, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10},
    {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4e, 0x00}, 7},
    {0x98, {0x3e, 0x07}, 2},
    {0x99, {0x3e, 0x07}, 2},
    {0x35, {0x00}, 1},
    {0x44, {0x00, 0x4a}, 2},
    {0x21, {0x00}, 0},
    {0x2a, {0x00, 0x00, 0x00, 0xef}, 4},
    {0x2b, {0x00, 0x00, 0x00, 0xef}, 4},
    {0x2c, {0x00}, 0},
    {0x11, {0x00}, 0},
    {0x29, {0x00}, 0},
    {0, {0}, 0xff},
};

static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    if (gc9a01->init_cmds) {
        for (uint16_t i = 0; i < gc9a01->init_cmds_size; i++) {
            // The vendor table may write MADCTL (LCD_CMD_MADCTL) directly
            // over SPI. Keep madctl_val in sync with that so a later
            // mirror()/swap_xy() call (e.g. from esp_lvgl_port's rotation
            // setup) recomputes from the panel's real current MADCTL
            // instead of the constructor's stale default, and doesn't
            // silently overwrite bits (e.g. BGR) the vendor table set.
            if (gc9a01->init_cmds[i].cmd == LCD_CMD_MADCTL && gc9a01->init_cmds[i].data_bytes >= 1) {
                gc9a01->madctl_val = ((const uint8_t *)gc9a01->init_cmds[i].data)[0];
            }
            // TODO(debug): remove -- confirms the real MADCTL value the
            // panel receives at init, and that the shadow above now matches
            // it.
            if (gc9a01->init_cmds[i].cmd == 0x36 && gc9a01->init_cmds[i].data_bytes >= 1) {
                ESP_LOGW(TAG, "vendor table sends MADCTL=0x%02x directly (madctl_val shadow now=0x%02x)",
                         ((const uint8_t *)gc9a01->init_cmds[i].data)[0], gc9a01->madctl_val);
            }
            esp_lcd_panel_io_tx_param(io, gc9a01->init_cmds[i].cmd, gc9a01->init_cmds[i].data, gc9a01->init_cmds[i].data_bytes);
            if (gc9a01->init_cmds[i].delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(gc9a01->init_cmds[i].delay_ms));
            }
        }
    } else {
        int cmd = 0;
        while (vendor_specific_init[cmd].data_bytes != 0xff) {
            esp_lcd_panel_io_tx_param(io, vendor_specific_init[cmd].cmd, vendor_specific_init[cmd].data, vendor_specific_init[cmd].data_bytes & 0x1F);
            cmd++;
        }
    }

    return ESP_OK;
}

static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    // TODO(debug): remove -- logs every draw_bitmap call with an explicit
    // microsecond timestamp (esp_timer_get_time(), monotonic since boot) to
    // check whether the once-per-second black flash coincides with a redraw.
    // Capped only as a runaway-log safety net, not to hide anything relevant.
    static int debug_call_count = 0;
    if (debug_call_count < 500) {
        debug_call_count++;
        ESP_LOGI(TAG, "draw_bitmap #%d: t=%lld us x[%d,%d) y[%d,%d) data=%p",
                 debug_call_count, (long long)esp_timer_get_time(), x_start, x_end, y_start, y_end, color_data);
    }

    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    // TODO(debug): remove -- dumps the first 16 RGB565 pixels and the
    // gap values for draw_bitmap #1 only, before the gap adjustment below.
    if (debug_call_count == 1) {
        const uint16_t *px = (const uint16_t *)color_data;
        ESP_LOGW(TAG, "draw_bitmap #1 raw args: x[%d,%d) y[%d,%d) x_gap=%d y_gap=%d bits_per_pixel=%u",
                 x_start, x_end, y_start, y_end, gc9a01->x_gap, gc9a01->y_gap, gc9a01->bits_per_pixel);
        ESP_LOGW(TAG, "draw_bitmap #1 first 16 px: %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x",
                 px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7],
                 px[8], px[9], px[10], px[11], px[12], px[13], px[14], px[15]);
    }

    x_start += gc9a01->x_gap;
    x_end += gc9a01->x_gap;
    y_start += gc9a01->y_gap;
    y_end += gc9a01->y_gap;

    // define an area of frame memory where MCU can access
    const uint8_t caset[4] = {
        (uint8_t)((x_start >> 8) & 0xFF),
        (uint8_t)(x_start & 0xFF),
        (uint8_t)(((x_end - 1) >> 8) & 0xFF),
        (uint8_t)((x_end - 1) & 0xFF),
    };
    const uint8_t raset[4] = {
        (uint8_t)((y_start >> 8) & 0xFF),
        (uint8_t)(y_start & 0xFF),
        (uint8_t)(((y_end - 1) >> 8) & 0xFF),
        (uint8_t)((y_end - 1) & 0xFF),
    };
    // TODO(debug): remove -- the exact address window bytes sent to the
    // panel for draw_bitmap #1, after gap adjustment.
    if (debug_call_count == 1) {
        ESP_LOGW(TAG, "draw_bitmap #1 CASET=%02x %02x %02x %02x  RASET=%02x %02x %02x %02x",
                 caset[0], caset[1], caset[2], caset[3], raset[0], raset[1], raset[2], raset[3]);
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, caset, 4);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, raset, 4);
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * gc9a01->bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);

    // TODO(debug): remove -- "flush complete" from this driver's point of
    // view: esp_lcd_panel_io_tx_color() has returned, i.e. the transfer is
    // queued (DMA may still be physically transmitting; actual completion
    // is signaled later via the io's on_color_trans_done callback, which
    // esp_lvgl_port registers internally and which this driver has no
    // visibility into).
    if (debug_call_count <= 500) {
        ESP_LOGI(TAG, "draw_bitmap #%d flush-queued t=%lld us len=%u",
                 debug_call_count, (long long)esp_timer_get_time(), (unsigned)len);
    }

    return ESP_OK;
}

static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    // TODO(debug): remove -- traces every invert_color() call.
    ESP_LOGW(TAG, "invert_color(%d) t=%lld us -> cmd=0x%02x", invert_color_data, (long long)esp_timer_get_time(), command);
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}

// TODO(experiment): both functions below are temporarily forced into no-ops
// -- neither touches madctl_val nor sends a MADCTL write -- so the vendor
// table's MADCTL=0x48 is never modified after init, no matter what
// esp_lvgl_port's automatic rotation setup calls them with. Isolated test
// only; not a permanent fix.
#define GC9A01_EXPERIMENT_PIN_MADCTL 1

static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
#if GC9A01_EXPERIMENT_PIN_MADCTL
    ESP_LOGW(TAG, "mirror(x=%d,y=%d) t=%lld us called -> IGNORED, MADCTL left at 0x%02x",
             mirror_x, mirror_y, (long long)esp_timer_get_time(), gc9a01->madctl_val);
    return ESP_OK;
#endif
    if (mirror_x) {
        gc9a01->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        gc9a01->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    // TODO(debug): remove -- confirms whether/when this gets called (e.g. by
    // esp_lvgl_port's rotation setup) and what MADCTL value it computes from
    // its own shadow state, which was never told about the vendor table's
    // own 0x36 write during init.
    ESP_LOGW(TAG, "mirror(x=%d,y=%d) t=%lld us -> MADCTL=0x%02x", mirror_x, mirror_y, (long long)esp_timer_get_time(), gc9a01->madctl_val);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9a01->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
#if GC9A01_EXPERIMENT_PIN_MADCTL
    ESP_LOGW(TAG, "swap_xy(%d) t=%lld us called -> IGNORED, MADCTL left at 0x%02x",
             swap_axes, (long long)esp_timer_get_time(), gc9a01->madctl_val);
    return ESP_OK;
#endif
    if (swap_axes) {
        gc9a01->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    // TODO(debug): remove -- see panel_gc9a01_mirror() above.
    ESP_LOGW(TAG, "swap_xy(%d) t=%lld us -> MADCTL=0x%02x", swap_axes, (long long)esp_timer_get_time(), gc9a01->madctl_val);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9a01->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    gc9a01->x_gap = x_gap;
    gc9a01->y_gap = y_gap;
    // TODO(debug): remove -- traces every set_gap() call.
    ESP_LOGW(TAG, "set_gap(x=%d,y=%d)", x_gap, y_gap);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
    int command = 0;
    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    // TODO(debug): remove -- traces every disp_on_off() call; DISPOFF here
    // would blank the panel outright.
    ESP_LOGW(TAG, "disp_on_off(%d) t=%lld us -> cmd=0x%02x", on_off, (long long)esp_timer_get_time(), command);
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}
