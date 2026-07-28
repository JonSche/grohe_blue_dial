#include "bringup/color_cycle_test.hpp"

#include "board/board_config.hpp"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gc9a01_vendor/gc9a01_vendor_init.hpp"

namespace bringup {
namespace {
constexpr char kTag[] = "color_cycle_test";
constexpr size_t kPixelCount = static_cast<size_t>(board::kLcdHorizontalResolution) *
                               board::kLcdVerticalResolution;

struct NamedColor {
  const char* name;
  uint16_t rgb565;
};

// Standard RGB565 primaries plus the two extremes, per the milestone spec.
constexpr NamedColor kColors[] = {
    {"red", 0xF800},   {"green", 0x07E0}, {"blue", 0x001F},
    {"white", 0xFFFF}, {"black", 0x0000},
};
}  // namespace

ColorCycleTest::~ColorCycleTest() {
  if (frame_buffer_ != nullptr) {
    heap_caps_free(frame_buffer_);
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

esp_err_t ColorCycleTest::Init() {
  const gpio_config_t backlight_cfg = {
      .pin_bit_mask = 1ULL << board::kLcdPinBacklight,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&backlight_cfg));
  gpio_set_level(board::kLcdPinBacklight, 0);

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
  ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io_, &panel_cfg, &panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

  frame_buffer_ = static_cast<uint16_t*>(
      heap_caps_malloc(kPixelCount * sizeof(uint16_t), MALLOC_CAP_DMA));
  if (frame_buffer_ == nullptr) {
    ESP_LOGE(kTag, "Failed to allocate %u-byte frame buffer",
             static_cast<unsigned>(kPixelCount * sizeof(uint16_t)));
    return ESP_ERR_NO_MEM;
  }

  // Clear to black before the backlight comes on so nothing stale/garbled
  // is visible for even one frame.
  ESP_ERROR_CHECK(FillColor(0x0000));
  gpio_set_level(board::kLcdPinBacklight, 1);

  return ESP_OK;
}

esp_err_t ColorCycleTest::FillColor(uint16_t rgb565_color) const {
  // The GC9A01 expects big-endian pixel data over SPI; our CPU is
  // little-endian, so each 16-bit color needs a byte swap before it goes
  // out. A real UI path would do this via esp_lvgl_port's swap_bytes flag;
  // here there's no LVGL, so it's done by hand, once per fill (not
  // per-frame, since this isn't a real-time redraw path).
  const uint16_t swapped = __builtin_bswap16(rgb565_color);
  for (size_t i = 0; i < kPixelCount; ++i) {
    frame_buffer_[i] = swapped;
  }
  return esp_lcd_panel_draw_bitmap(panel_, 0, 0, board::kLcdHorizontalResolution,
                                    board::kLcdVerticalResolution, frame_buffer_);
}

void ColorCycleTest::Run() {
  ESP_LOGI(kTag, "Cycling colors (raw esp_lcd fill, no LVGL)");
  for (;;) {
    for (const auto& color : kColors) {
      ESP_LOGI(kTag, "Fill: %s", color.name);
      ESP_ERROR_CHECK(FillColor(color.rgb565));
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

}  // namespace bringup
