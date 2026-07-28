#include "display/gc9a01_display.hpp"

#include "board/board_config.hpp"
#include "esp_lcd_gc9a01.h"
#include "gc9a01_vendor/gc9a01_vendor_init.hpp"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"  // TODO(debug): esp_timer_get_time() for SetBacklight() timestamps
#include "driver/gpio.h"
#include "lcd_panel_gc9a01.h"

namespace display {
namespace {
constexpr char kTag[] = "gc9a01_display";
}  // namespace

Gc9a01Display::~Gc9a01Display() {
  if (lv_display_ != nullptr) {
    lvgl_port_remove_disp(lv_display_);
  }
  if (panel_ != nullptr) {
    esp_lcd_panel_del(panel_);
  }
  if (panel_io_ != nullptr) {
    esp_lcd_panel_io_del(panel_io_);
  }
  if (spi_bus_initialized_) {
    spi_bus_free(board::kLcdSpiHost);
  }
}

esp_err_t Gc9a01Display::Init() {
  const gpio_config_t backlight_cfg = {
      .pin_bit_mask = 1ULL << board::kLcdPinBacklight,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&backlight_cfg));
  SetBacklight(false);

  ESP_LOGI(kTag, "Initializing SPI bus");
  const spi_bus_config_t bus_cfg = GC9A01_PANEL_BUS_SPI_CONFIG(
      board::kLcdPinSclk, board::kLcdPinMosi,
      board::kLcdHorizontalResolution * board::kLcdVerticalResolution *
          static_cast<int>(sizeof(uint16_t)));
  ESP_ERROR_CHECK(
      spi_bus_initialize(board::kLcdSpiHost, &bus_cfg, SPI_DMA_CH_AUTO));
  spi_bus_initialized_ = true;

  ESP_LOGI(kTag, "Installing panel IO");
  esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(
      board::kLcdPinCs, board::kLcdPinDc, nullptr, nullptr);
  io_cfg.pclk_hz = board::kLcdPixelClockHz;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      static_cast<esp_lcd_spi_bus_handle_t>(board::kLcdSpiHost), &io_cfg,
      &panel_io_));

  ESP_LOGI(kTag, "Installing GC9A01 panel driver");
  esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = board::kLcdPinReset,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  panel_cfg.vendor_config = const_cast<gc9a01_vendor_config_t*>(
      &gc9a01_vendor::kGc9a01VendorConfig);
  ESP_ERROR_CHECK(lcd_new_panel_gc9a01(panel_io_, &panel_cfg, &panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

  ESP_LOGI(kTag, "Starting esp_lvgl_port");
  const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

  const lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = panel_io_,
      .panel_handle = panel_,
      .control_handle = nullptr,
      // Full-frame double buffer + full_refresh, matching the vendor
      // project's lvgl_port.c (buf_size = LCD_H_RES*LCD_V_RES, full_refresh=1
      // via avoid_tear) -- esp_lvgl_port asserts buffer_size == hres*vres
      // whenever flags.full_refresh is set. This is the last stable
      // configuration (Grohe Dial visible, no boot loop); restored as-is.
      .buffer_size = static_cast<uint32_t>(board::kLcdHorizontalResolution) *
                      board::kLcdVerticalResolution,
      .double_buffer = true,
      .trans_size = 0,
      .hres = static_cast<uint32_t>(board::kLcdHorizontalResolution),
      .vres = static_cast<uint32_t>(board::kLcdVerticalResolution),
      .monochrome = false,
      .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
      .rounder_cb = nullptr,
      // Render natively pre-swapped (matches the vendor project's
      // CONFIG_LV_COLOR_16_SWAP=y, which bakes the swap into every LVGL
      // color at the type level in their LVGL v8 build) instead of
      // rendering LV_COLOR_FORMAT_RGB565 and applying a separate
      // lv_draw_sw_rgb565_swap() pass over every flush buffer via
      // flags.swap_bytes. LVGL v9 has no global "16-bit swap" build
      // option -- RGB565_SWAPPED is the per-display equivalent.
      .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED,
      .flags =
          {
              .buff_dma = true,
              .buff_spiram = false,
              .sw_rotate = false,
              .swap_bytes = false,
              .full_refresh = true,
              .direct_mode = false,
          },
  };
  lv_display_ = lvgl_port_add_disp(&disp_cfg);
  if (lv_display_ == nullptr) {
    ESP_LOGE(kTag, "lvgl_port_add_disp failed");
    return ESP_FAIL;
  }
  // TODO(debug): remove -- confirms lvgl_port_add_disp actually succeeded
  // and returned a non-null lv_display_t*.
  ESP_LOGI(kTag, "lvgl_port_add_disp succeeded, lv_display_=%p",
           (void*)lv_display_);

  SetBacklight(true);
  return ESP_OK;
}

void Gc9a01Display::SetBacklight(bool on) const {
  // TODO(debug): remove -- traces every SetBacklight() call.
  ESP_LOGW(kTag, "SetBacklight(%d) t=%lld us", on,
           static_cast<long long>(esp_timer_get_time()));
  gpio_set_level(board::kLcdPinBacklight, on ? 1 : 0);
}

bool Gc9a01Display::Lock(uint32_t timeout_ms) {
  return lvgl_port_lock(timeout_ms);
}

void Gc9a01Display::Unlock() { lvgl_port_unlock(); }

}  // namespace display
