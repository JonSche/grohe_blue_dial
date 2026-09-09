#pragma once

#include "dial_state/dial_state.hpp"

// Persistent (NVS-backed) dial settings -- M13.1. The three values here
// exist only as compile-time constants in dial_state.hpp today (the
// default dispense amount, the encoder's rotate step size, and the
// default water type); this component adds a runtime-writable, NVS-
// backed layer on top of them, without touching dial_state.hpp itself,
// which stays the fallback for a device with nothing stored yet -- see
// DialSettingsStore::Init()'s own comment.
//
// Deliberately independent of app/ and every other component above
// dial_state/ in the dependency graph (see docs/ARCHITECTURE.md's "Goals
// behind the structure") -- this component has no notion that
// app::DialController or app::App exist. app::App owns the one
// DialSettingsStore instance and is the only thing that ever feeds its
// Values() into app::DialController (see DialController::ApplySettings()).
namespace settings {

struct DialSettings {
  // Mirrors dial_state::DialState::amount_ml's own default (dial_state.hpp
  // has no named constant for it, only the struct's in-class initializer).
  int default_amount_ml = 500;
  int amount_step_ml = dial_state::kAmountStepMl;
  dial_state::WaterType default_water_type = dial_state::WaterType::kSparkling;
};

class DialSettingsStore {
 public:
  DialSettingsStore() = default;

  // Opens NVS (idempotent nvs_flash_init() -- matches this project's own
  // established multi-caller-safe pattern; see e.g.
  // time_service::WifiConnection::Init()) and loads any previously
  // stored, validated settings. Never fails hard: any problem (NVS not
  // ready, nothing stored yet, an out-of-range/corrupt entry) just
  // leaves Values() at its own compile-time defaults -- settings are a
  // convenience layered on top of dial_state.hpp's existing constants,
  // never a new startup dependency. Call once, before reading Values().
  void Init();

  [[nodiscard]] const DialSettings& Values() const { return values_; }

  // Validates (the same bounds Init() itself enforces -- see
  // dial_settings.cpp's IsValid()) before writing anything, then persists
  // the whole struct as a single NVS blob entry and commits it. Updates
  // Values() immediately on success, no reboot required. Returns false
  // (NVS and Values() both untouched) if validation fails.
  [[nodiscard]] bool Set(const DialSettings& new_values);

 private:
  DialSettings values_{};
};

}  // namespace settings
