#pragma once

#include <cstdint>

// Abstract source of the current Unix epoch, so protocol code (GroheProtocol
// -- see grohe_protocol.hpp) never knows or cares where the value came from.
// The one implementation today is SntpTimeProvider (sntp_time_provider.hpp);
// this interface exists so that never has to change if a future milestone
// adds a different source.
namespace time_service {

class TimeProvider {
 public:
  virtual ~TimeProvider() = default;

  // Cheap, no-output-param check for status display (see
  // app::DialController::HandleTimeStatus()) -- does not itself imply the
  // value GetCurrentEpoch() would return is fresh at this exact instant,
  // only that a value is available at all.
  [[nodiscard]] virtual bool IsValid() const = 0;

  // Returns true and fills *out_epoch_seconds with the current Unix epoch
  // if valid time is available; returns false (leaving *out_epoch_seconds
  // untouched) otherwise. Never fabricates a timestamp when time isn't
  // known -- callers (see grohe_protocol.hpp's BuildDispensePayload()/
  // BuildStopPayload()) must treat a false return as a reason to reject
  // the command, not a reason to guess.
  [[nodiscard]] virtual bool GetCurrentEpoch(uint32_t* out_epoch_seconds) const = 0;
};

}  // namespace time_service
