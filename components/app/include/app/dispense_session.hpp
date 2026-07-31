#pragma once

#include <cstdint>

namespace app {

// A small stopwatch wrapping the appliance's empirically-measured physical
// dispense-duration model (grohe_ble::PredictDispenseDurationMs()) --
// isolates the timing math from DialController's own state machine, so a
// future progress indicator or recalibration only ever touches this class.
//
// Not thread-safe; used exclusively from DialController, itself only ever
// touched from the app task (see app::App::Run()).
class DispenseSession {
 public:
  // Starts (or restarts) the session for a dispense of amount_ml, predicted
  // from now_us via grohe_ble::PredictDispenseDurationMs().
  void Start(int amount_ml, int64_t now_us);

  // Ends the session (a stop() acknowledgement, or Finished() firing).
  // Idempotent -- safe to call when not active().
  void Stop();

  [[nodiscard]] bool active() const { return active_; }

  // True once now_us has reached the predicted end time. Always false when
  // !active().
  [[nodiscard]] bool Finished(int64_t now_us) const;

  // Microseconds remaining until the predicted end time, clamped to >= 0.
  // 0 when !active().
  [[nodiscard]] int64_t Remaining(int64_t now_us) const;

 private:
  bool active_ = false;
  int64_t end_time_us_ = 0;
};

}  // namespace app
