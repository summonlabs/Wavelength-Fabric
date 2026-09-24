#pragma once

#include <cstdint>
#include <string>

#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/resource.hpp"
#include "wavelength_fabric/time.hpp"

// Spectrum capability registration.
//
// A capability publication is a claim by a controller incarnation that a
// specific spectrum domain, at a specific domain generation, on a specific
// grid generation, either supports allocation or does not. UNSUPPORTED is a
// first-class published value: a request against an unsupported domain is
// refused with UNSUPPORTED and is never approximated with synthetic channels.
// UNKNOWN is never treated as SUPPORTED.

namespace wavelength_fabric {

enum class SpectrumSupport : std::uint8_t {
  Unsupported = 0,
  Supported = 1,
  Unknown = 2,
};

[[nodiscard]] std::string_view toToken(SpectrumSupport support) noexcept;
[[nodiscard]] SpectrumSupport supportFromToken(std::string_view token) noexcept;

// Opaque evidence supplied by the caller. The runtime never invents evidence:
// a claim that a domain supports a grid, or that conversion/regeneration is
// available, must carry a non-zero digest and a bounded source label.
struct CapabilityEvidence {
  bool present{false};
  std::uint64_t digest{0};
  std::string source;

  [[nodiscard]] bool usable() const noexcept { return present && digest != 0; }
};

struct SpectrumCapability {
  SpectrumDomainId domain{};
  SpectrumDomainGeneration domainGeneration{};
  ChannelGridId grid{};
  GridGeneration gridGeneration{};

  SpectrumSupport support{SpectrumSupport::Unknown};

  // Allocatable window inside the grid. Must be empty when support is
  // Unsupported or Unknown.
  std::uint32_t firstAllocatableSlot{0};
  std::uint32_t allocatableSlots{0};

  // Tunable window in absolute frequency. Must be a non-empty range inside the
  // grid's frequency span when support is Supported.
  std::int64_t minTunableMhz{0};
  std::int64_t maxTunableMhz{0};

  // When true, a channel on this domain must occupy contiguous slots. The
  // domain's own requirement is the intersection of this flag and the
  // domain's declared requiresContiguity.
  bool contiguityEnforced{true};

  // Conversion/regeneration across grids is never assumed. When
  // conversionSupported is true, conversionEvidence must carry a usable digest.
  bool conversionSupported{false};
  CapabilityEvidence conversionEvidence;

  // Evidence for the support claim itself.
  CapabilityEvidence presenceEvidence;

  CapabilityGeneration generation{};
  ControllerId publisher{};
  ControllerFence fence{};
  Instant publishedAt{};

  std::string detail;
};

[[nodiscard]] Status validateCapabilityShape(const SpectrumCapability& capability);

// The allocatable slot window of a live capability, as [first, first + count).
[[nodiscard]] SlotRange allocatableWindow(const SpectrumCapability& capability) noexcept;

[[nodiscard]] std::string describeCapability(const SpectrumCapability& capability);

}  // namespace wavelength_fabric
