#include "app/dispense_session.hpp"

#include "grohe_ble/grohe_protocol.hpp"

namespace app {

void DispenseSession::Start(int amount_ml, int64_t now_us) {
  const uint32_t duration_ms = grohe_ble::PredictDispenseDurationMs(amount_ml);
  end_time_us_ = now_us + static_cast<int64_t>(duration_ms) * 1000;
  active_ = true;
}

void DispenseSession::Stop() { active_ = false; }

bool DispenseSession::Finished(int64_t now_us) const {
  return active_ && now_us >= end_time_us_;
}

int64_t DispenseSession::Remaining(int64_t now_us) const {
  if (!active_) {
    return 0;
  }
  const int64_t remaining = end_time_us_ - now_us;
  return remaining > 0 ? remaining : 0;
}

}  // namespace app
