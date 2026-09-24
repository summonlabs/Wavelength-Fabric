#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavelength_fabric/authority.hpp"
#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/grid.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/quantity.hpp"
#include "wavelength_fabric/time.hpp"

// A spectrum allocation request.
//
// The request names every optical resource it spans. When it spans more than
// one resource the continuity and contiguity requirements must be stated
// explicitly: an unstated requirement on a multi-resource request is refused
// rather than guessed.

namespace wavelength_fabric {

enum class ContinuityRequirement : std::uint8_t {
  Unspecified = 0,
  Required = 1,
  NotRequired = 2,
};

enum class ContiguityRequirement : std::uint8_t {
  Unspecified = 0,
  Required = 1,
  NotRequired = 2,
};

[[nodiscard]] std::string_view toToken(ContinuityRequirement requirement) noexcept;
[[nodiscard]] std::string_view toToken(ContiguityRequirement requirement) noexcept;

// Explicit constraints applied during candidate enumeration.
struct SpectrumConstraints {
  // Slot ranges in the request grid that must remain unused.
  std::vector<SlotRange> excludedSlots;
  // Absolute frequency ranges that must remain unused.
  std::vector<FrequencyRange> excludedFrequencies;
  // Additional reservations that the candidate must not conflict with. Used to
  // express coexistence requirements that are not implied by the exclusion
  // domains alone.
  std::vector<ReservationId> mustNotConflictWith;
};

struct SpectrumRequest {
  AllocationRequestId requestId{};
  AllocationRequestGeneration requestGeneration{};

  OwnerId owner{};
  OwnerGeneration ownerGeneration{};

  // The optical resources this request spans, in caller order. Order is part
  // of the input: identical inputs, including order, produce identical
  // decisions.
  std::vector<SpectrumDomainId> domains;
  std::vector<SpectrumDomainGeneration> domainGenerations;

  // The grid the request is expressed in. For a multi-resource request that
  // requires continuity, every spanned domain must be on this grid and
  // generation unless conversion capability evidence is supplied.
  ChannelGridId grid{};
  GridGeneration gridGeneration{};

  // Channel width in slots. Must be within the grid per-channel bounds.
  std::uint32_t slots{1};

  ContiguityRequirement contiguity{ContiguityRequirement::Unspecified};
  ContinuityRequirement continuity{ContinuityRequirement::Unspecified};

  // Permitted absolute frequency windows. Empty means the whole grid.
  std::vector<FrequencyRange> frequencyWindows;

  // Separation kept from any other live allocation.
  std::int64_t guardBandMhz{0};

  Duration leaseDuration{};
  std::uint32_t maxRenewals{0};  // 0 means the lease may be renewed without a fixed cap

  // Lease start is the later of requestedAt and notBefore; the lease ends at
  // start + leaseDuration.
  Instant requestedAt{};
  Instant notBefore{};

  PolicyGeneration policyGeneration{};
  PriorityGeneration priorityGeneration{};

  SpectrumConstraints constraints;

  EligibilityAuthority eligibilityAuthority{};
  ReservationAuthority reservationAuthority{};
};

// Structural validation: identity validity, bounded sizes, ranges, and the
// explicit-requirement rules for multi-resource and multi-slot requests.
[[nodiscard]] Status validateRequestShape(const SpectrumRequest& request);

[[nodiscard]] std::string describeRequest(const SpectrumRequest& request);

}  // namespace wavelength_fabric
