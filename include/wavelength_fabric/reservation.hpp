#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavelength_fabric/capability.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/lifecycle.hpp"
#include "wavelength_fabric/quantity.hpp"
#include "wavelength_fabric/time.hpp"

// A committed spectrum reservation, its lease, and its occupancy accounting.

namespace wavelength_fabric {

struct Lease {
  LeaseGeneration generation{};
  Instant grantedAt{};
  Instant expiresAt{};
  std::uint32_t renewalCount{0};
  std::uint32_t maxRenewals{0};  // zero means the lease may be renewed without a fixed cap

  [[nodiscard]] bool validAt(Instant now) const noexcept { return expiresAt > now; }
  [[nodiscard]] bool expiredAt(Instant now) const noexcept { return !validAt(now); }
  [[nodiscard]] bool renewable() const noexcept {
    return maxRenewals == 0 || renewalCount < maxRenewals;
  }
};

struct SpectrumReservation {
  ReservationId id{};
  ReservationGeneration generation{};

  AllocationRequestId requestId{};
  AllocationRequestGeneration requestGeneration{};

  OwnerId owner{};
  OwnerGeneration ownerGeneration{};

  ReservationState state{ReservationState::None};

  // The optical resources this reservation owns spectrum on.
  std::vector<SpectrumDomainId> domains;
  std::vector<SpectrumDomainGeneration> domainGenerations;

  // Anchor domain: the domain whose grid expresses slots and frequency.
  SpectrumDomainId anchorDomain{};
  ChannelGridId grid{};
  GridGeneration gridGeneration{};

  SlotRange slots{};
  FrequencyRange frequency{};
  // Per-domain slot ranges resolved at commit time, parallel to domains. For a
  // single-grid request this repeats slots on every domain.
  std::vector<SlotRange> perDomainSlots;

  bool crossGrid{false};
  bool contiguityRequired{false};
  bool continuityRequired{false};
  std::int64_t guardBandMhz{0};

  ExclusionDomainId exclusionDomain{};

  Lease lease{};

  Instant createdAt{};
  Instant updatedAt{};
  Instant activatedAt{};
  Instant deactivatedAt{};
  Instant releasedAt{};
  Instant reclaimedAt{};

  // Generation of the capability the commit was evaluated against.
  CapabilityGeneration capabilityGeneration{};

  ControllerFence commitFence{};
  OperationGeneration lastOperation{};

  // Set when durable recovery found this reservation in a state that required
  // fresh physical evidence. Recovery never restores Active status.
  bool needsRevalidation{false};

  std::string detail;

  [[nodiscard]] bool isLiveAt(Instant now) const noexcept {
    return isLiveState(state) && lease.validAt(now);
  }
};

// Occupancy accounting for one spectrum domain at one instant.
struct SpectrumUsage {
  SpectrumDomainId domain{};
  SpectrumDomainGeneration domainGeneration{};
  ChannelGridId grid{};
  GridGeneration gridGeneration{};

  std::uint32_t totalSlots{0};
  std::uint32_t allocatableSlots{0};
  std::uint32_t liveSlots{0};
  std::uint32_t activeSlots{0};
  std::uint32_t reservedSlots{0};
  std::uint32_t freeSlots{0};
  std::uint32_t lapsedSlots{0};

  std::uint32_t liveReservations{0};
  std::uint32_t activeReservations{0};
  std::uint32_t lapsedReservations{0};
  std::uint32_t reclaimedReservations{0};
  std::uint32_t releasedReservations{0};

  // Free slots as explicit contiguous runs, in ascending order.
  std::vector<SlotRange> freeRuns;
};

}  // namespace wavelength_fabric
