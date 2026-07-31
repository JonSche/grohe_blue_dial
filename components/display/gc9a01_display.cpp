#include "display/gc9a01_display.hpp"

#include "board/board_config.hpp"
#include "esp_lcd_gc9a01.h"
#include "gc9a01_vendor/gc9a01_vendor_init.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "lcd_panel_gc9a01.h"

namespace display {
namespace {
constexpr char kTag[] = "gc9a01_display";

// LVGL glue constants. The vendor's own lvgl_port.c is the behavioral
// reference for init order and memory footprint, translated to LVGL v9.
constexpr uint32_t kLvTickPeriodMs = 2;
// 10ms, not the vendor's 5ms: at CONFIG_FREERTOS_HZ=100 (10ms/tick),
// pdMS_TO_TICKS(5) truncates to 0 ticks, so vTaskDelay() would never
// actually block. 10ms is the smallest delay at this tick rate that blocks
// for a real, non-zero tick, and is the closest achievable approximation of
// the original 5ms cadence.
constexpr uint32_t kLvglTaskPeriodMs = 10;
// Known-good size carried over from esp_lvgl_port's default. The vendor's
// own task uses a smaller 4096-byte stack; shrinking this is a separate,
// later optimization gated on a high-water-mark measurement
// (uxTaskGetStackHighWaterMark()).
constexpr uint32_t kLvglTaskStackSize = 7168;
constexpr UBaseType_t kLvglTaskPriority = 5;
constexpr BaseType_t kLvglTaskCore = 0;

void FlushCbTrampoline(lv_display_t* disp, const lv_area_t* area,
                       uint8_t* px_map) {
  auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
  esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1,
                             area->y2 + 1, px_map);
}

bool OnColorTransDoneTrampoline(esp_lcd_panel_io_handle_t /*panel_io*/,
                                 esp_lcd_panel_io_event_data_t* /*edata*/,
                                 void* user_ctx) {
  lv_display_flush_ready(static_cast<lv_display_t*>(user_ctx));
  return false;
}

void TickCb(void* /*arg*/) { lv_tick_inc(kLvTickPeriodMs); }
}  // namespace

Gc9a01Display* Gc9a01Display::instance_ = nullptr;

Gc9a01Display::~Gc9a01Display() {
  if (lvgl_task_ != nullptr) {
    vTaskDelete(lvgl_task_);
    lvgl_task_ = nullptr;
  }
  if (lv_tick_timer_ != nullptr) {
    esp_timer_stop(lv_tick_timer_);
    esp_timer_delete(lv_tick_timer_);
    lv_tick_timer_ = nullptr;
  }
  if (lock_mutex_ != nullptr) {
    vSemaphoreDelete(lock_mutex_);
    lock_mutex_ = nullptr;
  }
  if (instance_ == this) {
    instance_ = nullptr;
  }
  if (lv_display_ != nullptr) {
    lv_display_delete(lv_display_);
    lv_display_ = nullptr;
  }
  if (lv_buf_ != nullptr) {
    heap_caps_free(lv_buf_);
    lv_buf_ = nullptr;
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

  ESP_LOGI(kTag, "Starting self-owned LVGL v9 integration");
  lv_init();

  // Small row-based partial buffer, not a full-screen framebuffer: neither
  // LVGL, the GC9A01 (which has its own GRAM and full CASET/RASET windowed-
  // write support -- see lcd_panel_gc9a01.c), nor this UI's mostly-static,
  // small-widget rendering pattern requires the MCU to hold a full 240x240
  // frame at once (see docs/ARCHITECTURE.md and the M3.2 architecture
  // review). LV_DISPLAY_RENDER_MODE_PARTIAL computes buffer height as
  // buf_size / stride (lv_display.c), so kPartialBufRows directly controls
  // how many full-width rows this buffer covers. 30 rows (of LVGL's
  // recommended >=1/10-screen minimum) keeps the allocation two orders of
  // magnitude smaller than the old 115200-byte full frame, comfortably
  // within every DMA-capable heap region measured in the BLE RCA.
  //
  // Single-buffered, as before: LVGL simply waits for this (now much
  // smaller and faster) buffer's flush to complete before rendering the
  // next chunk, which remains imperceptible for this UI.
  constexpr uint32_t kPartialBufRows = 30;
  const uint32_t hres = static_cast<uint32_t>(board::kLcdHorizontalResolution);
  const uint32_t vres = static_cast<uint32_t>(board::kLcdVerticalResolution);
  const size_t buffer_size_bytes = kPartialBufRows * hres * sizeof(uint16_t);
  constexpr uint32_t kBufCaps =
      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

  lv_buf_ = heap_caps_malloc(buffer_size_bytes, kBufCaps);
  if (lv_buf_ == nullptr) {
    ESP_LOGE(kTag, "draw buffer allocation failed (%u bytes)",
             static_cast<unsigned>(buffer_size_bytes));
    return ESP_FAIL;
  }

  lv_display_ = lv_display_create(static_cast<int32_t>(hres),
                                   static_cast<int32_t>(vres));
  if (lv_display_ == nullptr) {
    ESP_LOGE(kTag, "lv_display_create failed");
    return ESP_FAIL;
  }
  // Render natively pre-swapped (matches the vendor project's
  // CONFIG_LV_COLOR_16_SWAP=y, which bakes the swap into every LVGL color at
  // the type level in their LVGL v8 build) instead of rendering
  // LV_COLOR_FORMAT_RGB565 and applying a separate byte-swap pass over
  // every flush buffer. LVGL v9 has no global "16-bit swap" build option --
  // RGB565_SWAPPED is the per-display equivalent.
  lv_display_set_color_format(lv_display_, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_buffers(lv_display_, lv_buf_, nullptr, buffer_size_bytes,
                          LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_user_data(lv_display_, panel_);
  lv_display_set_flush_cb(lv_display_, FlushCbTrampoline);

  const esp_lcd_panel_io_callbacks_t io_cbs = {
      .on_color_trans_done = OnColorTransDoneTrampoline,
  };
  ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
      panel_io_, &io_cbs, lv_display_));

  const esp_timer_create_args_t tick_timer_args = {
      .callback = TickCb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "lv_tick",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &lv_tick_timer_));
  ESP_ERROR_CHECK(esp_timer_start_periodic(
      lv_tick_timer_, static_cast<uint64_t>(kLvTickPeriodMs) * 1000));

  instance_ = this;
  lock_mutex_ = xSemaphoreCreateRecursiveMutex();
  if (lock_mutex_ == nullptr) {
    ESP_LOGE(kTag, "xSemaphoreCreateRecursiveMutex failed");
    return ESP_FAIL;
  }

  const BaseType_t task_created = xTaskCreatePinnedToCore(
      &Gc9a01Display::TaskLoop, "lvgl", kLvglTaskStackSize, nullptr,
      kLvglTaskPriority, &lvgl_task_, kLvglTaskCore);
  if (task_created != pdPASS) {
    ESP_LOGE(kTag, "xTaskCreatePinnedToCore failed");
    return ESP_FAIL;
  }

  SetBacklight(true);
  return ESP_OK;
}

void Gc9a01Display::SetBacklight(bool on) const {
  // Active-LOW: confirmed via the vendor's bsp_lcd.c duty math, the board
  // schematic's P-channel MOSFET (Q1, CJ3407) high-side switch, and
  // empirically on hardware. LOW = ON, HIGH = OFF.
  gpio_set_level(board::kLcdPinBacklight, on ? 0 : 1);
}

bool Gc9a01Display::Lock(uint32_t timeout_ms) {
  if (instance_ == nullptr || instance_->lock_mutex_ == nullptr) {
    return false;
  }
  const TickType_t timeout_ticks =
      (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(instance_->lock_mutex_, timeout_ticks) ==
         pdTRUE;
}

void Gc9a01Display::Unlock() {
  if (instance_ != nullptr && instance_->lock_mutex_ != nullptr) {
    xSemaphoreGiveRecursive(instance_->lock_mutex_);
  }
}

void Gc9a01Display::TaskLoop(void* /*arg*/) {
  for (;;) {
    static_cast<void>(Lock());
    lv_timer_handler();
    Unlock();
    vTaskDelay(pdMS_TO_TICKS(kLvglTaskPeriodMs));
  }
}

}  // namespace display
