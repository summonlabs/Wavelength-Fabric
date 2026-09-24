#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/grid.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/quantity.hpp"

// Abstract optical spectrum resources.
//
// A span is an abstract conduit. A port is an abstract endpoint. A spectrum
// domain is the allocatable spectrum window that Wavelength Fabric governs on
// one span or between two ports. None of these types describe real hardware,
// and registering one asserts nothing about a physical deployment.

namespace wavelength_fabric {

enum class ResourceClass : std::uint8_t {
  Unknown = 0,  // rejected by validateDomain
  FiberSpan = 1,
  MediaChannel = 2,
  OpticalPort = 3,
  AmplifierBand = 4,
  AbstractDomain = 5,
};

[[nodiscard]] std::string_view toToken(ResourceClass klass) noexcept;
[[nodiscard]] ResourceClass resourceClassFromToken(std::string_view token) noexcept;

struct Span {
  SpanId id{};
  SpanGeneration generation{};
  std::string label;
};

struct OpticalPort {
  PortId id{};
  PortGeneration generation{};
  std::string label;
};

struct SpectrumDomain {
  SpectrumDomainId id{};
  SpectrumDomainGeneration generation{};
  ResourceClass klass{ResourceClass::AbstractDomain};

  // The grid this domain's spectrum is expressed in.
  ChannelGridId grid{};
  GridGeneration gridGeneration{};

  // Optional physical-ish anchors. These are opaque references supplied by the
  // caller; Wavelength Fabric never derives them.
  SpanId span{};
  SpanGeneration spanGeneration{};
  PortId portA{};
  PortGeneration portAGeneration{};
  PortId portB{};
  PortGeneration portBGeneration{};

  // A domain that requires contiguity cannot honour a multi-slot request that
  // asks for non-contiguous placement.
  bool requiresContiguity{true};

  std::string label;
};

[[nodiscard]] Status validateDomain(const SpectrumDomain& domain);
[[nodiscard]] Status validateSpan(const Span& span);
[[nodiscard]] Status validatePort(const OpticalPort& port);

// A conflict domain groups spectrum domains that must not hold overlapping
// authoritative frequency allocations, even across different media. It is the
// explicit model of "these resources are mutually exclusive".
struct ExclusionDomain {
  ExclusionDomainId id{};
  ExclusionDomainGeneration generation{};
  std::string label;
  std::vector<SpectrumDomainId> members;
  // Minimum separation enforced between frequency ranges owned by two
  // different members. Zero means exact overlap is required to conflict.
  std::int64_t guardBandMhz{0};
};

[[nodiscard]] Status validateExclusionDomain(const ExclusionDomain& domain);
[[nodiscard]] bool exclusionDomainContains(const ExclusionDomain& domain, SpectrumDomainId member) noexcept;

}  // namespace wavelength_fabric
