#pragma once

#include <cstdint>
#include <string>

#include "wavelength_fabric/decision.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/time.hpp"

// Audit history.
//
// Every ownership-affecting operation appends one bounded audit record that
// carries the controller fence which performed it. Records are also part of
// persisted state, so a recovered runtime can explain what the previous
// incarnation did.

namespace wavelength_fabric {

enum class AuditKind : std::uint8_t {
  None = 0,
  RuntimeOpened = 1,
  RecoveryPerformed = 2,
  GridRegistered = 3,
  SpanRegistered = 4,
  PortRegistered = 5,
  DomainRegistered = 6,
  ExclusionDomainRegistered = 7,
  CapabilityPublished = 8,
  RequestEvaluated = 9,
  AllocationCommitted = 10,
  AllocationRefused = 11,
  ReservationRenewed = 12,
  ReservationActivated = 13,
  ReservationDeactivated = 14,
  ReservationReleased = 15,
  ReservationExpired = 16,
  ReservationReclaimed = 17,
  ReservationSuperseded = 18,
  AuthorityAdvanced = 19,
  PersistenceFlushed = 20,
  PersistenceFailed = 21,
  CorruptionDetected = 22,
  ReplayRejected = 23,
  StateReset = 24,
};

[[nodiscard]] std::string_view toToken(AuditKind kind) noexcept;
[[nodiscard]] AuditKind auditKindFromToken(std::string_view token) noexcept;

struct AuditRecord {
  AuditSequence sequence{};
  Instant at{};
  AuditKind kind{AuditKind::None};
  ControllerFence fence{};
  AllocationOutcome outcome{AllocationOutcome::Unknown};
  ReservationId reservation{};
  ReservationGeneration reservationGeneration{};
  SpectrumDomainId domain{};
  SlotRange slots{};
  FrequencyRange frequency{};
  std::string detail;

  [[nodiscard]] std::string describe() const;
};

}  // namespace wavelength_fabric
