#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/quantity.hpp"

// Candidate enumeration.
//
// Enumeration is read-only and deterministic: the same request against the
// same state produces the same ordered candidate list, including why each
// candidate was rejected. Enumerating never creates ownership.

namespace wavelength_fabric {

enum class CandidateEligibility : std::uint8_t {
  Eligible = 0,
  IneligibleUnsupported = 1,
  IneligibleUnknownCapability = 2,
  IneligibleStaleCapability = 3,
  IneligibleStaleDomain = 4,
  IneligibleOutsideWindow = 5,
  IneligibleOutsideTunable = 6,
  IneligibleExcluded = 7,
  IneligibleConflict = 8,
  IneligibleContiguity = 9,
  IneligibleConversionRequired = 10,
  IneligibleChannelWidth = 11,
  IneligibleGuardBand = 12,
  IneligibleNoGrid = 13,
  // Used when an eligibility token is not recognised. Never eligible.
  IneligibleUnknown = 14,
};

[[nodiscard]] std::string_view toToken(CandidateEligibility eligibility) noexcept;
[[nodiscard]] CandidateEligibility candidateEligibilityFromToken(std::string_view token) noexcept;
[[nodiscard]] bool isEligible(CandidateEligibility eligibility) noexcept;

struct SpectrumCandidate {
  std::size_t ordinal{0};
  SpectrumDomainId anchorDomain{};
  SlotRange slots{};
  FrequencyRange frequency{};
  CandidateEligibility eligibility{CandidateEligibility::Eligible};

  // Reservations that block this candidate. Populated for conflict rejections.
  std::vector<ReservationId> conflicts;

  // Slot range resolved on each spanned domain, parallel to the request domain
  // list. Identical to slots when every domain shares one grid.
  std::vector<SlotRange> perDomainSlots;
  bool crossGrid{false};

  std::string detail;

  [[nodiscard]] std::string describe() const;
};

struct CandidateSet {
  // Ok when enumeration completed. A non-Ok status means no candidate list was
  // produced at all (for example a structurally invalid request); the list is
  // empty in that case.
  Status status;
  std::vector<SpectrumCandidate> candidates;
  std::size_t eligibleCount{0};
  // Candidates that exist but were not retained because of the configured
  // bound. A non-zero value means the list is a prefix of the full set.
  std::size_t omitted{0};
  bool complete{true};
  std::string summary;

  [[nodiscard]] const SpectrumCandidate* firstEligible() const noexcept;
};

}  // namespace wavelength_fabric
