#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wavelength_fabric/candidate.hpp"
#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/reservation.hpp"

// Typed allocation outcomes and their explanations.
//
// A refusal is never reported as success, and distinct refusals stay distinct:
// UNSUPPORTED, UNKNOWN capability, CONFLICT, STALE authority, INCOMPLETE
// requirements and INVALID requests are all separately observable.

namespace wavelength_fabric {

enum class AllocationOutcome : std::uint8_t {
  Allocated = 0,
  RefusedUnsupported = 1,
  RefusedUnknownCapability = 2,
  RefusedNoCapacity = 3,
  RefusedConflict = 4,
  RefusedContiguity = 5,
  RefusedContinuity = 6,
  RefusedConversion = 7,
  RefusedConstraint = 8,
  RefusedExclusion = 9,
  RefusedStaleGeneration = 10,
  RefusedStaleIncarnation = 11,
  RefusedStaleEpoch = 12,
  RefusedStaleAuthority = 13,
  RefusedStaleCapability = 14,
  RefusedDuplicate = 15,
  RefusedInvalidRequest = 16,
  RefusedLeaseExpired = 17,
  RefusedLimitExceeded = 18,
  RefusedUnknownDomain = 19,
  RefusedNotPermitted = 20,
  RefusedChannelWidth = 21,
  Unknown = 22,
};

[[nodiscard]] std::string_view toToken(AllocationOutcome outcome) noexcept;
[[nodiscard]] AllocationOutcome allocationOutcomeFromToken(std::string_view token) noexcept;
[[nodiscard]] bool isRefusal(AllocationOutcome outcome) noexcept;

struct DecisionExplanation {
  AllocationOutcome outcome{AllocationOutcome::Unknown};
  ReservationId reservation{};
  ReservationGeneration generation{};

  // The fence that evaluated the request.
  ControllerFence fence{};

  std::size_t candidatesEnumerated{0};
  std::size_t candidatesRejected{0};
  std::size_t candidatesOmitted{0};
  std::size_t candidateOrdinal{0};

  // Present when the outcome is Allocated.
  SpectrumCandidate selected{};

  // Bounded list of rejected candidates that explain a refusal.
  std::vector<SpectrumCandidate> rejected;

  // Reservations that blocked the request, in ascending identity order.
  std::vector<ReservationId> conflicts;

  // Human-readable, ordered reasons. The first reason is the decisive one.
  std::vector<std::string> reasons;

  [[nodiscard]] std::string summary() const;
};

struct AllocationDecision {
  // Ok only when a reservation committed. Every refusal carries a typed
  // outcome and a non-Ok status so callers cannot mistake it for success.
  Status status;
  AllocationOutcome outcome{AllocationOutcome::Unknown};
  ReservationId reservation{};
  ReservationGeneration generation{};
  DecisionExplanation explanation;

  [[nodiscard]] bool allocated() const noexcept {
    return status.ok() && outcome == AllocationOutcome::Allocated;
  }
  [[nodiscard]] std::string describe() const;
};

// Result of a lease expiry sweep.
struct ReclaimReport {
  Status status;
  Instant evaluatedAt{};
  ControllerFence fence{};

  std::size_t scanned{0};
  std::size_t expired{0};

  // Reservations moved to Reclaimed by this sweep, in ascending identity order.
  std::vector<ReservationId> reclaimed;
  // Reservations marked Expired but deliberately not reclaimed.
  std::vector<ReservationId> lapsed;

  [[nodiscard]] std::string describe() const;
};

// Result of durable recovery.
struct RecoveryReport {
  Status status;
  bool recovered{false};
  std::string source;

  ControllerFence fence{};
  RuntimeGeneration runtimeGeneration{};
  RecoveryGeneration recoveryGeneration{};

  std::uint64_t recordsRead{0};
  std::uint64_t recordsAccepted{0};
  std::uint64_t recordsRejected{0};

  std::size_t gridsRestored{0};
  std::size_t domainsRestored{0};
  std::size_t exclusionDomainsRestored{0};
  std::size_t capabilitiesRestored{0};
  std::size_t reservationsRestored{0};
  std::size_t auditsRestored{0};

  // Reservations that were Active in the persisted image and were conservatively
  // demoted because activation evidence is not durable.
  std::size_t demotedFromActive{0};

  std::vector<std::string> diagnostics;

  [[nodiscard]] std::string describe() const;
};

}  // namespace wavelength_fabric
