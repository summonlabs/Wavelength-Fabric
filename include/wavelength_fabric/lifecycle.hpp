#pragma once

#include <cstdint>
#include <string_view>

// The reservation lifecycle state machine.
//
// Illegal transitions are rejected deterministically. A committed reservation
// is the only thing that owns spectrum; the transient states around it exist
// so that a commit is never observable in a half-applied form.

namespace wavelength_fabric {

enum class ReservationState : std::uint8_t {
  None = 0,
  Requested = 1,
  Evaluated = 2,
  Committing = 3,
  Reserved = 4,
  Activating = 5,
  Active = 6,
  Deactivating = 7,
  Renewing = 8,
  Releasing = 9,
  Reclaiming = 10,
  Recovering = 11,
  Released = 12,
  Expired = 13,
  Reclaimed = 14,
  Superseded = 15,
  Refused = 16,
  Retired = 17,
};

[[nodiscard]] std::string_view toToken(ReservationState state) noexcept;
[[nodiscard]] ReservationState reservationStateFromToken(std::string_view token) noexcept;

// Terminal states never transition again.
[[nodiscard]] bool isTerminalState(ReservationState state) noexcept;

// Live states are the states in which a reservation can own spectrum, subject
// to its lease still being valid at the evaluation instant.
[[nodiscard]] bool isLiveState(ReservationState state) noexcept;

// Transient states are observable only inside a single guarded operation.
[[nodiscard]] bool isTransientState(ReservationState state) noexcept;

[[nodiscard]] bool isLegalTransition(ReservationState from, ReservationState to) noexcept;

// Human-readable reason why a transition is illegal; empty when it is legal.
[[nodiscard]] std::string_view illegalTransitionReason(ReservationState from, ReservationState to) noexcept;

}  // namespace wavelength_fabric
