#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Candidate enumeration.
//
// Enumeration is read-only and deterministic: identical inputs against identical
// state produce the same ordered list, every eligibility class is accounted for,
// and a truncated list reports complete=false with an exact omission count. The
// expectations below were derived from src/runtime_candidates.cpp and confirmed
// against the built library: the search window is the intersection of the
// allocatable window, the tunable window and the requested frequency windows;
// live reservations (and only live ones) cut free runs out of that window;
// candidates inside a blocked window are still reported so a refusal can name
// the reservation that owns the spectrum.

using namespace wavelength_fabric;
using namespace wf_test;

namespace {

template <class Enum>
[[nodiscard]] std::string token(Enum value) {
  return std::string(toToken(value));
}

const Instant kRequestedAt = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::hours(1);
constexpr std::int64_t kAnchor = 191'300'000;
constexpr std::int64_t kSlotWidth = 50'000;
constexpr std::int64_t kGridEnd = kAnchor + 96 * kSlotWidth;

[[nodiscard]] std::int64_t slotLow(std::uint32_t slot) {
  return kAnchor + static_cast<std::int64_t>(slot) * kSlotWidth;
}

[[nodiscard]] bool sameCandidate(const SpectrumCandidate& left, const SpectrumCandidate& right) {
  return left.ordinal == right.ordinal && left.anchorDomain == right.anchorDomain &&
         left.slots == right.slots && left.frequency == right.frequency &&
         left.eligibility == right.eligibility && left.conflicts == right.conflicts &&
         left.perDomainSlots == right.perDomainSlots && left.crossGrid == right.crossGrid &&
         left.detail == right.detail;
}

[[nodiscard]] bool sameCandidateList(const CandidateSet& left, const CandidateSet& right) {
  if (left.status.code != right.status.code) return false;
  if (left.summary != right.summary) return false;
  if (left.eligibleCount != right.eligibleCount) return false;
  if (left.omitted != right.omitted) return false;
  if (left.complete != right.complete) return false;
  if (left.candidates.size() != right.candidates.size()) return false;
  for (std::size_t index = 0; index < left.candidates.size(); ++index) {
    if (!sameCandidate(left.candidates[index], right.candidates[index])) return false;
  }
  return true;
}

[[nodiscard]] std::size_t countEligibility(const CandidateSet& set, CandidateEligibility eligibility) {
  std::size_t count = 0;
  for (const SpectrumCandidate& candidate : set.candidates) {
    if (candidate.eligibility == eligibility) ++count;
  }
  return count;
}

[[nodiscard]] bool ordinalsAreSequential(const CandidateSet& set) {
  for (std::size_t index = 0; index < set.candidates.size(); ++index) {
    if (set.candidates[index].ordinal != index) return false;
  }
  return true;
}

// One 96-slot flex grid (1..8 slots per channel, so multi-slot channels are
// representable) with domain 1 on it. The capability can be republished at a
// newer generation to move the allocatable window and the tunable range.
struct Rig {
  SpectrumRuntime runtime;
  ControllerFence fence;

  explicit Rig(RuntimeConfig config = RuntimeConfig{}) : runtime(std::move(config)) {
    (void)runtime.registerGrid(
        flexGrid(ChannelGridId(1), GridGeneration(1), kAnchor, kSlotWidth, 96, 1, 8));
    (void)runtime.registerDomain(makeDomain(SpectrumDomainId(1), ChannelGridId(1)));
    (void)runtime.publishCapability(makeCapability(
        SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, 0, 96, runtime.fence()));
    fence = runtime.fence();
  }

  Status publish(std::uint32_t first, std::uint32_t count, CapabilityGeneration generation,
                 std::int64_t minTunable = kAnchor, std::int64_t maxTunable = kGridEnd) {
    SpectrumCapability capability = makeCapability(
        SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, first, count, fence, ControllerId(1), generation);
    capability.minTunableMhz = minTunable;
    capability.maxTunableMhz = maxTunable;
    return runtime.publishCapability(capability);
  }

  [[nodiscard]] EligibilityAuthority eligibility() const {
    EligibilityAuthority token;
    token.generation = EligibilityAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] ReservationAuthority reservation() const {
    ReservationAuthority token;
    token.generation = ReservationAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] ReleaseAuthority releaseAuthority() const {
    ReleaseAuthority token;
    token.generation = ReleaseAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] SpectrumRequest request(AllocationRequestId requestId, std::uint32_t slots = 1,
                                        Instant requestedAt = kRequestedAt,
                                        Duration lease = kLease) const {
    return makeRequest(requestId, OwnerId(1), {SpectrumDomainId(1)},
                       {SpectrumDomainGeneration(1)}, ChannelGridId(1), GridGeneration(1), slots,
                       requestedAt, lease, eligibility(), reservation(),
                       slots > 1 ? ContiguityRequirement::Required
                                 : ContiguityRequirement::Unspecified);
  }
};

// A 96-slot anchor grid that can express up to 32 slots per channel plus a flex
// grid whose per-channel width bound is configurable, used for the cross-grid
// conversion rules.
struct CrossGridRig {
  SpectrumRuntime runtime;
  ControllerFence fence;

  explicit CrossGridRig(std::uint32_t maxSlotsPerChannel, bool conversionEvidence) {
    (void)runtime.registerGrid(
        flexGrid(ChannelGridId(1), GridGeneration(1), kAnchor, kSlotWidth, 96, 1, 32));
    (void)runtime.registerGrid(flexGrid(ChannelGridId(2), GridGeneration(1), kAnchor, 12'500, 384, 1,
                                        maxSlotsPerChannel));
    (void)runtime.registerDomain(makeDomain(SpectrumDomainId(1), ChannelGridId(1)));
    (void)runtime.registerDomain(makeDomain(SpectrumDomainId(2), ChannelGridId(2)));
    SpectrumCapability anchor = makeCapability(
        SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, 0, 96, runtime.fence());
    SpectrumCapability converted = makeCapability(
        SpectrumDomainId(2), ChannelGridId(2), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, 0, 384, runtime.fence());
    converted.minTunableMhz = kAnchor;
    converted.maxTunableMhz = kAnchor + 384 * 12'500;
    if (conversionEvidence) {
      anchor.conversionSupported = true;
      anchor.conversionEvidence = CapabilityEvidence{true, 11, "anchor-evidence"};
      converted.conversionSupported = true;
      converted.conversionEvidence = CapabilityEvidence{true, 22, "converted-evidence"};
    }
    (void)runtime.publishCapability(anchor);
    (void)runtime.publishCapability(converted);
    fence = runtime.fence();
  }

  [[nodiscard]] EligibilityAuthority eligibility() const {
    EligibilityAuthority token;
    token.generation = EligibilityAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] ReservationAuthority reservation() const {
    ReservationAuthority token;
    token.generation = ReservationAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] SpectrumRequest request(AllocationRequestId requestId, std::uint32_t slots) const {
    return makeRequest(AllocationRequestId(requestId), OwnerId(1),
                       {SpectrumDomainId(1), SpectrumDomainId(2)},
                       {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)},
                       ChannelGridId(1), GridGeneration(1), slots, kRequestedAt, kLease,
                       eligibility(), reservation(),
                       slots > 1 ? ContiguityRequirement::Required
                                 : ContiguityRequirement::Unspecified,
                       ContinuityRequirement::Required);
  }
};

// One grid of the caller's choosing with domain 1 on it and a full-window
// capability, so a test can pin the per-channel width bound of the request grid
// itself. capabilityStatus records the publication result: a grid that cannot
// express a single-slot channel cannot carry a supported capability at all.
struct BoundRig {
  SpectrumRuntime runtime;
  ControllerFence fence;
  Status capabilityStatus;

  explicit BoundRig(const ChannelGrid& grid) {
    (void)runtime.registerGrid(grid);
    (void)runtime.registerDomain(
        makeDomain(SpectrumDomainId(1), grid.id, SpectrumDomainGeneration(1), grid.generation));
    SpectrumCapability capability = makeCapability(
        SpectrumDomainId(1), grid.id, SpectrumDomainGeneration(1), grid.generation,
        SpectrumSupport::Supported, 0, grid.slotCount, runtime.fence());
    capability.minTunableMhz = grid.anchorMhz;
    capability.maxTunableMhz =
        grid.anchorMhz + grid.slotWidthMhz * static_cast<std::int64_t>(grid.slotCount);
    capabilityStatus = runtime.publishCapability(capability);
    fence = runtime.fence();
  }

  [[nodiscard]] EligibilityAuthority eligibility() const {
    EligibilityAuthority token;
    token.generation = EligibilityAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] ReservationAuthority reservation() const {
    ReservationAuthority token;
    token.generation = ReservationAuthorityGeneration(1);
    token.fence = fence;
    return token;
  }

  [[nodiscard]] SpectrumRequest request(AllocationRequestId requestId, std::uint32_t slots) const {
    return makeRequest(requestId, OwnerId(1), {SpectrumDomainId(1)},
                       {SpectrumDomainGeneration(1)}, ChannelGridId(1), GridGeneration(1), slots,
                       kRequestedAt, kLease, eligibility(), reservation(),
                       slots > 1 ? ContiguityRequirement::Required
                                 : ContiguityRequirement::Unspecified);
  }
};

}  // namespace

WF_TEST(enumeration_is_deterministic_and_canonically_ordered) {
  Rig first;
  Rig second;
  const SpectrumRequest request = first.request(AllocationRequestId(1));

  const CandidateSet left = first.runtime.enumerateCandidates(request);
  const CandidateSet right = first.runtime.enumerateCandidates(request);
  const CandidateSet elsewhere = second.runtime.enumerateCandidates(request);

  WF_CHECK(sameCandidateList(left, right));
  WF_CHECK(sameCandidateList(left, elsewhere));
  WF_CHECK(left.status.ok());
  WF_CHECK(ordinalsAreSequential(left));

  WF_CHECK_EQ(left.candidates.size(), std::size_t{96});
  WF_CHECK_EQ(left.eligibleCount, std::size_t{96});
  WF_CHECK_EQ(left.omitted, std::size_t{0});
  WF_CHECK(left.complete);
  WF_CHECK(!left.summary.empty());

  for (std::size_t index = 0; index < left.candidates.size(); ++index) {
    const SpectrumCandidate& candidate = left.candidates[index];
    const std::uint32_t slot = static_cast<std::uint32_t>(index);
    WF_CHECK_EQ(candidate.ordinal, index);
    WF_CHECK_EQ(candidate.anchorDomain.raw(), std::uint64_t{1});
    WF_CHECK_EQ(candidate.slots.first, slot);
    WF_CHECK_EQ(candidate.slots.count, std::uint32_t{1});
    WF_CHECK_EQ(candidate.frequency.lowMhz, slotLow(slot));
    WF_CHECK_EQ(candidate.frequency.highMhz, slotLow(slot) + kSlotWidth);
    WF_CHECK(candidate.eligibility == CandidateEligibility::Eligible);
    WF_CHECK(candidate.conflicts.empty());
    WF_CHECK(!candidate.crossGrid);
    WF_CHECK_EQ(candidate.perDomainSlots.size(), std::size_t{1});
    WF_CHECK(candidate.perDomainSlots[0] == candidate.slots);
    WF_CHECK_EQ(candidate.detail, std::string("eligible"));
  }

  // Enumeration never creates ownership and never touches the accounting.
  WF_CHECK(first.runtime.reservations().empty());
  WF_CHECK_EQ(first.runtime.stats().candidatesEvaluated, std::uint64_t{0});
  WF_CHECK_EQ(first.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(first.runtime.stats().enumerationTruncations, std::uint64_t{0});
}

WF_TEST(allocatable_window_and_tunable_range_bound_the_search) {
  Rig rig;
  const SpectrumRequest request = rig.request(AllocationRequestId(1));

  WF_REQUIRE(rig.publish(10, 10, CapabilityGeneration(2)).ok());
  const CandidateSet windowed = rig.runtime.enumerateCandidates(request);
  WF_REQUIRE(windowed.candidates.size() == std::size_t{10});
  WF_CHECK_EQ(windowed.eligibleCount, std::size_t{10});
  WF_CHECK_EQ(windowed.candidates.front().slots.first, std::uint32_t{10});
  WF_CHECK_EQ(windowed.candidates.back().slots.first, std::uint32_t{19});
  WF_CHECK(windowed.complete);

  WF_REQUIRE(rig.publish(0, 96, CapabilityGeneration(3), kAnchor + 5 * kSlotWidth,
                         kAnchor + 10 * kSlotWidth).ok());
  const CandidateSet tunable = rig.runtime.enumerateCandidates(request);
  WF_REQUIRE(tunable.candidates.size() == std::size_t{5});
  WF_CHECK_EQ(tunable.eligibleCount, std::size_t{5});
  WF_CHECK_EQ(tunable.candidates.front().slots.first, std::uint32_t{5});
  WF_CHECK_EQ(tunable.candidates.back().slots.first, std::uint32_t{9});

  // The window is the intersection of both bounds.
  WF_REQUIRE(rig.publish(4, 10, CapabilityGeneration(4), kAnchor + 5 * kSlotWidth,
                         kAnchor + 10 * kSlotWidth).ok());
  const CandidateSet intersected = rig.runtime.enumerateCandidates(request);
  WF_REQUIRE(intersected.candidates.size() == std::size_t{5});
  WF_CHECK_EQ(intersected.candidates.front().slots.first, std::uint32_t{5});
  WF_CHECK_EQ(intersected.candidates.back().slots.first, std::uint32_t{9});

  // A tunable range that fully covers no slot leaves nothing to allocate, and a
  // request against it is refused as unsupported rather than approximated.
  WF_REQUIRE(rig.publish(0, 96, CapabilityGeneration(5), kAnchor + 49'999, kAnchor + 50'000).ok());
  const CandidateSet subSlot = rig.runtime.enumerateCandidates(request);
  WF_CHECK(subSlot.status.code == StatusCode::Unsupported);
  WF_CHECK(subSlot.candidates.empty());
  WF_CHECK_EQ(subSlot.eligibleCount, std::size_t{0});
  WF_CHECK(!subSlot.complete);
  const AllocationDecision decision = rig.runtime.allocate(request);
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(decision.status.code == StatusCode::Unsupported);
  WF_CHECK(rig.runtime.reservations().empty());
}

WF_TEST(request_frequency_windows_narrow_the_search) {
  Rig rig;
  const SpectrumRequest baseline = rig.request(AllocationRequestId(1));

  SpectrumRequest single = baseline;
  single.frequencyWindows.push_back(FrequencyRange{slotLow(4), slotLow(6)});
  const CandidateSet oneWindow = rig.runtime.enumerateCandidates(single);
  WF_REQUIRE(oneWindow.candidates.size() == std::size_t{2});
  WF_CHECK_EQ(oneWindow.eligibleCount, std::size_t{2});
  WF_CHECK_EQ(oneWindow.candidates[0].slots.first, std::uint32_t{4});
  WF_CHECK_EQ(oneWindow.candidates[1].slots.first, std::uint32_t{5});
  WF_CHECK_EQ(oneWindow.candidates[1].frequency.highMhz, slotLow(6));
  WF_CHECK(oneWindow.complete);
  WF_CHECK_EQ(oneWindow.omitted, std::size_t{0});

  SpectrumRequest overlapping = baseline;
  overlapping.frequencyWindows.push_back(FrequencyRange{slotLow(1), slotLow(4)});
  overlapping.frequencyWindows.push_back(FrequencyRange{slotLow(3), slotLow(6)});
  const CandidateSet merged = rig.runtime.enumerateCandidates(overlapping);
  WF_REQUIRE(merged.candidates.size() == std::size_t{5});
  WF_CHECK_EQ(merged.candidates.front().slots.first, std::uint32_t{1});
  WF_CHECK_EQ(merged.candidates.back().slots.first, std::uint32_t{5});

  SpectrumRequest disjoint = baseline;
  disjoint.frequencyWindows.push_back(FrequencyRange{slotLow(0), slotLow(2)});
  disjoint.frequencyWindows.push_back(FrequencyRange{slotLow(50), slotLow(52)});
  const CandidateSet separated = rig.runtime.enumerateCandidates(disjoint);
  WF_REQUIRE(separated.candidates.size() == std::size_t{4});
  WF_CHECK_EQ(separated.candidates[0].slots.first, std::uint32_t{0});
  WF_CHECK_EQ(separated.candidates[1].slots.first, std::uint32_t{1});
  WF_CHECK_EQ(separated.candidates[2].slots.first, std::uint32_t{50});
  WF_CHECK_EQ(separated.candidates[3].slots.first, std::uint32_t{51});

  // A window that covers no whole slot yields an empty but complete result: the
  // search ran to the end and found nothing rather than being cut short.
  SpectrumRequest halfSlot = baseline;
  halfSlot.frequencyWindows.push_back(FrequencyRange{kAnchor + 25'000, kAnchor + 50'000});
  const CandidateSet empty = rig.runtime.enumerateCandidates(halfSlot);
  WF_CHECK(empty.status.ok());
  WF_CHECK(empty.candidates.empty());
  WF_CHECK_EQ(empty.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(empty.omitted, std::size_t{0});
  WF_CHECK(empty.complete);
  const AllocationDecision none = rig.runtime.allocate(halfSlot);
  WF_CHECK(none.outcome == AllocationOutcome::RefusedNoCapacity);
  WF_CHECK(none.status.code == StatusCode::Unavailable);
  WF_CHECK(rig.runtime.reservations().empty());

  SpectrumRequest beyondGrid = baseline;
  beyondGrid.frequencyWindows.push_back(FrequencyRange{kGridEnd, kGridEnd + 1'000});
  const CandidateSet outside = rig.runtime.enumerateCandidates(beyondGrid);
  WF_CHECK(outside.status.ok());
  WF_CHECK(outside.candidates.empty());
  WF_CHECK(outside.complete);

  SpectrumRequest beforeAnchor = baseline;
  beforeAnchor.frequencyWindows.push_back(FrequencyRange{kAnchor - 10, kAnchor - 5});
  const CandidateSet earlier = rig.runtime.enumerateCandidates(beforeAnchor);
  WF_CHECK(earlier.status.ok());
  WF_CHECK(earlier.candidates.empty());
  WF_CHECK(earlier.complete);

  // An empty window is a malformed request, not an empty search.
  SpectrumRequest malformed = baseline;
  malformed.frequencyWindows.push_back(FrequencyRange{kAnchor, kAnchor});
  const CandidateSet rejected = rig.runtime.enumerateCandidates(malformed);
  WF_CHECK(rejected.status.code == StatusCode::InvalidArgument);
  WF_CHECK(rejected.candidates.empty());
  WF_CHECK(!rejected.complete);
}

WF_TEST(excluded_slots_and_frequencies_are_accounted_per_candidate) {
  Rig rig;
  const SpectrumRequest baseline = rig.request(AllocationRequestId(1));

  SpectrumRequest excludedSlots = baseline;
  excludedSlots.constraints.excludedSlots.push_back(SlotRange{0, 3});
  excludedSlots.constraints.excludedSlots.push_back(SlotRange{94, 2});
  const CandidateSet slotSet = rig.runtime.enumerateCandidates(excludedSlots);
  WF_REQUIRE(slotSet.candidates.size() == std::size_t{96});
  WF_CHECK_EQ(slotSet.eligibleCount, std::size_t{91});
  WF_CHECK_EQ(slotSet.omitted, std::size_t{0});
  WF_CHECK(slotSet.complete);
  WF_CHECK_EQ(countEligibility(slotSet, CandidateEligibility::IneligibleExcluded), std::size_t{5});
  WF_CHECK(slotSet.candidates[0].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(slotSet.candidates[1].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(slotSet.candidates[2].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(slotSet.candidates[3].eligibility == CandidateEligibility::Eligible);
  WF_CHECK(slotSet.candidates[94].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(slotSet.candidates[95].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(!slotSet.candidates[0].detail.empty());

  SpectrumRequest excludedFrequencies = baseline;
  excludedFrequencies.constraints.excludedFrequencies.push_back(
      FrequencyRange{slotLow(3), slotLow(5)});
  const CandidateSet frequencySet = rig.runtime.enumerateCandidates(excludedFrequencies);
  WF_REQUIRE(frequencySet.candidates.size() == std::size_t{96});
  WF_CHECK_EQ(frequencySet.eligibleCount, std::size_t{94});
  WF_CHECK(frequencySet.complete);
  WF_CHECK(frequencySet.candidates[2].eligibility == CandidateEligibility::Eligible);
  WF_CHECK(frequencySet.candidates[3].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(frequencySet.candidates[4].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(frequencySet.candidates[5].eligibility == CandidateEligibility::Eligible);

  // An excluded range that only touches a candidate does not exclude it.
  SpectrumRequest abutting = baseline;
  abutting.constraints.excludedFrequencies.push_back(FrequencyRange{slotLow(3), slotLow(4)});
  const CandidateSet touching = rig.runtime.enumerateCandidates(abutting);
  WF_CHECK_EQ(touching.eligibleCount, std::size_t{95});
  WF_CHECK(touching.candidates[3].eligibility == CandidateEligibility::IneligibleExcluded);
  WF_CHECK(touching.candidates[4].eligibility == CandidateEligibility::Eligible);

  // Excluding every slot refuses the request with the constraint outcome and
  // never produces a synthetic channel.
  SpectrumRequest everything = baseline;
  everything.constraints.excludedSlots.push_back(SlotRange{0, 96});
  const CandidateSet all = rig.runtime.enumerateCandidates(everything);
  WF_CHECK_EQ(all.candidates.size(), std::size_t{96});
  WF_CHECK_EQ(all.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(all.omitted, std::size_t{0});
  WF_CHECK(all.complete);
  const AllocationDecision decision = rig.runtime.allocate(everything);
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedConstraint);
  WF_CHECK(decision.status.code == StatusCode::Refused);
  WF_CHECK(decision.explanation.conflicts.empty());
  WF_CHECK(decision.explanation.selected.slots.empty());
  WF_CHECK(rig.runtime.reservations().empty());

  // An unknown reservation named in mustNotConflictWith is refused before any
  // candidate is produced.
  SpectrumRequest unknownConflict = baseline;
  unknownConflict.constraints.mustNotConflictWith.push_back(ReservationId(1));
  const CandidateSet missing = rig.runtime.enumerateCandidates(unknownConflict);
  WF_CHECK(missing.status.code == StatusCode::NotFound);
  WF_CHECK(missing.candidates.empty());
  WF_CHECK(!missing.complete);
}

WF_TEST(channel_width_limits_a_converted_domain) {
  CrossGridRig narrow(4, true);
  const SpectrumRequest wide = narrow.request(AllocationRequestId(1), 8);
  const CandidateSet rejected = narrow.runtime.enumerateCandidates(wide);
  WF_CHECK(rejected.status.ok());
  WF_REQUIRE(rejected.candidates.size() == std::size_t{89});
  WF_CHECK_EQ(rejected.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(rejected.omitted, std::size_t{0});
  WF_CHECK(rejected.complete);
  WF_CHECK_EQ(countEligibility(rejected, CandidateEligibility::IneligibleConversionRequired),
              std::size_t{89});
  WF_CHECK(rejected.candidates.front().crossGrid);
  WF_CHECK(rejected.candidates.front().perDomainSlots[1].empty());
  WF_CHECK_EQ(rejected.candidates.front().slots.first, std::uint32_t{0});
  WF_CHECK_EQ(rejected.candidates.front().slots.count, std::uint32_t{8});
  const AllocationDecision refused = narrow.runtime.allocate(wide);
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedConversion);
  WF_CHECK(refused.status.code == StatusCode::NotPermitted);
  WF_CHECK(narrow.runtime.reservations().empty());

  // One slot fits the converted grid, so the same request shape succeeds.
  const SpectrumRequest narrowRequest = narrow.request(AllocationRequestId(2), 1);
  const CandidateSet single = narrow.runtime.enumerateCandidates(narrowRequest);
  WF_REQUIRE(single.candidates.size() == std::size_t{96});
  WF_CHECK_EQ(single.eligibleCount, std::size_t{96});
  WF_CHECK_EQ(single.candidates[0].perDomainSlots[1].first, std::uint32_t{0});
  WF_CHECK_EQ(single.candidates[0].perDomainSlots[1].count, std::uint32_t{4});

  CrossGridRig wideEnough(32, true);
  const SpectrumRequest wideRequest = wideEnough.request(AllocationRequestId(1), 8);
  const CandidateSet accepted = wideEnough.runtime.enumerateCandidates(wideRequest);
  WF_CHECK(accepted.status.ok());
  WF_REQUIRE(accepted.candidates.size() == std::size_t{89});
  WF_CHECK_EQ(accepted.eligibleCount, std::size_t{89});
  WF_CHECK(accepted.complete);
  WF_CHECK(accepted.candidates.front().crossGrid);
  WF_CHECK_EQ(accepted.candidates.front().perDomainSlots.size(), std::size_t{2});
  WF_CHECK(accepted.candidates.front().perDomainSlots[0] == accepted.candidates.front().slots);
  WF_CHECK_EQ(accepted.candidates.front().perDomainSlots[1].first, std::uint32_t{0});
  WF_CHECK_EQ(accepted.candidates.front().perDomainSlots[1].count, std::uint32_t{32});
  WF_CHECK_EQ(accepted.candidates.back().slots.first, std::uint32_t{88});
  WF_CHECK_EQ(accepted.candidates.back().perDomainSlots[1].first, std::uint32_t{352});
  WF_CHECK_EQ(accepted.candidates.back().perDomainSlots[1].count, std::uint32_t{32});
  const AllocationDecision allocated = wideEnough.runtime.allocate(wideRequest);
  WF_CHECK(allocated.allocated());
  const std::optional<SpectrumReservation> reservation =
      wideEnough.runtime.reservation(allocated.reservation);
  WF_REQUIRE(reservation.has_value());
  WF_CHECK_EQ(reservation->perDomainSlots.size(), std::size_t{2});
  WF_CHECK(reservation->crossGrid);
  WF_CHECK_EQ(reservation->perDomainSlots[1].count, std::uint32_t{32});

  // Crossing grids without conversion evidence is refused up front; no
  // candidate is invented for the converted domain.
  CrossGridRig noEvidence(32, false);
  const SpectrumRequest unproven = noEvidence.request(AllocationRequestId(1), 1);
  const CandidateSet evidence = noEvidence.runtime.enumerateCandidates(unproven);
  WF_CHECK(evidence.status.code == StatusCode::NotPermitted);
  WF_CHECK(evidence.candidates.empty());
  WF_CHECK(!evidence.complete);
  const AllocationDecision denied = noEvidence.runtime.allocate(unproven);
  WF_CHECK(denied.outcome == AllocationOutcome::RefusedConversion);
  WF_CHECK(denied.status.code == StatusCode::NotPermitted);
  WF_CHECK(noEvidence.runtime.reservations().empty());
}

WF_TEST(live_reservations_are_separated_by_guard_bands) {
  Rig rig;
  const AllocationDecision owner = rig.runtime.allocate(rig.request(AllocationRequestId(1), 4));
  WF_REQUIRE(owner.allocated());
  const std::optional<SpectrumReservation> owned = rig.runtime.reservation(owner.reservation);
  WF_REQUIRE(owned.has_value());
  WF_CHECK_EQ(owned->slots.first, std::uint32_t{0});
  WF_CHECK_EQ(owned->slots.count, std::uint32_t{4});
  WF_CHECK(owned->isLiveAt(kRequestedAt));

  SpectrumRequest abutting = rig.request(AllocationRequestId(2));
  abutting.guardBandMhz = 0;
  const CandidateSet zero = rig.runtime.enumerateCandidates(abutting);
  WF_REQUIRE(zero.candidates.size() == std::size_t{92});
  WF_CHECK_EQ(zero.eligibleCount, std::size_t{92});
  WF_CHECK_EQ(zero.candidates.front().slots.first, std::uint32_t{4});
  WF_CHECK_EQ(zero.candidates.back().slots.first, std::uint32_t{95});
  WF_CHECK(zero.complete);
  WF_CHECK_EQ(zero.omitted, std::size_t{0});
  WF_CHECK_EQ(zero.candidates.front().frequency.lowMhz, slotLow(4));
  WF_CHECK(zero.candidates.front().frequency.lowMhz == owned->frequency.highMhz);

  SpectrumRequest oneMhz = abutting;
  oneMhz.guardBandMhz = 1;
  const CandidateSet tiny = rig.runtime.enumerateCandidates(oneMhz);
  WF_REQUIRE(tiny.candidates.size() == std::size_t{91});
  WF_CHECK_EQ(tiny.candidates.front().slots.first, std::uint32_t{5});

  SpectrumRequest halfSlot = abutting;
  halfSlot.guardBandMhz = kSlotWidth;
  const CandidateSet guarded = rig.runtime.enumerateCandidates(halfSlot);
  WF_REQUIRE(guarded.candidates.size() == std::size_t{91});
  WF_CHECK_EQ(guarded.candidates.front().slots.first, std::uint32_t{5});
  WF_CHECK_EQ(guarded.candidates.front().frequency.lowMhz, slotLow(5));

  SpectrumRequest fullSlot = abutting;
  fullSlot.guardBandMhz = 2 * kSlotWidth;
  const CandidateSet wide = rig.runtime.enumerateCandidates(fullSlot);
  WF_REQUIRE(wide.candidates.size() == std::size_t{90});
  WF_CHECK_EQ(wide.candidates.front().slots.first, std::uint32_t{6});

  SpectrumRequest huge = abutting;
  huge.guardBandMhz = kMaxGuardBandMhz;
  const CandidateSet far = rig.runtime.enumerateCandidates(huge);
  WF_REQUIRE(far.candidates.size() == std::size_t{72});
  WF_CHECK_EQ(far.candidates.front().slots.first, std::uint32_t{24});
  WF_CHECK(far.complete);

  // The reservation's own guard band applies in the other direction too.
  Rig reciprocal;
  SpectrumRequest guardedOwner = reciprocal.request(AllocationRequestId(1), 4);
  guardedOwner.guardBandMhz = 2 * kSlotWidth;
  const AllocationDecision guardedDecision = reciprocal.runtime.allocate(guardedOwner);
  WF_REQUIRE(guardedDecision.allocated());
  const std::optional<SpectrumReservation> stored =
      reciprocal.runtime.reservation(guardedDecision.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK_EQ(stored->guardBandMhz, std::int64_t{2 * kSlotWidth});
  const CandidateSet reciprocalSet =
      reciprocal.runtime.enumerateCandidates(reciprocal.request(AllocationRequestId(2)));
  WF_REQUIRE(reciprocalSet.candidates.size() == std::size_t{90});
  WF_CHECK_EQ(reciprocalSet.candidates.front().slots.first, std::uint32_t{6});

  // A multi-slot request still fits beside the owned range.
  const SpectrumRequest pair = rig.request(AllocationRequestId(3), 2);
  const CandidateSet pairs = rig.runtime.enumerateCandidates(pair);
  WF_REQUIRE(pairs.candidates.size() == std::size_t{91});
  WF_CHECK_EQ(pairs.eligibleCount, std::size_t{91});
  WF_CHECK_EQ(pairs.candidates.front().slots.first, std::uint32_t{4});
  WF_CHECK_EQ(pairs.candidates.back().slots.first, std::uint32_t{94});
  const AllocationDecision second = rig.runtime.allocate(pair);
  WF_CHECK(second.allocated());
  const std::optional<SpectrumReservation> committed = rig.runtime.reservation(second.reservation);
  WF_REQUIRE(committed.has_value());
  WF_CHECK_EQ(committed->slots.first, std::uint32_t{4});
  WF_CHECK_EQ(committed->slots.count, std::uint32_t{2});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{2});
}

WF_TEST(conflicting_candidates_are_reported_with_their_owner) {
  Rig rig;
  WF_REQUIRE(rig.publish(0, 1, CapabilityGeneration(2)).ok());
  const AllocationDecision owner = rig.runtime.allocate(rig.request(AllocationRequestId(1)));
  WF_REQUIRE(owner.allocated());

  const SpectrumRequest blocked = rig.request(AllocationRequestId(2));
  const CandidateSet set = rig.runtime.enumerateCandidates(blocked);
  WF_CHECK(set.status.ok());
  WF_REQUIRE(set.candidates.size() == std::size_t{1});
  WF_CHECK_EQ(set.eligibleCount, std::size_t{0});
  // A diagnostic candidate is reported, not omitted: omitted counts only the
  // starts the configured bound cut off.
  WF_CHECK_EQ(set.omitted, std::size_t{0});
  WF_CHECK(set.complete);
  WF_CHECK(set.candidates[0].eligibility == CandidateEligibility::IneligibleConflict);
  WF_CHECK_EQ(set.candidates[0].slots.first, std::uint32_t{0});
  WF_CHECK_EQ(set.candidates[0].slots.count, std::uint32_t{1});
  WF_CHECK_EQ(set.candidates[0].conflicts.size(), std::size_t{1});
  WF_CHECK_EQ(set.candidates[0].conflicts[0].raw(), owner.reservation.raw());
  WF_CHECK_EQ(set.candidates[0].perDomainSlots.size(), std::size_t{1});
  WF_CHECK(!set.candidates[0].detail.empty());

  const AllocationDecision refused = rig.runtime.allocate(blocked);
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedConflict);
  WF_CHECK(refused.status.code == StatusCode::Conflict);
  WF_CHECK_EQ(refused.explanation.candidatesEnumerated, std::size_t{1});
  WF_CHECK_EQ(refused.explanation.candidatesRejected, std::size_t{1});
  WF_CHECK_EQ(refused.explanation.candidatesOmitted, std::size_t{0});
  WF_CHECK_EQ(refused.explanation.conflicts.size(), std::size_t{1});
  WF_CHECK_EQ(refused.explanation.conflicts[0].raw(), owner.reservation.raw());
  WF_CHECK(!refused.explanation.reasons.empty());
  WF_CHECK(refused.explanation.selected.slots.empty());
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{1});

  // A blocked window that cannot hold the requested channel produces no
  // candidate at all: one free slot remains in a two-slot window, and nothing
  // is invented for the rest.
  WF_REQUIRE(rig.publish(0, 2, CapabilityGeneration(3)).ok());
  const SpectrumRequest wider = rig.request(AllocationRequestId(3), 2);
  const CandidateSet cramped = rig.runtime.enumerateCandidates(wider);
  WF_CHECK(cramped.status.ok());
  WF_CHECK(cramped.candidates.empty());
  WF_CHECK_EQ(cramped.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(cramped.omitted, std::size_t{0});
  WF_CHECK(cramped.complete);
  const AllocationDecision crampedRefused = rig.runtime.allocate(wider);
  WF_CHECK(crampedRefused.outcome == AllocationOutcome::RefusedNoCapacity);
  WF_CHECK(crampedRefused.status.code == StatusCode::Unavailable);
  WF_CHECK(rig.runtime.reservations().size() == std::size_t{1});

  // When the whole window is blocked and wide enough for the channel, the
  // refusal names the reservation that owns the spectrum.
  Rig pairs;
  WF_REQUIRE(pairs.publish(0, 2, CapabilityGeneration(2)).ok());
  const AllocationDecision pairOwner = pairs.runtime.allocate(pairs.request(AllocationRequestId(1), 2));
  WF_REQUIRE(pairOwner.allocated());
  const SpectrumRequest pairBlocked = pairs.request(AllocationRequestId(2), 2);
  const CandidateSet pair = pairs.runtime.enumerateCandidates(pairBlocked);
  WF_REQUIRE(pair.candidates.size() == std::size_t{1});
  WF_CHECK_EQ(pair.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(pair.omitted, std::size_t{0});
  WF_CHECK(pair.complete);
  WF_CHECK_EQ(pair.candidates[0].slots.first, std::uint32_t{0});
  WF_CHECK_EQ(pair.candidates[0].slots.count, std::uint32_t{2});
  WF_CHECK(pair.candidates[0].eligibility == CandidateEligibility::IneligibleConflict);
  WF_CHECK_EQ(pair.candidates[0].conflicts.size(), std::size_t{1});
  WF_CHECK_EQ(pair.candidates[0].conflicts[0].raw(), pairOwner.reservation.raw());
  const AllocationDecision pairRefused = pairs.runtime.allocate(pairBlocked);
  WF_CHECK(pairRefused.outcome == AllocationOutcome::RefusedConflict);
  WF_CHECK(pairRefused.status.code == StatusCode::Conflict);
  WF_CHECK_EQ(pairRefused.explanation.conflicts.size(), std::size_t{1});
  WF_CHECK(pairs.runtime.reservations().size() == std::size_t{1});
}

WF_TEST(truncated_candidate_sets_report_exact_omissions) {
  RuntimeConfig config;
  config.maxCandidatesPerRequest = 8;
  Rig rig(config);
  const SpectrumRequest full = rig.request(AllocationRequestId(1));

  const CandidateSet truncated = rig.runtime.enumerateCandidates(full);
  WF_CHECK(truncated.status.ok());
  WF_REQUIRE(truncated.candidates.size() == std::size_t{8});
  WF_CHECK_EQ(truncated.eligibleCount, std::size_t{8});
  WF_CHECK_EQ(truncated.omitted, std::size_t{88});
  WF_CHECK(!truncated.complete);
  WF_CHECK(ordinalsAreSequential(truncated));
  for (std::size_t index = 0; index < truncated.candidates.size(); ++index) {
    WF_CHECK_EQ(truncated.candidates[index].slots.first, static_cast<std::uint32_t>(index));
  }

  // A second identical call is byte-for-byte identical and still read-only.
  WF_CHECK(sameCandidateList(truncated, rig.runtime.enumerateCandidates(full)));
  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().enumerationTruncations, std::uint64_t{0});

  const DecisionExplanation explained = rig.runtime.explain(full);
  WF_CHECK_EQ(explained.candidatesEnumerated, std::size_t{8});
  WF_CHECK_EQ(explained.candidatesOmitted, std::size_t{88});
  WF_CHECK(explained.outcome == AllocationOutcome::Allocated);
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().enumerationTruncations, std::uint64_t{0});

  const AllocationDecision allocated = rig.runtime.allocate(full);
  WF_CHECK(allocated.allocated());
  WF_CHECK_EQ(allocated.explanation.candidatesEnumerated, std::size_t{8});
  WF_CHECK_EQ(allocated.explanation.candidatesOmitted, std::size_t{88});
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{8});
  WF_CHECK_EQ(rig.runtime.stats().enumerationTruncations, std::uint64_t{1});

  // A reservation in the middle splits the free space into two runs; the
  // omission count is the number of starts the truncated prefix did not reach.
  RuntimeConfig holeConfig;
  holeConfig.maxCandidatesPerRequest = 8;
  Rig holed(holeConfig);
  SpectrumRequest hole = holed.request(AllocationRequestId(1), 2);
  hole.frequencyWindows.push_back(FrequencyRange{slotLow(2), slotLow(6)});
  const AllocationDecision placed = holed.runtime.allocate(hole);
  WF_REQUIRE(placed.allocated());
  const std::optional<SpectrumReservation> reserved = holed.runtime.reservation(placed.reservation);
  WF_REQUIRE(reserved.has_value());
  WF_CHECK_EQ(reserved->slots.first, std::uint32_t{2});
  WF_CHECK_EQ(reserved->slots.count, std::uint32_t{2});

  const CandidateSet split = holed.runtime.enumerateCandidates(holed.request(AllocationRequestId(2)));
  WF_CHECK(split.status.ok());
  WF_REQUIRE(split.candidates.size() == std::size_t{8});
  WF_CHECK_EQ(split.eligibleCount, std::size_t{8});
  WF_CHECK_EQ(split.omitted, std::size_t{86});
  WF_CHECK(!split.complete);
  const std::vector<std::uint32_t> expectedStarts{0, 1, 4, 5, 6, 7, 8, 9};
  for (std::size_t index = 0; index < expectedStarts.size(); ++index) {
    WF_CHECK_EQ(split.candidates[index].slots.first, expectedStarts[index]);
  }
  WF_CHECK_EQ(holed.runtime.stats().candidatesEvaluated, std::uint64_t{3});
  WF_CHECK_EQ(holed.runtime.stats().enumerationTruncations, std::uint64_t{0});
  const AllocationDecision next = holed.runtime.allocate(holed.request(AllocationRequestId(3)));
  WF_CHECK(next.allocated());
  WF_CHECK_EQ(next.explanation.selected.slots.first, std::uint32_t{0});
  WF_CHECK_EQ(holed.runtime.stats().candidatesEvaluated, std::uint64_t{11});
  WF_CHECK_EQ(holed.runtime.stats().enumerationTruncations, std::uint64_t{1});

  // The default bound is wide enough for this grid: the list is complete.
  Rig unlimited;
  const CandidateSet whole = unlimited.runtime.enumerateCandidates(unlimited.request(AllocationRequestId(1)));
  WF_CHECK_EQ(whole.candidates.size(), std::size_t{96});
  WF_CHECK(whole.complete);
  WF_CHECK_EQ(whole.omitted, std::size_t{0});
}

WF_TEST(expired_leases_stop_blocking_before_they_are_reclaimed) {
  Rig rig;
  const AllocationDecision owner = rig.runtime.allocate(rig.request(AllocationRequestId(1), 4));
  WF_REQUIRE(owner.allocated());
  const std::optional<SpectrumReservation> stored = rig.runtime.reservation(owner.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->lease.grantedAt == kRequestedAt);
  WF_CHECK(stored->lease.expiresAt == kRequestedAt + kLease);

  const Instant almost = kRequestedAt + Duration::minutes(59);
  const CandidateSet before = rig.runtime.enumerateCandidates(
      rig.request(AllocationRequestId(2), 1, almost));
  WF_REQUIRE(before.candidates.size() == std::size_t{92});
  WF_CHECK_EQ(before.candidates.front().slots.first, std::uint32_t{4});

  const Instant later = kRequestedAt + Duration::hours(2);
  const SpectrumRequest after = rig.request(AllocationRequestId(3), 1, later);
  const CandidateSet reused = rig.runtime.enumerateCandidates(after);
  WF_REQUIRE(reused.candidates.size() == std::size_t{96});
  WF_CHECK_EQ(reused.eligibleCount, std::size_t{96});
  WF_CHECK_EQ(reused.candidates.front().slots.first, std::uint32_t{0});

  // The expired lease is ignored by conflict detection, but the reservation is
  // still Reserved until a sweep reclaims it.
  const std::optional<SpectrumReservation> stillReserved = rig.runtime.reservation(owner.reservation);
  WF_REQUIRE(stillReserved.has_value());
  WF_CHECK(stillReserved->state == ReservationState::Reserved);
  WF_CHECK(!stillReserved->isLiveAt(later));

  const AllocationDecision replacement = rig.runtime.allocate(after);
  WF_CHECK(replacement.allocated());
  WF_CHECK_EQ(replacement.explanation.selected.slots.first, std::uint32_t{0});

  const ReclaimReport report = rig.runtime.reclaimExpired(later);
  WF_CHECK(report.status.ok());
  WF_CHECK_EQ(report.expired, std::size_t{1});
  WF_CHECK_EQ(report.reclaimed.size(), std::size_t{1});
  WF_CHECK_EQ(report.reclaimed[0].raw(), owner.reservation.raw());
  const std::optional<SpectrumReservation> reclaimed = rig.runtime.reservation(owner.reservation);
  WF_REQUIRE(reclaimed.has_value());
  WF_CHECK(reclaimed->state == ReservationState::Reclaimed);
  const std::optional<SpectrumReservation> live = rig.runtime.reservation(replacement.reservation);
  WF_REQUIRE(live.has_value());
  WF_CHECK(live->state == ReservationState::Reserved);
}

WF_TEST(replay_and_supersede_keep_ownership_exact) {
  Rig rig;
  const SpectrumRequest original = rig.request(AllocationRequestId(1), 2);
  const AllocationDecision first = rig.runtime.allocate(original);
  WF_REQUIRE(first.allocated());
  WF_CHECK_EQ(first.reservation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(first.generation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{1});

  const AllocationDecision replay = rig.runtime.allocate(original);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.status.ok());
  WF_CHECK(replay.outcome == AllocationOutcome::Allocated);
  WF_CHECK_EQ(replay.reservation.raw(), first.reservation.raw());
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{0});
  WF_CHECK_EQ(replay.explanation.selected.slots.first, std::uint32_t{0});
  WF_CHECK_EQ(replay.explanation.selected.slots.count, std::uint32_t{2});
  WF_CHECK(!replay.explanation.selected.detail.empty());

  SpectrumRequest differentShape = original;
  differentShape.owner = OwnerId(2);
  const AllocationDecision duplicate = rig.runtime.allocate(differentShape);
  WF_CHECK(duplicate.outcome == AllocationOutcome::RefusedDuplicate);
  WF_CHECK(duplicate.status.code == StatusCode::Duplicate);
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{1});

  Rig generations;
  SpectrumRequest advanced = generations.request(AllocationRequestId(1));
  advanced.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision committed = generations.runtime.allocate(advanced);
  WF_REQUIRE(committed.allocated());
  SpectrumRequest stale = advanced;
  stale.requestGeneration = AllocationRequestGeneration(1);
  const AllocationDecision refused = generations.runtime.allocate(stale);
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK(refused.status.code == StatusCode::StaleGeneration);
  WF_CHECK_EQ(generations.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(generations.runtime.stats().allocationsCommitted, std::uint64_t{1});

  // A newer generation of the same request identity supersedes the live
  // reservation; the superseded slots are free again for later candidates.
  SpectrumRequest superseding = original;
  superseding.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision superseded = rig.runtime.allocate(superseding);
  WF_CHECK(superseded.allocated());
  WF_CHECK(superseded.reservation != first.reservation);
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{2});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{2});
  const std::optional<SpectrumReservation> previous = rig.runtime.reservation(first.reservation);
  WF_REQUIRE(previous.has_value());
  WF_CHECK(previous->state == ReservationState::Superseded);
  const std::optional<SpectrumReservation> current = rig.runtime.reservation(superseded.reservation);
  WF_REQUIRE(current.has_value());
  WF_CHECK_EQ(current->slots.first, std::uint32_t{2});
  WF_CHECK_EQ(current->slots.count, std::uint32_t{2});

  const CandidateSet after = rig.runtime.enumerateCandidates(rig.request(AllocationRequestId(9)));
  WF_REQUIRE(after.candidates.size() == std::size_t{94});
  WF_CHECK_EQ(after.eligibleCount, std::size_t{94});
  WF_CHECK_EQ(after.candidates[0].slots.first, std::uint32_t{0});
  WF_CHECK_EQ(after.candidates[1].slots.first, std::uint32_t{1});
  WF_CHECK_EQ(after.candidates[2].slots.first, std::uint32_t{4});
}

WF_TEST(usage_matches_committed_occupancy) {
  Rig rig;
  SpectrumRequest owner = rig.request(AllocationRequestId(1), 4);
  owner.notBefore = kRequestedAt + Duration::minutes(30);
  const AllocationDecision decision = rig.runtime.allocate(owner);
  WF_REQUIRE(decision.allocated());

  const std::optional<SpectrumReservation> reservation = rig.runtime.reservation(decision.reservation);
  WF_REQUIRE(reservation.has_value());
  WF_CHECK(reservation->lease.grantedAt == owner.notBefore);
  WF_CHECK(reservation->lease.expiresAt - reservation->lease.grantedAt == kLease);
  WF_CHECK(reservation->createdAt == owner.notBefore);
  WF_CHECK_EQ(reservation->slots.first, std::uint32_t{0});
  WF_CHECK_EQ(reservation->slots.count, std::uint32_t{4});
  WF_CHECK_EQ(reservation->guardBandMhz, std::int64_t{0});
  WF_CHECK_EQ(reservation->lease.maxRenewals, std::uint32_t{0});

  const Instant now = kRequestedAt + Duration::hours(1);
  const std::optional<SpectrumUsage> usage = rig.runtime.usage(SpectrumDomainId(1), now);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->domain.raw(), std::uint64_t{1});
  WF_CHECK_EQ(usage->totalSlots, std::uint32_t{96});
  WF_CHECK_EQ(usage->allocatableSlots, std::uint32_t{96});
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{4});
  WF_CHECK_EQ(usage->activeSlots, std::uint32_t{0});
  WF_CHECK_EQ(usage->reservedSlots, std::uint32_t{4});
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{92});
  WF_CHECK_EQ(usage->lapsedSlots, std::uint32_t{0});
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->activeReservations, std::uint32_t{0});
  WF_REQUIRE(usage->freeRuns.size() == std::size_t{1});
  WF_CHECK_EQ(usage->freeRuns[0].first, std::uint32_t{4});
  WF_CHECK_EQ(usage->freeRuns[0].count, std::uint32_t{92});

  WF_CHECK(rig.runtime.release(decision.reservation, ReservationGeneration(1),
                               rig.releaseAuthority(), now).ok());
  const std::optional<SpectrumUsage> released = rig.runtime.usage(SpectrumDomainId(1), now);
  WF_REQUIRE(released.has_value());
  WF_CHECK_EQ(released->liveSlots, std::uint32_t{0});
  WF_CHECK_EQ(released->freeSlots, std::uint32_t{96});
  WF_CHECK_EQ(released->liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(released->releasedReservations, std::uint32_t{1});
  WF_REQUIRE(released->freeRuns.size() == std::size_t{1});
  WF_CHECK_EQ(released->freeRuns[0].count, std::uint32_t{96});
}

WF_TEST(multi_domain_requests_resolve_every_domain) {
  Rig rig;
  (void)rig.runtime.registerDomain(makeDomain(SpectrumDomainId(2), ChannelGridId(1)));
  WF_REQUIRE(rig.runtime
                 .publishCapability(makeCapability(
                     SpectrumDomainId(2), ChannelGridId(1), SpectrumDomainGeneration(1),
                     GridGeneration(1), SpectrumSupport::Supported, 0, 96, rig.fence))
                 .ok());

  const SpectrumRequest single = rig.request(AllocationRequestId(1));
  const SpectrumRequest multiple =
      makeRequest(AllocationRequestId(2), OwnerId(1),
                  {SpectrumDomainId(1), SpectrumDomainId(2)},
                  {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)}, ChannelGridId(1),
                  GridGeneration(1), 1, kRequestedAt, kLease, rig.eligibility(), rig.reservation(),
                  ContiguityRequirement::Unspecified, ContinuityRequirement::Required);

  const CandidateSet oneDomain = rig.runtime.enumerateCandidates(single);
  const CandidateSet twoDomains = rig.runtime.enumerateCandidates(multiple);
  WF_REQUIRE(twoDomains.candidates.size() == oneDomain.candidates.size());
  WF_CHECK_EQ(twoDomains.eligibleCount, std::size_t{96});
  WF_CHECK(twoDomains.complete);
  for (std::size_t index = 0; index < twoDomains.candidates.size(); ++index) {
    const SpectrumCandidate& candidate = twoDomains.candidates[index];
    WF_CHECK(candidate.slots == oneDomain.candidates[index].slots);
    WF_CHECK(candidate.frequency == oneDomain.candidates[index].frequency);
    WF_CHECK_EQ(candidate.ordinal, index);
    WF_CHECK_EQ(candidate.anchorDomain.raw(), std::uint64_t{1});
    WF_CHECK(!candidate.crossGrid);
    WF_CHECK_EQ(candidate.perDomainSlots.size(), std::size_t{2});
    WF_CHECK(candidate.perDomainSlots[0] == candidate.slots);
    WF_CHECK(candidate.perDomainSlots[1] == candidate.slots);
  }

  const AllocationDecision decision = rig.runtime.allocate(multiple);
  WF_CHECK(decision.allocated());
  const std::optional<SpectrumReservation> reservation = rig.runtime.reservation(decision.reservation);
  WF_REQUIRE(reservation.has_value());
  WF_CHECK_EQ(reservation->domains.size(), std::size_t{2});
  WF_CHECK_EQ(reservation->perDomainSlots.size(), std::size_t{2});
  WF_CHECK_EQ(reservation->anchorDomain.raw(), std::uint64_t{1});
  WF_CHECK(!reservation->crossGrid);
  WF_CHECK(reservation->continuityRequired);
  WF_CHECK(!reservation->contiguityRequired);
  WF_CHECK_EQ(reservation->perDomainSlots[1].first, std::uint32_t{0});
}

WF_TEST(enumeration_and_explanation_never_create_ownership) {
  RuntimeConfig config;
  config.maxCandidatesPerRequest = 4;
  Rig rig(config);

  SpectrumRequest request = rig.request(AllocationRequestId(1), 2);
  request.guardBandMhz = kSlotWidth;
  request.constraints.excludedSlots.push_back(SlotRange{0, 1});

  for (int pass = 0; pass < 3; ++pass) {
    const CandidateSet set = rig.runtime.enumerateCandidates(request);
    WF_CHECK_EQ(set.candidates.size(), std::size_t{4});
    WF_CHECK(!set.complete);
    const DecisionExplanation explanation = rig.runtime.explain(request);
    WF_CHECK_EQ(explanation.candidatesEnumerated, std::size_t{4});
    WF_CHECK(explanation.outcome == AllocationOutcome::Allocated);
  }

  WF_CHECK(rig.runtime.reservations().empty());
  const std::vector<SpectrumUsage> usages = rig.runtime.usageAll(kRequestedAt);
  WF_REQUIRE(usages.size() == std::size_t{1});
  WF_CHECK_EQ(usages[0].allocatableSlots, std::uint32_t{96});
  WF_CHECK_EQ(usages[0].liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(usages[0].liveSlots, std::uint32_t{0});
  WF_REQUIRE(usages[0].freeRuns.size() == std::size_t{1});
  WF_CHECK_EQ(usages[0].freeRuns[0].count, std::uint32_t{96});
  const RuntimeStats stats = rig.runtime.stats();
  WF_CHECK_EQ(stats.allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(stats.allocationsRefused, std::uint64_t{0});
  WF_CHECK_EQ(stats.candidatesEvaluated, std::uint64_t{0});
  WF_CHECK_EQ(stats.enumerationTruncations, std::uint64_t{0});
  WF_CHECK_EQ(stats.renewals, std::uint64_t{0});
  WF_CHECK_EQ(stats.releases, std::uint64_t{0});
  WF_CHECK_EQ(stats.activations, std::uint64_t{0});
  WF_CHECK_EQ(stats.deactivations, std::uint64_t{0});
  WF_CHECK_EQ(stats.reclamations, std::uint64_t{0});
  WF_CHECK_EQ(stats.replayRejections, std::uint64_t{0});
  WF_CHECK_EQ(stats.corruptionDetections, std::uint64_t{0});
}

WF_TEST(request_channel_width_is_enforced_on_the_request_grid) {
  // A fixed grid carries exactly one slot per channel.
  WF_CHECK(channelWidthSupported(fixedGrid(), 1));
  WF_CHECK(!channelWidthSupported(fixedGrid(), 2));
  BoundRig fixed(fixedGrid());
  WF_REQUIRE(fixed.capabilityStatus.ok());
  const SpectrumRequest oneSlotRequest = fixed.request(AllocationRequestId(1), 1);
  const CandidateSet oneSlot = fixed.runtime.enumerateCandidates(oneSlotRequest);
  WF_REQUIRE(oneSlot.candidates.size() == std::size_t{96});
  WF_CHECK_EQ(oneSlot.eligibleCount, std::size_t{96});
  const AllocationDecision committed = fixed.runtime.allocate(oneSlotRequest);
  WF_REQUIRE(committed.allocated());
  WF_CHECK_EQ(fixed.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(fixed.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(fixed.runtime.stats().allocationsRefused, std::uint64_t{0});
  WF_CHECK_EQ(fixed.runtime.stats().candidatesEvaluated, std::uint64_t{96});

  // A two-slot channel is not representable on that grid: the request is refused
  // before any candidate exists, and nothing is allocated.
  const SpectrumRequest twoSlotRequest = fixed.request(AllocationRequestId(2), 2);
  const CandidateSet twoSlot = fixed.runtime.enumerateCandidates(twoSlotRequest);
  WF_CHECK(twoSlot.status.code == StatusCode::InvalidArgument);
  WF_CHECK(!twoSlot.status.message.empty());
  WF_CHECK(!twoSlot.summary.empty());
  WF_CHECK(twoSlot.candidates.empty());
  WF_CHECK_EQ(twoSlot.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(twoSlot.omitted, std::size_t{0});
  WF_CHECK(!twoSlot.complete);
  const DecisionExplanation explained = fixed.runtime.explain(twoSlotRequest);
  WF_CHECK(explained.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK_EQ(explained.candidatesEnumerated, std::size_t{0});

  const AllocationDecision refused = fixed.runtime.allocate(twoSlotRequest);
  WF_CHECK(!refused.allocated());
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(refused.status.code == StatusCode::InvalidArgument);
  WF_CHECK(!refused.reservation.valid());
  WF_CHECK(!refused.generation.valid());
  WF_CHECK(!refused.explanation.reasons.empty());
  WF_CHECK_EQ(fixed.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(fixed.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(fixed.runtime.stats().allocationsRefused, std::uint64_t{1});
  WF_CHECK_EQ(fixed.runtime.stats().candidatesEvaluated, std::uint64_t{96});
  const std::optional<SpectrumCapability> capability =
      fixed.runtime.capability(SpectrumDomainId(1));
  WF_REQUIRE(capability.has_value());
  WF_CHECK_EQ(allocatableWindow(*capability).count, std::uint32_t{96});

  // The widest width the shape validator allows is still bounded by the grid.
  const AllocationDecision widest =
      fixed.runtime.allocate(fixed.request(AllocationRequestId(3), kMaxSlotsPerChannel));
  WF_CHECK(widest.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(widest.status.code == StatusCode::InvalidArgument);
  WF_CHECK_EQ(fixed.runtime.stats().allocationsRefused, std::uint64_t{2});

  // Enumeration is identity-agnostic, while allocation answers an already
  // committed request identity before the grid width gate is consulted.
  const SpectrumRequest replay = fixed.request(AllocationRequestId(1), 2);
  WF_CHECK(fixed.runtime.enumerateCandidates(replay).status.code == StatusCode::InvalidArgument);
  const AllocationDecision duplicate = fixed.runtime.allocate(replay);
  WF_CHECK(duplicate.outcome == AllocationOutcome::RefusedDuplicate);
  WF_CHECK(duplicate.status.code == StatusCode::Duplicate);
  WF_CHECK_EQ(fixed.runtime.reservations().size(), std::size_t{1});

  // The request grid must exist and be current before its width bound applies.
  SpectrumRequest unknownGrid = fixed.request(AllocationRequestId(4), 2);
  unknownGrid.grid = ChannelGridId(9);
  const AllocationDecision withoutGrid = fixed.runtime.allocate(unknownGrid);
  WF_CHECK(withoutGrid.outcome == AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK(withoutGrid.status.code == StatusCode::InvalidArgument);
  SpectrumRequest staleGrid = fixed.request(AllocationRequestId(5), 2);
  staleGrid.gridGeneration = GridGeneration(2);
  const AllocationDecision staleDecision = fixed.runtime.allocate(staleGrid);
  WF_CHECK(staleDecision.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK(staleDecision.status.code == StatusCode::StaleGeneration);
  WF_CHECK_EQ(fixed.runtime.reservations().size(), std::size_t{1});

  // A flex grid expresses [minSlotsPerChannel, maxSlotsPerChannel] inclusive.
  BoundRig flex(flexGrid(ChannelGridId(1), GridGeneration(1), kAnchor, kSlotWidth, 96, 1, 8));
  WF_REQUIRE(flex.capabilityStatus.ok());
  const CandidateSet atMinimum =
      flex.runtime.enumerateCandidates(flex.request(AllocationRequestId(1), 1));
  WF_REQUIRE(atMinimum.candidates.size() == std::size_t{96});
  WF_CHECK_EQ(atMinimum.eligibleCount, std::size_t{96});
  const CandidateSet atMaximum =
      flex.runtime.enumerateCandidates(flex.request(AllocationRequestId(2), 8));
  WF_REQUIRE(atMaximum.candidates.size() == std::size_t{89});
  WF_CHECK_EQ(atMaximum.eligibleCount, std::size_t{89});
  WF_CHECK_EQ(atMaximum.candidates.front().slots.count, std::uint32_t{8});
  WF_CHECK_EQ(atMaximum.candidates.back().slots.first, std::uint32_t{88});

  const AllocationDecision minimumWidth = flex.runtime.allocate(flex.request(AllocationRequestId(3), 1));
  WF_REQUIRE(minimumWidth.allocated());
  const std::optional<SpectrumReservation> minimumSlots =
      flex.runtime.reservation(minimumWidth.reservation);
  WF_REQUIRE(minimumSlots.has_value());
  WF_CHECK_EQ(minimumSlots->slots.first, std::uint32_t{0});
  WF_CHECK_EQ(minimumSlots->slots.count, std::uint32_t{1});
  const AllocationDecision maximumWidth = flex.runtime.allocate(flex.request(AllocationRequestId(4), 8));
  WF_REQUIRE(maximumWidth.allocated());
  const std::optional<SpectrumReservation> maximumSlots =
      flex.runtime.reservation(maximumWidth.reservation);
  WF_REQUIRE(maximumSlots.has_value());
  WF_CHECK_EQ(maximumSlots->slots.first, std::uint32_t{1});
  WF_CHECK_EQ(maximumSlots->slots.count, std::uint32_t{8});

  const SpectrumRequest pastMaximum = flex.request(AllocationRequestId(5), 9);
  const CandidateSet beyond = flex.runtime.enumerateCandidates(pastMaximum);
  WF_CHECK(beyond.status.code == StatusCode::InvalidArgument);
  WF_CHECK(beyond.candidates.empty());
  WF_CHECK(!beyond.complete);
  const AllocationDecision refusedPast = flex.runtime.allocate(pastMaximum);
  WF_CHECK(refusedPast.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(refusedPast.status.code == StatusCode::InvalidArgument);
  WF_CHECK_EQ(flex.runtime.reservations().size(), std::size_t{2});
  WF_CHECK_EQ(flex.runtime.stats().allocationsRefused, std::uint64_t{1});

  // Zero slots never reaches the grid: the shape validator refuses it first.
  const AllocationDecision zeroWidth = flex.runtime.allocate(flex.request(AllocationRequestId(6), 0));
  WF_CHECK(zeroWidth.outcome == AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK(zeroWidth.status.code == StatusCode::InvalidArgument);
  WF_CHECK_EQ(flex.runtime.reservations().size(), std::size_t{2});

  // A flex grid whose smallest channel is two slots: the bounds are inclusive,
  // so two and eight are eligible and committable while one and nine are refused
  // for their width.
  const ChannelGrid narrow =
      flexGrid(ChannelGridId(1), GridGeneration(1), kAnchor, kSlotWidth, 96, 2, 8);
  WF_CHECK(!channelWidthSupported(narrow, 1));
  WF_CHECK(channelWidthSupported(narrow, 2));
  WF_CHECK(channelWidthSupported(narrow, 8));
  WF_CHECK(!channelWidthSupported(narrow, 9));
  BoundRig narrowRig(narrow);
  WF_REQUIRE(narrowRig.capabilityStatus.ok());

  const SpectrumRequest narrowAtMinimum = narrowRig.request(AllocationRequestId(1), 2);
  const CandidateSet narrowMinimumSet = narrowRig.runtime.enumerateCandidates(narrowAtMinimum);
  WF_REQUIRE(narrowMinimumSet.candidates.size() == std::size_t{95});
  WF_CHECK_EQ(narrowMinimumSet.eligibleCount, std::size_t{95});
  WF_CHECK_EQ(narrowMinimumSet.candidates.front().slots.first, std::uint32_t{0});
  WF_CHECK_EQ(narrowMinimumSet.candidates.front().slots.count, std::uint32_t{2});
  WF_CHECK_EQ(narrowMinimumSet.candidates.back().slots.first, std::uint32_t{94});
  WF_CHECK(narrowMinimumSet.status.code != StatusCode::InvalidArgument);
  // Both bounds are enumerated before anything is owned.
  const SpectrumRequest narrowAtMaximum = narrowRig.request(AllocationRequestId(2), 8);
  const CandidateSet narrowMaximumSet = narrowRig.runtime.enumerateCandidates(narrowAtMaximum);
  WF_REQUIRE(narrowMaximumSet.candidates.size() == std::size_t{89});
  WF_CHECK_EQ(narrowMaximumSet.eligibleCount, std::size_t{89});
  WF_CHECK_EQ(narrowMaximumSet.candidates.front().slots.first, std::uint32_t{0});
  WF_CHECK_EQ(narrowMaximumSet.candidates.front().slots.count, std::uint32_t{8});
  WF_CHECK_EQ(narrowMaximumSet.candidates.back().slots.first, std::uint32_t{88});
  WF_CHECK(narrowMaximumSet.status.code != StatusCode::InvalidArgument);

  const AllocationDecision narrowMinimum = narrowRig.runtime.allocate(narrowAtMinimum);
  WF_CHECK(narrowMinimum.outcome != AllocationOutcome::RefusedChannelWidth);
  WF_REQUIRE(narrowMinimum.allocated());
  const std::optional<SpectrumReservation> minimumReservation =
      narrowRig.runtime.reservation(narrowMinimum.reservation);
  WF_REQUIRE(minimumReservation.has_value());
  WF_CHECK_EQ(minimumReservation->slots.first, std::uint32_t{0});
  WF_CHECK_EQ(minimumReservation->slots.count, std::uint32_t{2});
  WF_CHECK(minimumReservation->contiguityRequired);

  // Once the two-slot channel is owned, the first free eight-slot run starts at
  // slot two.
  const CandidateSet afterMinimum =
      narrowRig.runtime.enumerateCandidates(narrowRig.request(AllocationRequestId(3), 8));
  WF_REQUIRE(afterMinimum.candidates.size() == std::size_t{87});
  WF_CHECK_EQ(afterMinimum.candidates.front().slots.first, std::uint32_t{2});
  WF_CHECK_EQ(afterMinimum.candidates.back().slots.first, std::uint32_t{88});
  const AllocationDecision narrowMaximum = narrowRig.runtime.allocate(narrowAtMaximum);
  WF_CHECK(narrowMaximum.outcome != AllocationOutcome::RefusedChannelWidth);
  WF_REQUIRE(narrowMaximum.allocated());
  const std::optional<SpectrumReservation> maximumReservation =
      narrowRig.runtime.reservation(narrowMaximum.reservation);
  WF_REQUIRE(maximumReservation.has_value());
  WF_CHECK_EQ(maximumReservation->slots.first, std::uint32_t{2});
  WF_CHECK_EQ(maximumReservation->slots.count, std::uint32_t{8});

  const AllocationDecision belowMinimum =
      narrowRig.runtime.allocate(narrowRig.request(AllocationRequestId(4), 1));
  WF_CHECK(belowMinimum.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(belowMinimum.status.code == StatusCode::InvalidArgument);
  WF_CHECK(!belowMinimum.reservation.valid());
  const AllocationDecision narrowPast =
      narrowRig.runtime.allocate(narrowRig.request(AllocationRequestId(5), 9));
  WF_CHECK(narrowPast.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(narrowPast.status.code == StatusCode::InvalidArgument);
  WF_CHECK(!narrowPast.reservation.valid());
  WF_CHECK_EQ(narrowRig.runtime.reservations().size(), std::size_t{2});
  WF_CHECK_EQ(narrowRig.runtime.stats().allocationsCommitted, std::uint64_t{2});
  WF_CHECK_EQ(narrowRig.runtime.stats().allocationsRefused, std::uint64_t{2});
}

WF_TEST_MAIN()
