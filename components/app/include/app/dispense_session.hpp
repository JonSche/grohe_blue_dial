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

  // The total predicted duration set by the most recent Start() call, in
  // microseconds -- exposed (M11) so a caller that already knows the
  // dispensed amount can derive delivered volume (amount_ml * elapsed /
  // duration) without this class needing to know about milliliters at
  // all. 0 when !active(), matching Remaining()'s own convention.
  [[nodiscard]] int64_t DurationUs() const { return active_ ? duration_us_ : 0; }

 private:
  bool active_ = false;
  int64_t end_time_us_ = 0;
  int64_t duration_us_ = 0;
};

}  // namespace app
