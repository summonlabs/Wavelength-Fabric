#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Typed status for every fallible runtime boundary.
//
// Status never collapses distinct failures into a boolean. In particular
// UNSUPPORTED, UNKNOWN, STALE and REFUSED remain distinguishable from success
// and from one another all the way to the caller.

namespace wavelength_fabric {

enum class StatusCode : std::uint8_t {
  Ok = 0,
  InvalidArgument = 1,
  NotFound = 2,
  Duplicate = 3,
  LimitExceeded = 4,
  Unsupported = 5,
  Unknown = 6,
  StaleGeneration = 7,
  StaleIncarnation = 8,
  StaleEpoch = 9,
  StaleAuthority = 10,
  IllegalTransition = 11,
  Conflict = 12,
  Excluded = 13,
  PersistenceFailure = 14,
  Corruption = 15,
  IoError = 16,
  NotPermitted = 17,
  Overflow = 18,
  Refused = 19,
  Unavailable = 20,
};

[[nodiscard]] std::string_view toToken(StatusCode code) noexcept;
[[nodiscard]] StatusCode statusCodeFromToken(std::string_view token) noexcept;
[[nodiscard]] bool isStale(StatusCode code) noexcept;

struct Status {
  StatusCode code{StatusCode::Ok};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::Ok; }

  [[nodiscard]] static Status success() { return Status{}; }

  [[nodiscard]] static Status failure(StatusCode code, std::string message) {
    Status status;
    status.code = code;
    status.message = std::move(message);
    return status;
  }

  friend bool operator==(const Status&, const Status&) = default;
};

[[nodiscard]] inline Status okStatus() { return Status{}; }

[[nodiscard]] inline Status fail(StatusCode code, std::string message) {
  return Status::failure(code, std::move(message));
}

}  // namespace wavelength_fabric
