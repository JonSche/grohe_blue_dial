#include "display/single_frame_test.hpp"

#include "board/board_config.hpp"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gc9a01_vendor/gc9a01_vendor_init.hpp"
#include "lcd_panel_gc9a01.h"

namespace display {
namespace {
constexpr char kTag[] = "single_frame_test";
constexpr size_t kPixelCount = static_cast<size_t>(board::kLcdHorizontalResolution) *
                               board::kLcdVerticalResolution;

// TODO(debug): on_color_trans_done fires from ISR context (the SPI DMA
// completion interrupt) -- calling ESP_LOG* directly from here crashes
// ("Stack protection fault", confirmed on hardware), since the regular log
// path isn't ISR-safe. So this only does ISR-safe work (increment a
// counter, stash a timestamp); the actual logging happens later, safely,
// from task context in Run()'s alive loop below.
volatile int g_trans_done_count = 0;
volatile int64_t g_trans_done_last_us = 0;

bool OnColorTransDone(esp_lcd_panel_io_handle_t /*panel_io*/,
                      esp_lcd_panel_io_event_data_t* /*edata*/,
                      void* /*user_ctx*/) {
  g_trans_done_count++;
  g_trans_done_last_us = esp_timer_get_time();
  return false;
}
}  // namespace

SingleFrameTest::~SingleFrameTest() {
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

esp_err_t SingleFrameTest::Init() {
  // TODO(experiment A): GPIO8 is configured as output and driven LOW here,
  // ONCE, and never touched again for the rest of execution -- no later
  // gpio_set_level() call on this pin exists anywhere in this file.
  // Testing the hypothesis that GPIO8 is an active-LOW backlight enable
  // (P-channel MOSFET, per bsp_lcd.c's duty math and the board schematic)
  // and that our previous active-HIGH assumption was inverted.
  const gpio_config_t backlight_cfg = {
      .pin_bit_mask = 1ULL << board::kLcdPinBacklight,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&backlight_cfg));
  gpio_set_level(board::kLcdPinBacklight, 0);
  ESP_LOGW(kTag, "GPIO8 driven LOW t=%lld us -- will NEVER be changed again",
           static_cast<long long>(esp_timer_get_time()));

  ESP_LOGI(kTag, "Initializing SPI bus");
  const spi_bus_config_t bus_cfg = GC9A01_PANEL_BUS_SPI_CONFIG(
      board::kLcdPinSclk, board::kLcdPinMosi,
      board::kLcdHorizontalResolution * board::kLcdVerticalResolution *
          static_cast<int>(sizeof(uint16_t)));
  ESP_ERROR_CHECK(
      spi_bus_initialize(board::kLcdSpiHost, &bus_cfg, SPI_DMA_CH_AUTO));
  spi_bus_initialized_ = true;

  ESP_LOGI(kTag, "Installing panel IO (with on_color_trans_done registered)");
  esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(
      board::kLcdPinCs, board::kLcdPinDc, OnColorTransDone, nullptr);
  io_cfg.pclk_hz = board::kLcdPixelClockHz;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      static_cast<esp_lcd_spi_bus_handle_t>(board::kLcdSpiHost), &io_cfg,
      &panel_io_));

  ESP_LOGI(kTag, "Installing ported GC9A01 panel driver (lcd_new_panel_gc9a01)");
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

  frame_buffer_ = static_cast<uint16_t*>(
      heap_caps_malloc(kPixelCount * sizeof(uint16_t), MALLOC_CAP_DMA));
  if (frame_buffer_ == nullptr) {
    ESP_LOGE(kTag, "Failed to allocate %u-byte frame buffer",
             static_cast<unsigned>(kPixelCount * sizeof(uint16_t)));
    return ESP_ERR_NO_MEM;
  }

  // Solid red, RGB565. GC9A01 expects big-endian pixel data over SPI; the
  // CPU is little-endian, so swap once before filling (matches
  // bringup::ColorCycleTest's FillColor()).
  constexpr uint16_t kRed = 0xF800;
  const uint16_t swapped = __builtin_bswap16(kRed);
  for (size_t i = 0; i < kPixelCount; ++i) {
    frame_buffer_[i] = swapped;
  }

  ESP_LOGI(kTag, "Calling draw_bitmap() exactly once: solid red, %dx%d",
           board::kLcdHorizontalResolution, board::kLcdVerticalResolution);
  ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
      panel_, 0, 0, board::kLcdHorizontalResolution,
      board::kLcdVerticalResolution, frame_buffer_));
  const int64_t draw_bitmap_returned_us = esp_timer_get_time();
  ESP_LOGI(kTag, "draw_bitmap() returned t=%lld us. No further draws will ever happen.",
           static_cast<long long>(draw_bitmap_returned_us));

  // TODO(experiment A): deliberately no backlight handling here at all --
  // GPIO8 was already driven LOW once, above, before any of this, and stays
  // that way. Nothing touches it again.
  return ESP_OK;
}

void SingleFrameTest::Run() {
  // TODO(debug): this task does nothing to the panel from here on -- no
  // further draw_bitmap(), no LVGL, no SPI activity of any kind from our
  // side. It only logs "alive" once a second so we can tell, from the log
  // timestamps alone, whether any observed black flash lines up with
  // anything this firmware does (it shouldn't -- there's nothing left to
  // line up with) or happens independently of software.
  for (;;) {
    ESP_LOGI(kTag, "alive t=%lld us on_color_trans_done_count=%d last_fired_t=%lld us",
             static_cast<long long>(esp_timer_get_time()), g_trans_done_count,
             static_cast<long long>(g_trans_done_last_us));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace display
