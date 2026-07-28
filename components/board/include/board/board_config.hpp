#pragma once

// Pin and bus configuration for the VIEWE UEDX24240013-MD50E-B:
//   - ESP32-C3, 240x240 round GC9A01 LCD over 4-wire SPI with a hardware
//     reset line (GPIO2) and a tearing-effect input (GPIO5).
//   - EC11-style rotary encoder (2-phase, no integrated push button).
//   - A separate momentary button wired to IO9.
//
// This is the only file that should know about physical GPIO numbers; every
// other component takes pins/handles through this header instead of
// hard-coding them.

#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace board {

// --- LCD (GC9A01, 4-wire SPI) -----------------------------------------
inline constexpr gpio_num_t kLcdPinSclk = GPIO_NUM_1;
inline constexpr gpio_num_t kLcdPinMosi = GPIO_NUM_0;
inline constexpr gpio_num_t kLcdPinCs = GPIO_NUM_10;
inline constexpr gpio_num_t kLcdPinDc = GPIO_NUM_4;
inline constexpr gpio_num_t kLcdPinBacklight = GPIO_NUM_8;
// Tearing-effect output from the panel, used to gate LVGL flushes against
// the panel's own scan line and avoid visible tearing.
inline constexpr gpio_num_t kLcdPinTe = GPIO_NUM_5;
// Hardware reset line for the panel.
inline constexpr gpio_num_t kLcdPinReset = GPIO_NUM_2;

inline constexpr spi_host_device_t kLcdSpiHost = SPI2_HOST;
inline constexpr int kLcdPixelClockHz = 80 * 1000 * 1000;

inline constexpr int kLcdHorizontalResolution = 240;
inline constexpr int kLcdVerticalResolution = 240;

// --- Rotary encoder + button -------------------------------------------
inline constexpr gpio_num_t kEncoderPinPhaseA = GPIO_NUM_7;
inline constexpr gpio_num_t kEncoderPinPhaseB = GPIO_NUM_6;
inline constexpr gpio_num_t kButtonPin = GPIO_NUM_9;

// --- UART (debug header) ------------------------------------------------
inline constexpr gpio_num_t kUartPinRx = GPIO_NUM_20;
inline constexpr gpio_num_t kUartPinTx = GPIO_NUM_21;

}  // namespace board
