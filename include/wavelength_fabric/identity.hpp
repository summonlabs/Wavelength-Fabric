#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

// Strongly typed identities, generations, epochs and incarnations.
//
// Every distinct authority domain has its own type. Two identities of
// different types never compare, convert, or substitute for one another, so a
// domain id can never be passed where a reservation id is required.
//
// The zero value is the "none" value for every type here and is never a valid
// identity that the runtime will accept on a mutating path.

namespace wavelength_fabric {

template <class Tag, class Rep = std::uint64_t>
class StrongValue {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongValue() noexcept = default;
  constexpr explicit StrongValue(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongValue fromRaw(Rep value) noexcept {
    return StrongValue(value);
  }

  [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }
  [[nodiscard]] constexpr bool none() const noexcept { return value_ == Rep{0}; }

  // Advances a monotonic generation/epoch counter. Callers are responsible for
  // never moving a durable counter backwards.
  constexpr void bump() noexcept { ++value_; }

  friend constexpr bool operator==(StrongValue, StrongValue) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(StrongValue, StrongValue) noexcept = default;

 private:
  Rep value_{0};
};

// ---------------------------------------------------------------------------
// Identity tags
// ---------------------------------------------------------------------------
struct SpectrumDomainIdTag;
struct SpanIdTag;
struct PortIdTag;
struct ChannelGridIdTag;
struct ExclusionDomainIdTag;
struct ReservationIdTag;
struct AllocationRequestIdTag;
struct OwnerIdTag;
struct ControllerIdTag;

// ---------------------------------------------------------------------------
// Generation tags
// ---------------------------------------------------------------------------
struct SpectrumDomainGenerationTag;
struct SpanGenerationTag;
struct PortGenerationTag;
struct GridGenerationTag;
struct ExclusionDomainGenerationTag;
struct ReservationGenerationTag;
struct AllocationRequestGenerationTag;
struct OwnerGenerationTag;
struct CapabilityGenerationTag;
struct PolicyGenerationTag;
struct PriorityGenerationTag;
struct LeaseGenerationTag;
struct OperationGenerationTag;
struct RecoveryGenerationTag;
struct RuntimeGenerationTag;
struct EligibilityAuthorityGenerationTag;
struct ReservationAuthorityGenerationTag;
struct ActivationAuthorityGenerationTag;
struct ReleaseAuthorityGenerationTag;
struct ControllerEpochTag;
struct ControllerIncarnationTag;
struct AuditSequenceTag;

// ---------------------------------------------------------------------------
// Concrete types
// ---------------------------------------------------------------------------
using SpectrumDomainId = StrongValue<SpectrumDomainIdTag>;
using SpanId = StrongValue<SpanIdTag>;
using PortId = StrongValue<PortIdTag>;
using ChannelGridId = StrongValue<ChannelGridIdTag>;
using ExclusionDomainId = StrongValue<ExclusionDomainIdTag>;
using ReservationId = StrongValue<ReservationIdTag>;
using AllocationRequestId = StrongValue<AllocationRequestIdTag>;
using OwnerId = StrongValue<OwnerIdTag>;
using ControllerId = StrongValue<ControllerIdTag>;

using SpectrumDomainGeneration = StrongValue<SpectrumDomainGenerationTag>;
using SpanGeneration = StrongValue<SpanGenerationTag>;
using PortGeneration = StrongValue<PortGenerationTag>;
using GridGeneration = StrongValue<GridGenerationTag>;
using ExclusionDomainGeneration = StrongValue<ExclusionDomainGenerationTag>;
using ReservationGeneration = StrongValue<ReservationGenerationTag>;
using AllocationRequestGeneration = StrongValue<AllocationRequestGenerationTag>;
using OwnerGeneration = StrongValue<OwnerGenerationTag>;
using CapabilityGeneration = StrongValue<CapabilityGenerationTag>;
using PolicyGeneration = StrongValue<PolicyGenerationTag>;
using PriorityGeneration = StrongValue<PriorityGenerationTag>;
using LeaseGeneration = StrongValue<LeaseGenerationTag>;
using OperationGeneration = StrongValue<OperationGenerationTag>;
using RecoveryGeneration = StrongValue<RecoveryGenerationTag>;
using RuntimeGeneration = StrongValue<RuntimeGenerationTag>;

using EligibilityAuthorityGeneration = StrongValue<EligibilityAuthorityGenerationTag>;
using ReservationAuthorityGeneration = StrongValue<ReservationAuthorityGenerationTag>;
using ActivationAuthorityGeneration = StrongValue<ActivationAuthorityGenerationTag>;
using ReleaseAuthorityGeneration = StrongValue<ReleaseAuthorityGenerationTag>;

// Epoch advances once per successful durable recovery; incarnation changes on
// every runtime start, including a restart that recovers no durable state.
using ControllerEpoch = StrongValue<ControllerEpochTag>;
using ControllerIncarnation = StrongValue<ControllerIncarnationTag>;
using AuditSequence = StrongValue<AuditSequenceTag, std::uint64_t>;

// Maximum representable epoch/incarnation; the runtime refuses to wrap.
inline constexpr std::uint64_t kMaxFenceValue = 0xFFFF'FFFF'FFFF'FFFEull;

// A fence is the (epoch, incarnation) pair that identifies exactly which
// controller boot is allowed to mutate authoritative ownership.
struct ControllerFence {
  ControllerEpoch epoch{};
  ControllerIncarnation incarnation{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return epoch.valid() && incarnation.valid();
  }
  [[nodiscard]] constexpr bool matches(const ControllerFence& other) const noexcept {
    return epoch == other.epoch && incarnation == other.incarnation;
  }
  friend constexpr bool operator==(const ControllerFence&, const ControllerFence&) noexcept = default;
};

// Renders a typed identity for audit records, CLI output and test diagnostics.
template <class Tag, class Rep>
[[nodiscard]] inline std::string typedToken(std::string_view prefix, StrongValue<Tag, Rep> value) {
  std::string out(prefix);
  out.push_back(':');
  out.append(std::to_string(value.raw()));
  return out;
}

}  // namespace wavelength_fabric
