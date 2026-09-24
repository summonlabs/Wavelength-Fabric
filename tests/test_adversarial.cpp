#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

// Streams any library enum through its own token table, so a failing comparison
// prints the documented name instead of the underlying integer.
template <class Enum>
  requires std::is_enum_v<Enum> && requires(Enum value) { toToken(value); }
std::ostream& operator<<(std::ostream& out, Enum value) {
  return out << toToken(value);
}

// Streams a strongly typed identity or generation through its raw value.
template <class Strong>
  requires std::is_class_v<Strong> && requires(const Strong& value) { value.raw(); }
std::ostream& operator<<(std::ostream& out, const Strong& value) {
  return out << value.raw();
}

// Malformed and hostile input, and every configured limit.
//
// Each case asserts an exact typed outcome: a status code, an allocation
// outcome, a reservation count, a counter. A refusal is checked together with
// the accounting it must leave behind, and a success with the ownership it
// created.

namespace {

using namespace wavelength_fabric;

constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::int64_t kSlotWidthMhz = 12'500;
constexpr std::uint32_t kSlots = 48;
constexpr std::uint32_t kMaxChannelSlotsPerChannel = 8;
constexpr std::int64_t kEndMhz = kAnchorMhz + kSlotWidthMhz * static_cast<std::int64_t>(kSlots);
constexpr std::uint64_t kMaxIdentity = 0xFFFF'FFFF'FFFF'FFFFull;
// Ten years in nanoseconds: the lease ceiling of validateRequestShape.
constexpr std::int64_t kMaxLeaseNanos = 3650ll * 24ll * 60ll * 60ll * 1'000'000'000ll;

const Instant kStart = Instant::fromSeconds(1'800'000'000);
const Instant kLate = kStart + Duration::seconds(100'000);
const Duration kLease = Duration::seconds(600);

std::uint64_t g_nextRequest = 1;

AllocationRequestId nextRequest() { return AllocationRequestId(g_nextRequest++); }

// A runtime with one grid and three domains, each fully allocatable.
struct World {
  explicit World(std::uint32_t slotCount = kSlots, RuntimeConfig config = RuntimeConfig())
      : fixture(std::move(config)), slots(slotCount) {
    const std::int64_t endMhz =
        kAnchorMhz + kSlotWidthMhz * static_cast<std::int64_t>(slotCount);
    const ChannelGrid definition = wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), kAnchorMhz,
                                                     kSlotWidthMhz, slotCount, 1, 8);
    (void)fixture.runtime.registerGrid(definition);
    for (std::uint64_t value = 1; value <= 3; ++value) {
      const SpectrumDomainId domain(value);
      (void)fixture.runtime.registerDomain(wf_test::makeDomain(domain, ChannelGridId(1)));
      SpectrumCapability capability = wf_test::makeCapability(
          domain, ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
          SpectrumSupport::Supported, 0, slotCount, fixture.runtime.fence());
      capability.minTunableMhz = kAnchorMhz;
      capability.maxTunableMhz = endMhz;
      (void)fixture.runtime.publishCapability(capability);
    }
  }

  wf_test::Fixture fixture;
  std::uint32_t slots;

  SpectrumRuntime& runtime() { return fixture.runtime; }
  const SpectrumRuntime& runtime() const { return fixture.runtime; }

  SpectrumRequest request(std::uint32_t slotWidth, Instant requestedAt = kStart,
                          SpectrumDomainId domain = SpectrumDomainId(1)) const {
    return wf_test::makeRequest(nextRequest(), OwnerId(1), {domain},
                                {SpectrumDomainGeneration(1)}, ChannelGridId(1),
                                GridGeneration(1), slotWidth, requestedAt, kLease,
                                fixture.eligibility(), fixture.reservation(),
                                slotWidth > 1 ? ContiguityRequirement::Required
                                              : ContiguityRequirement::Unspecified,
                                ContinuityRequirement::Unspecified);
  }

  SpectrumRequest multiRequest(const std::vector<SpectrumDomainId>& domains,
                               std::uint32_t slotWidth) const {
    return wf_test::makeRequest(
        nextRequest(), OwnerId(1), domains,
        std::vector<SpectrumDomainGeneration>(domains.size(), SpectrumDomainGeneration(1)),
        ChannelGridId(1), GridGeneration(1), slotWidth, kStart, kLease, fixture.eligibility(),
        fixture.reservation(),
        slotWidth > 1 ? ContiguityRequirement::Required : ContiguityRequirement::Unspecified,
        domains.size() > 1 ? ContinuityRequirement::Required : ContinuityRequirement::Unspecified);
  }
};

EligibilityAuthority eligibilityFor(const SpectrumRuntime& runtime) {
  EligibilityAuthority token;
  token.generation = EligibilityAuthorityGeneration(1);
  token.fence = runtime.fence();
  return token;
}

ReservationAuthority reservationFor(const SpectrumRuntime& runtime) {
  ReservationAuthority token;
  token.generation = ReservationAuthorityGeneration(1);
  token.fence = runtime.fence();
  return token;
}

std::filesystem::path temporaryDirectory(const std::string& name) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
  return path;
}

}  // namespace

// ---------------------------------------------------------------------------
// Malformed requests
// ---------------------------------------------------------------------------

WF_TEST(a_default_constructed_request_is_refused_without_touching_state) {
  World world;
  const SpectrumRequest request;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  const AllocationDecision decision = world.runtime().allocate(request);
  WF_CHECK(!decision.allocated());
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK_EQ(decision.status.code, StatusCode::InvalidArgument);
  WF_CHECK(decision.reservation.none());
  WF_CHECK(!decision.explanation.reasons.empty());
  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().allocationsRefused, std::uint64_t(1));
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));

  const CandidateSet set = world.runtime().enumerateCandidates(request);
  WF_CHECK_EQ(set.status.code, StatusCode::InvalidArgument);
  WF_CHECK(set.candidates.empty());
  WF_CHECK(!set.complete);
  WF_CHECK(!set.summary.empty());

  const DecisionExplanation explanation = world.runtime().explain(request);
  WF_CHECK_EQ(explanation.outcome, AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK(world.runtime().reservations().empty());
}

WF_TEST(every_zero_identity_is_refused_at_its_own_field) {
  World world;
  const SpectrumRequest base = world.request(1);

  SpectrumRequest request = base;
  request.requestId = AllocationRequestId(0);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);
  WF_CHECK_EQ(world.runtime().allocate(request).outcome,
              AllocationOutcome::RefusedInvalidRequest);

  request = base;
  request.requestGeneration = AllocationRequestGeneration(0);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.owner = OwnerId(0);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.ownerGeneration = OwnerGeneration(0);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.domains = {SpectrumDomainId(0)};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.domainGenerations = {SpectrumDomainGeneration(0)};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.grid = ChannelGridId(0);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.gridGeneration = GridGeneration(0);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.slots = 0;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.eligibilityAuthority = EligibilityAuthority{};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.reservationAuthority = ReservationAuthority{};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.leaseDuration = Duration::zero();
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.requestedAt = Instant{};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.notBefore = Instant::fromNanos(-1);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.guardBandMhz = -1;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.guardBandMhz = kMaxGuardBandMhz + 1;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.guardBandMhz = kMaxGuardBandMhz;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);

  request = base;
  request.leaseDuration = Duration::nanos(kMaxLeaseNanos);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);

  request = base;
  request.leaseDuration = Duration::nanos(kMaxLeaseNanos + 1);
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  WF_CHECK(world.runtime().reservations().empty());
}

WF_TEST(duplicate_and_unsorted_domain_lists_are_refused) {
  World world;
  const SpectrumRequest base = world.request(1);

  SpectrumRequest request = base;
  request.domains = {SpectrumDomainId(1), SpectrumDomainId(1)};
  request.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  request.continuity = ContinuityRequirement::Required;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);
  WF_CHECK_EQ(world.runtime().allocate(request).outcome,
              AllocationOutcome::RefusedInvalidRequest);

  request = base;
  request.domains = {SpectrumDomainId(2), SpectrumDomainId(1)};
  request.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  request.continuity = ContinuityRequirement::Required;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);
  WF_CHECK_EQ(world.runtime().allocate(request).outcome,
              AllocationOutcome::RefusedInvalidRequest);

  request = base;
  request.domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
  request.domainGenerations = {SpectrumDomainGeneration(1)};
  request.continuity = ContinuityRequirement::Required;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.domains = std::vector<SpectrumDomainId>(kMaxRequestDomains + 1, SpectrumDomainId(1));
  request.domainGenerations =
      std::vector<SpectrumDomainGeneration>(kMaxRequestDomains + 1, SpectrumDomainGeneration(1));
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  // A multi-domain request that does not state continuity is refused, and so is
  // a multi-slot request that does not state contiguity.
  request = world.multiRequest({SpectrumDomainId(1), SpectrumDomainId(2)}, 1);
  request.continuity = ContinuityRequirement::Unspecified;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.slots = 2;
  request.contiguity = ContiguityRequirement::Unspecified;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));
}

WF_TEST(extreme_slot_counts_and_frequency_windows_are_bounded) {
  World world;
  const SpectrumRequest base = world.request(1);

  SpectrumRequest request = base;
  request.slots = kMaxSlotsPerChannel;
  request.contiguity = ContiguityRequirement::Required;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);
  const AllocationDecision tooWide = world.runtime().allocate(request);
  WF_CHECK(!tooWide.allocated());
  WF_CHECK_EQ(tooWide.outcome, AllocationOutcome::RefusedChannelWidth);
  WF_CHECK_EQ(tooWide.status.code, StatusCode::InvalidArgument);
  WF_CHECK(tooWide.explanation.selected.slots.count == 0);

  // A width the request grid cannot express is refused before any candidate is
  // considered: this grid carries at most eight slots per channel.
  request = base;
  request.slots = 9;
  request.contiguity = ContiguityRequirement::Required;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);
  const AllocationDecision nineWide = world.runtime().allocate(request);
  WF_CHECK(!nineWide.allocated());
  WF_CHECK_EQ(nineWide.outcome, AllocationOutcome::RefusedChannelWidth);
  WF_CHECK_EQ(nineWide.status.code, StatusCode::InvalidArgument);

  request = base;
  request.slots = kMaxSlotsPerChannel + 1;
  request.contiguity = ContiguityRequirement::Required;
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);
  WF_CHECK_EQ(world.runtime().allocate(request).status.code, StatusCode::InvalidArgument);

  request = base;
  request.frequencyWindows = {FrequencyRange{0, kMaxFrequencyMhz}};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.frequencyWindows = {FrequencyRange{kAnchorMhz, kAnchorMhz}};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.frequencyWindows = {FrequencyRange{kAnchorMhz, kMaxFrequencyMhz + 1}};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.frequencyWindows =
      std::vector<FrequencyRange>(kMaxFrequencyWindows + 1, FrequencyRange{kAnchorMhz, kEndMhz});
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.constraints.excludedSlots = {SlotRange{0, 0}};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.constraints.excludedSlots = {SlotRange{0xFFFF'FFFFu, 1}};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);

  request = base;
  request.constraints.excludedSlots =
      std::vector<SlotRange>(kMaxExcludedRanges + 1, SlotRange{0, 1});
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.constraints.excludedFrequencies = {FrequencyRange{kAnchorMhz, kAnchorMhz}};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.constraints.mustNotConflictWith = {ReservationId(0)};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);

  request = base;
  request.constraints.mustNotConflictWith = {ReservationId(9999)};
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);
  const AllocationDecision unknownConstraint = world.runtime().allocate(request);
  WF_CHECK(!unknownConstraint.allocated());
  WF_CHECK_EQ(unknownConstraint.outcome, AllocationOutcome::RefusedConstraint);
  WF_CHECK_EQ(unknownConstraint.status.code, StatusCode::Refused);

  request = base;
  request.frequencyWindows = {FrequencyRange{kAnchorMhz, kAnchorMhz + kSlotWidthMhz}};
  request.slots = 1;
  const AllocationDecision windowed = world.runtime().allocate(request);
  WF_CHECK(windowed.allocated());
  WF_REQUIRE(windowed.allocated());
  const std::optional<SpectrumReservation> stored =
      world.runtime().reservation(windowed.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->slots.first == 0 && stored->slots.count == 1);
  WF_CHECK(stored->frequency.lowMhz == kAnchorMhz);
}

WF_TEST(frequencies_and_instants_that_would_overflow_are_refused) {
  World world;
  const SpectrumRequest base = world.request(1);

  SpectrumRequest request = base;
  request.requestedAt = Instant::max();
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::InvalidArgument);
  WF_CHECK_EQ(world.runtime().allocate(request).outcome,
              AllocationOutcome::RefusedInvalidRequest);

  // A lease that starts at notBefore overflows even though requestedAt is
  // small. The commit refuses it, consumes no reservation identity and creates
  // no ownership.
  request = base;
  request.notBefore = Instant::max();
  WF_CHECK_EQ(validateRequestShape(request).code, StatusCode::Ok);
  const AllocationDecision overflowed = world.runtime().allocate(request);
  WF_CHECK(!overflowed.allocated());
  WF_CHECK_EQ(overflowed.outcome, AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK_EQ(overflowed.status.code, StatusCode::InvalidArgument);
  WF_CHECK(overflowed.reservation.none());
  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));

  const AllocationDecision next = world.runtime().allocate(world.request(1));
  WF_REQUIRE(next.allocated());
  WF_CHECK_EQ(next.reservation.raw(), std::uint64_t(1));

  SpectrumRequest maximal = base;
  maximal.requestId = AllocationRequestId(kMaxIdentity);
  maximal.owner = OwnerId(kMaxIdentity);
  maximal.ownerGeneration = OwnerGeneration(kMaxIdentity);
  maximal.domains = {SpectrumDomainId(2)};
  maximal.domainGenerations = {SpectrumDomainGeneration(1)};
  const AllocationDecision huge = world.runtime().allocate(maximal);
  WF_CHECK(huge.allocated());
  WF_CHECK_EQ(huge.reservation.raw(), std::uint64_t(2));
  const std::optional<SpectrumReservation> stored = world.runtime().reservation(huge.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->requestId == AllocationRequestId(kMaxIdentity));
  WF_CHECK(stored->owner == OwnerId(kMaxIdentity));

  SpectrumRequest unknown = base;
  unknown.domains = {SpectrumDomainId(kMaxIdentity)};
  unknown.domainGenerations = {SpectrumDomainGeneration(1)};
  const AllocationDecision missing = world.runtime().allocate(unknown);
  WF_CHECK_EQ(missing.outcome, AllocationOutcome::RefusedUnknownDomain);
  WF_CHECK_EQ(missing.status.code, StatusCode::NotFound);
}

WF_TEST(unknown_identities_are_reported_as_unknown_everywhere) {
  World world;
  WF_CHECK(!world.runtime().grid(ChannelGridId(0)).has_value());
  WF_CHECK(!world.runtime().grid(ChannelGridId(999)).has_value());
  WF_CHECK(!world.runtime().domain(SpectrumDomainId(0)).has_value());
  WF_CHECK(!world.runtime().domain(SpectrumDomainId(999)).has_value());
  WF_CHECK(!world.runtime().capability(SpectrumDomainId(999)).has_value());
  WF_CHECK(!world.runtime().exclusionDomain(ExclusionDomainId(999)).has_value());
  WF_CHECK(!world.runtime().reservation(ReservationId(0)).has_value());
  WF_CHECK(!world.runtime().reservation(ReservationId(999)).has_value());
  WF_CHECK(world.runtime().reservationsForDomain(SpectrumDomainId(999)).empty());
  WF_CHECK(!world.runtime().usage(SpectrumDomainId(999), kStart).has_value());

  SpectrumRequest request = world.request(1);
  request.grid = ChannelGridId(999);
  const AllocationDecision unknownGrid = world.runtime().allocate(request);
  WF_CHECK(!unknownGrid.allocated());
  WF_CHECK_EQ(unknownGrid.outcome, AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK_EQ(unknownGrid.status.code, StatusCode::InvalidArgument);
  WF_CHECK(!unknownGrid.explanation.reasons.empty());

  request = world.request(1);
  request.domains = {SpectrumDomainId(999)};
  request.domainGenerations = {SpectrumDomainGeneration(1)};
  const AllocationDecision unknownDomain = world.runtime().allocate(request);
  WF_CHECK_EQ(unknownDomain.outcome, AllocationOutcome::RefusedUnknownDomain);
  WF_CHECK_EQ(unknownDomain.status.code, StatusCode::NotFound);

  request = world.request(1);
  request.gridGeneration = GridGeneration(2);
  const AllocationDecision staleGrid = world.runtime().allocate(request);
  WF_CHECK_EQ(staleGrid.outcome, AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK_EQ(staleGrid.status.code, StatusCode::StaleGeneration);

  request = world.request(1);
  request.domainGenerations = {SpectrumDomainGeneration(2)};
  const AllocationDecision staleDomain = world.runtime().allocate(request);
  WF_CHECK_EQ(staleDomain.outcome, AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK_EQ(staleDomain.status.code, StatusCode::StaleGeneration);

  WF_CHECK(world.runtime().reservations().empty());
}

WF_TEST(a_domain_without_a_capability_is_refused) {
  World world;
  (void)world.runtime().registerDomain(wf_test::makeDomain(SpectrumDomainId(9), ChannelGridId(1)));
  SpectrumRequest request = world.request(1);
  request.domains = {SpectrumDomainId(9)};
  request.domainGenerations = {SpectrumDomainGeneration(1)};
  const AllocationDecision decision = world.runtime().allocate(request);
  WF_CHECK(!decision.allocated());
  WF_CHECK_EQ(decision.status.code, StatusCode::Unknown);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedUnknownCapability);
  WF_CHECK(world.runtime().reservations().empty());
}

WF_TEST(unsupported_and_unknown_capabilities_are_never_approximated) {
  World world;
  for (std::uint64_t value = 4; value <= 5; ++value) {
    (void)world.runtime().registerDomain(
        wf_test::makeDomain(SpectrumDomainId(value), ChannelGridId(1)));
  }
  SpectrumCapability unsupported = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unsupported, 0, 0, world.runtime().fence());
  WF_CHECK_EQ(world.runtime().publishCapability(unsupported).code, StatusCode::Ok);
  SpectrumCapability unknown = wf_test::makeCapability(
      SpectrumDomainId(5), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unknown, 0, 0, world.runtime().fence());
  WF_CHECK_EQ(world.runtime().publishCapability(unknown).code, StatusCode::Ok);

  SpectrumRequest request = world.request(1, kStart, SpectrumDomainId(4));
  AllocationDecision decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedUnsupported);
  WF_CHECK_EQ(decision.status.code, StatusCode::Unsupported);

  request = world.request(1, kStart, SpectrumDomainId(5));
  decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedUnknownCapability);
  WF_CHECK_EQ(decision.status.code, StatusCode::Unknown);

  WF_CHECK(world.runtime().reservations().empty());

  SpectrumCapability lying = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unsupported, 0, 8, world.runtime().fence());
  WF_CHECK_EQ(world.runtime().publishCapability(lying).code, StatusCode::InvalidArgument);

  SpectrumCapability unevidenced = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 8, world.runtime().fence());
  unevidenced.presenceEvidence.present = false;
  unevidenced.presenceEvidence.digest = 0;
  WF_CHECK_EQ(world.runtime().publishCapability(unevidenced).code, StatusCode::InvalidArgument);

  SpectrumCapability outside = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, kSlots + 1, world.runtime().fence());
  outside.minTunableMhz = kAnchorMhz;
  outside.maxTunableMhz = kEndMhz;
  WF_CHECK_EQ(world.runtime().publishCapability(outside).code, StatusCode::InvalidArgument);

  SpectrumCapability mistuned = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, kSlots, world.runtime().fence());
  mistuned.minTunableMhz = kAnchorMhz - 1;
  mistuned.maxTunableMhz = kEndMhz;
  WF_CHECK_EQ(world.runtime().publishCapability(mistuned).code, StatusCode::InvalidArgument);

  SpectrumCapability converting = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, kSlots, world.runtime().fence());
  converting.minTunableMhz = kAnchorMhz;
  converting.maxTunableMhz = kEndMhz;
  converting.conversionSupported = true;
  // Domain 4 already carries generation 1 from the UNSUPPORTED publication.
  converting.generation = CapabilityGeneration(2);
  WF_CHECK_EQ(world.runtime().publishCapability(converting).code, StatusCode::InvalidArgument);
  converting.conversionEvidence.present = true;
  converting.conversionEvidence.digest = 0x1234;
  converting.conversionEvidence.source = "evidence";
  WF_CHECK_EQ(world.runtime().publishCapability(converting).code, StatusCode::Ok);
  WF_CHECK_EQ(world.runtime().publishCapability(converting).code, StatusCode::Duplicate);
  converting.generation = CapabilityGeneration(0);
  WF_CHECK_EQ(world.runtime().publishCapability(converting).code, StatusCode::InvalidArgument);

  // A publication minted by a dead incarnation is rejected even though its
  // domain, grid and generation are current.
  SpectrumCapability stale = wf_test::makeCapability(
      SpectrumDomainId(4), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, kSlots, world.runtime().fence());
  stale.minTunableMhz = kAnchorMhz;
  stale.maxTunableMhz = kEndMhz;
  stale.generation = CapabilityGeneration(9);
  stale.fence.incarnation = ControllerIncarnation(world.runtime().fence().incarnation.raw() + 1);
  WF_CHECK_EQ(world.runtime().publishCapability(stale).code, StatusCode::StaleIncarnation);
  stale.fence = world.runtime().fence();
  stale.fence.epoch = ControllerEpoch(world.runtime().fence().epoch.raw() + 1);
  WF_CHECK_EQ(world.runtime().publishCapability(stale).code, StatusCode::StaleEpoch);
}

// ---------------------------------------------------------------------------
// Identity, replay and supersede
// ---------------------------------------------------------------------------

WF_TEST(replay_is_idempotent_and_supersede_is_exact) {
  World world;
  SpectrumRequest request = world.request(2);
  request.maxRenewals = 0;

  const AllocationDecision first = world.runtime().allocate(request);
  WF_REQUIRE(first.allocated());
  const std::size_t count = world.runtime().reservations().size();
  WF_CHECK_EQ(count, std::size_t(1));
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(1));

  const AllocationDecision replay = world.runtime().allocate(request);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.status.ok());
  WF_CHECK_EQ(replay.outcome, AllocationOutcome::Allocated);
  WF_CHECK(replay.reservation == first.reservation);
  WF_CHECK(replay.generation == first.generation);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(1));
  WF_CHECK_EQ(world.runtime().reservations().size(), count);
  WF_CHECK_EQ(world.runtime().explain(request).reservation.raw(), first.reservation.raw());

  SpectrumRequest different = request;
  different.slots = 4;
  different.contiguity = ContiguityRequirement::Required;
  const AllocationDecision clash = world.runtime().allocate(different);
  WF_CHECK(!clash.allocated());
  WF_CHECK_EQ(clash.outcome, AllocationOutcome::RefusedDuplicate);
  WF_CHECK_EQ(clash.status.code, StatusCode::Duplicate);
  WF_CHECK_EQ(world.runtime().reservations().size(), count);

  // A newer generation of the same request identity supersedes the live one.
  SpectrumRequest newer = different;
  newer.requestGeneration = AllocationRequestGeneration(2);
  newer.owner = OwnerId(2);
  const AllocationDecision superseding = world.runtime().allocate(newer);
  WF_REQUIRE(superseding.allocated());
  WF_CHECK(superseding.reservation != first.reservation);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(2));
  WF_CHECK_EQ(world.runtime().reservations().size(), std::size_t(2));

  const std::optional<SpectrumReservation> superseded =
      world.runtime().reservation(first.reservation);
  WF_REQUIRE(superseded.has_value());
  WF_CHECK_EQ(superseded->state, ReservationState::Superseded);
  WF_CHECK(!superseded->isLiveAt(kStart));
  const std::optional<SpectrumReservation> live =
      world.runtime().reservation(superseding.reservation);
  WF_REQUIRE(live.has_value());
  WF_CHECK_EQ(live->state, ReservationState::Reserved);
  WF_CHECK_EQ(live->slots.count, std::uint32_t(4));

  const std::optional<SpectrumUsage> usage = world.runtime().usage(SpectrumDomainId(1), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t(4));
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t(1));

  // Replaying the superseded generation is stale, and the current generation is
  // still an idempotent replay.
  SpectrumRequest rolledBack = newer;
  rolledBack.requestGeneration = AllocationRequestGeneration(1);
  const AllocationDecision old = world.runtime().allocate(rolledBack);
  WF_CHECK(!old.allocated());
  WF_CHECK_EQ(old.outcome, AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK_EQ(old.status.code, StatusCode::StaleGeneration);

  const AllocationDecision current = world.runtime().allocate(newer);
  WF_CHECK(current.allocated());
  WF_CHECK(current.reservation == superseding.reservation);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(2));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WF_TEST(lifecycle_transitions_are_exact) {
  World world;
  SpectrumRequest request = world.request(2);
  request.maxRenewals = 2;
  const AllocationDecision decision = world.runtime().allocate(request);
  WF_REQUIRE(decision.allocated());
  const ReservationId id = decision.reservation;
  ReservationGeneration generation = decision.generation;

  const std::optional<SpectrumReservation> created = world.runtime().reservation(id);
  WF_REQUIRE(created.has_value());
  WF_CHECK_EQ(created->state, ReservationState::Reserved);
  WF_CHECK_EQ(created->lease.grantedAt.nanos(), kStart.nanos());
  WF_CHECK_EQ(created->lease.expiresAt.nanos(), (kStart + kLease).nanos());
  WF_CHECK_EQ(created->lease.renewalCount, std::uint32_t(0));

  WF_CHECK_EQ(world.runtime()
                  .activate(id, generation, world.fixture.activation(), kStart)
                  .code,
              StatusCode::Ok);
  WF_CHECK_EQ(world.runtime().reservation(id)->state, ReservationState::Active);
  WF_CHECK_EQ(world.runtime()
                  .activate(id, generation, world.fixture.activation(), kStart)
                  .code,
              StatusCode::IllegalTransition);
  WF_CHECK_EQ(world.runtime().stats().activations, std::uint64_t(1));

  WF_CHECK_EQ(world.runtime()
                  .deactivate(id, generation, world.fixture.activation(), kStart)
                  .code,
              StatusCode::Ok);
  WF_CHECK_EQ(world.runtime().reservation(id)->state, ReservationState::Reserved);
  WF_CHECK_EQ(world.runtime()
                  .deactivate(id, generation, world.fixture.activation(), kStart)
                  .code,
              StatusCode::IllegalTransition);
  WF_CHECK_EQ(world.runtime().stats().deactivations, std::uint64_t(1));

  WF_CHECK_EQ(world.runtime()
                  .renew(id, generation, Duration::zero(), world.fixture.reservation(), kStart, 0)
                  .code,
              StatusCode::InvalidArgument);
  WF_CHECK_EQ(world.runtime()
                  .renew(id, generation, Duration::nanos(-1), world.fixture.reservation(), kStart, 0)
                  .code,
              StatusCode::InvalidArgument);
  WF_CHECK_EQ(
      world.runtime()
          .renew(id, generation, Duration::seconds(3650ll * 24ll * 60ll * 60ll + 1),
                 world.fixture.reservation(), kStart, 0)
          .code,
      StatusCode::InvalidArgument);

  WF_CHECK_EQ(world.runtime()
                  .renew(id, generation, Duration::seconds(300), world.fixture.reservation(), kStart,
                         0)
                  .code,
              StatusCode::Ok);
  generation = ReservationGeneration(generation.raw() + 1);
  const std::optional<SpectrumReservation> renewed = world.runtime().reservation(id);
  WF_REQUIRE(renewed.has_value());
  WF_CHECK_EQ(renewed->generation, generation);
  WF_CHECK_EQ(renewed->lease.renewalCount, std::uint32_t(1));
  WF_CHECK_EQ(renewed->lease.expiresAt.nanos(), (kStart + kLease + Duration::seconds(300)).nanos());

  const ReservationGeneration previous = ReservationGeneration(generation.raw() - 1);
  WF_CHECK_EQ(world.runtime()
                  .renew(id, previous, Duration::seconds(300), world.fixture.reservation(), kStart, 0)
                  .code,
              StatusCode::StaleGeneration);

  WF_CHECK_EQ(world.runtime()
                  .renew(id, generation, Duration::seconds(300), world.fixture.reservation(), kStart,
                         0)
                  .code,
              StatusCode::Ok);
  generation = ReservationGeneration(generation.raw() + 1);
  WF_CHECK_EQ(world.runtime()
                  .renew(id, generation, Duration::seconds(300), world.fixture.reservation(), kStart,
                         0)
                  .code,
              StatusCode::LimitExceeded);
  WF_CHECK_EQ(world.runtime().stats().renewals, std::uint64_t(2));

  WF_CHECK_EQ(
      world.runtime().release(id, generation, world.fixture.release(), kStart).code, StatusCode::Ok);
  WF_CHECK_EQ(world.runtime().reservation(id)->state, ReservationState::Released);
  WF_CHECK_EQ(
      world.runtime().release(id, generation, world.fixture.release(), kStart).code,
      StatusCode::IllegalTransition);
  WF_CHECK_EQ(world.runtime().stats().releases, std::uint64_t(1));

  const std::optional<SpectrumUsage> usage = world.runtime().usage(SpectrumDomainId(1), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t(0));
  WF_CHECK_EQ(usage->freeSlots, kSlots);
  WF_CHECK_EQ(usage->releasedReservations, std::uint32_t(1));
}

WF_TEST(a_lapsed_lease_cannot_be_renewed_or_activated_but_can_be_released) {
  World world;
  const AllocationDecision decision = world.runtime().allocate(world.request(2));
  WF_REQUIRE(decision.allocated());
  const ReservationId id = decision.reservation;

  WF_CHECK_EQ(world.runtime()
                  .renew(id, decision.generation, Duration::seconds(300),
                         world.fixture.reservation(), kLate, 0)
                  .code,
              StatusCode::Refused);
  WF_CHECK_EQ(world.runtime()
                  .activate(id, decision.generation, world.fixture.activation(), kLate)
                  .code,
              StatusCode::Refused);
  WF_CHECK_EQ(world.runtime().stats().renewals, std::uint64_t(0));
  WF_CHECK_EQ(world.runtime().reservation(id)->lease.renewalCount, std::uint32_t(0));

  // A lapsed lease still owns nothing, so a fresh request can take its slots
  // before any sweep runs.
  const AllocationDecision overlapping = world.runtime().allocate(world.request(2));
  WF_REQUIRE(overlapping.allocated());
  WF_CHECK_EQ(world.runtime().reservation(overlapping.reservation)->slots.first, std::uint32_t(2));

  // Release does not depend on the lease, only on the state.
  WF_CHECK_EQ(
      world.runtime().release(id, decision.generation, world.fixture.release(), kLate).code,
      StatusCode::Ok);
  WF_CHECK_EQ(world.runtime().reservation(id)->state, ReservationState::Released);

  // Before any sweep the second reservation still owns its slots but its lease
  // has lapsed, which is accounted separately from live ownership.
  const std::optional<SpectrumUsage> lapsed = world.runtime().usage(SpectrumDomainId(1), kLate);
  WF_REQUIRE(lapsed.has_value());
  WF_CHECK_EQ(lapsed->liveSlots, std::uint32_t(0));
  WF_CHECK_EQ(lapsed->lapsedSlots, std::uint32_t(2));
  WF_CHECK_EQ(lapsed->lapsedReservations, std::uint32_t(1));

  // Expiry and reclamation accounting is exact.
  const ReclaimReport expired = world.runtime().expireLeases(kLate);
  WF_CHECK_EQ(expired.status.code, StatusCode::Ok);
  WF_CHECK_EQ(expired.evaluatedAt.nanos(), kLate.nanos());
  WF_CHECK_EQ(expired.scanned, world.runtime().reservations().size());
  WF_CHECK_EQ(expired.lapsed.size(), std::size_t(1));
  WF_CHECK_EQ(expired.lapsed[0].raw(), overlapping.reservation.raw());
  WF_CHECK_EQ(expired.expired, std::size_t(1));
  WF_CHECK(expired.reclaimed.empty());
  WF_CHECK_EQ(world.runtime().reservation(overlapping.reservation)->state,
              ReservationState::Expired);

  WF_CHECK_EQ(world.runtime().usage(SpectrumDomainId(1), kLate)->lapsedSlots, std::uint32_t(0));

  const ReclaimReport reclaimed = world.runtime().reclaimExpired(kLate);
  WF_CHECK_EQ(reclaimed.status.code, StatusCode::Ok);
  WF_CHECK_EQ(reclaimed.lapsed.size(), std::size_t(0));
  WF_CHECK_EQ(reclaimed.reclaimed.size(), std::size_t(1));
  WF_CHECK_EQ(reclaimed.reclaimed[0].raw(), overlapping.reservation.raw());
  WF_CHECK_EQ(world.runtime().stats().reclamations, std::uint64_t(1));

  const std::optional<SpectrumUsage> after = world.runtime().usage(SpectrumDomainId(1), kLate);
  WF_REQUIRE(after.has_value());
  WF_CHECK_EQ(after->liveSlots, std::uint32_t(0));
  WF_CHECK_EQ(after->lapsedSlots, std::uint32_t(0));
  WF_CHECK_EQ(after->freeSlots, kSlots);
  WF_CHECK_EQ(after->reclaimedReservations, std::uint32_t(1));
  WF_CHECK_EQ(after->releasedReservations, std::uint32_t(1));
  WF_CHECK_EQ(after->freeRuns.size(), std::size_t(1));
}

WF_TEST(operations_on_reservations_that_never_existed_are_not_found) {
  World world;
  const ReservationId absent(4242);
  const ReservationGeneration generation(1);
  WF_CHECK_EQ(world.runtime().activate(absent, generation, world.fixture.activation(), kStart).code,
              StatusCode::NotFound);
  WF_CHECK_EQ(world.runtime().deactivate(absent, generation, world.fixture.activation(), kStart).code,
              StatusCode::NotFound);
  WF_CHECK_EQ(
      world.runtime()
          .renew(absent, generation, Duration::seconds(300), world.fixture.reservation(), kStart, 0)
          .code,
      StatusCode::NotFound);
  WF_CHECK_EQ(world.runtime().release(absent, generation, world.fixture.release(), kStart).code,
              StatusCode::NotFound);
  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().renewals, std::uint64_t(0));
  WF_CHECK_EQ(world.runtime().stats().releases, std::uint64_t(0));

  // A mismatched generation on an existing reservation is stale, not illegal.
  const AllocationDecision decision = world.runtime().allocate(world.request(2));
  WF_REQUIRE(decision.allocated());
  const ReservationGeneration wrong(decision.generation.raw() + 7);
  WF_CHECK_EQ(
      world.runtime().release(decision.reservation, wrong, world.fixture.release(), kStart).code,
      StatusCode::StaleGeneration);
  WF_CHECK_EQ(world.runtime()
                  .activate(decision.reservation, wrong, world.fixture.activation(), kStart)
                  .code,
              StatusCode::StaleGeneration);
  WF_CHECK_EQ(world.runtime()
                  .deactivate(decision.reservation, wrong, world.fixture.activation(), kStart)
                  .code,
              StatusCode::StaleGeneration);
  WF_CHECK_EQ(world.runtime()
                  .renew(decision.reservation, wrong, Duration::seconds(300),
                         world.fixture.reservation(), kStart, 0)
                  .code,
              StatusCode::StaleGeneration);
  WF_CHECK_EQ(world.runtime().reservation(decision.reservation)->state,
              ReservationState::Reserved);
}

WF_TEST(stale_authority_tokens_are_refused_at_the_exact_field) {
  World world;
  const ControllerFence fence = world.runtime().fence();

  SpectrumRequest request = world.request(1);

  EligibilityAuthority token = world.fixture.eligibility();
  token.fence.incarnation = ControllerIncarnation(fence.incarnation.raw() + 1);
  request.eligibilityAuthority = token;
  AllocationDecision decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.status.code, StatusCode::StaleIncarnation);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedStaleIncarnation);

  token = world.fixture.eligibility();
  token.fence.epoch = ControllerEpoch(fence.epoch.raw() + 1);
  request.eligibilityAuthority = token;
  decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.status.code, StatusCode::StaleEpoch);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedStaleEpoch);

  token = world.fixture.eligibility();
  token.generation = EligibilityAuthorityGeneration(2);
  request.eligibilityAuthority = token;
  decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.status.code, StatusCode::StaleGeneration);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedStaleGeneration);

  token = world.fixture.eligibility();
  token.generation = EligibilityAuthorityGeneration(0);
  request.eligibilityAuthority = token;
  decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.status.code, StatusCode::InvalidArgument);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedInvalidRequest);

  request = world.request(1);
  ReservationAuthority reservation = world.fixture.reservation();
  reservation.fence.incarnation = ControllerIncarnation(fence.incarnation.raw() + 1);
  request.reservationAuthority = reservation;
  decision = world.runtime().allocate(request);
  WF_CHECK_EQ(decision.status.code, StatusCode::StaleIncarnation);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::RefusedStaleIncarnation);

  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));

  // The lifecycle paths are fenced the same way.
  const AllocationDecision good = world.runtime().allocate(world.request(2));
  WF_REQUIRE(good.allocated());
  ReservationAuthority renewal = world.fixture.reservation();
  renewal.generation = ReservationAuthorityGeneration(2);
  WF_CHECK_EQ(world.runtime()
                  .renew(good.reservation, good.generation, Duration::seconds(300), renewal, kStart,
                         0)
                  .code,
              StatusCode::StaleGeneration);
  ActivationAuthority activation = world.fixture.activation();
  activation.fence.incarnation = ControllerIncarnation(fence.incarnation.raw() + 1);
  WF_CHECK_EQ(world.runtime().activate(good.reservation, good.generation, activation, kStart).code,
              StatusCode::StaleIncarnation);
  ReleaseAuthority release = world.fixture.release();
  release.fence.epoch = ControllerEpoch(fence.epoch.raw() + 5);
  WF_CHECK_EQ(world.runtime().release(good.reservation, good.generation, release, kStart).code,
              StatusCode::StaleEpoch);
  WF_CHECK_EQ(world.runtime().reservation(good.reservation)->state, ReservationState::Reserved);
}

// ---------------------------------------------------------------------------
// Configured limits
// ---------------------------------------------------------------------------

WF_TEST(the_reservation_limit_is_enforced_at_the_configured_bound) {
  RuntimeConfig config;
  config.maxReservations = 2;
  World world(kSlots, config);
  WF_CHECK_EQ(world.runtime().config().maxReservations, std::uint32_t(2));

  const AllocationDecision first = world.runtime().allocate(world.request(1));
  const AllocationDecision second = world.runtime().allocate(world.request(1));
  WF_REQUIRE(first.allocated());
  WF_REQUIRE(second.allocated());
  WF_CHECK_EQ(first.reservation.raw(), std::uint64_t(1));
  WF_CHECK_EQ(second.reservation.raw(), std::uint64_t(2));

  const AllocationDecision third = world.runtime().allocate(world.request(1));
  WF_CHECK(!third.allocated());
  WF_CHECK_EQ(third.outcome, AllocationOutcome::RefusedLimitExceeded);
  WF_CHECK_EQ(third.status.code, StatusCode::LimitExceeded);
  WF_CHECK(!third.explanation.reasons.empty());
  WF_CHECK_EQ(world.runtime().reservations().size(), std::size_t(2));
  WF_CHECK_EQ(world.runtime().stats().allocationsRefused, std::uint64_t(1));

  // A table full of live records refuses the next commit.
  WF_CHECK(world.runtime().stats().releases == 0);

  // Releasing a record makes room again: the oldest terminal record is pruned
  // to admit the commit, and the audit trail keeps its history.
  const AuditSequence beforePrune = world.runtime().lastAuditSequence();
  WF_REQUIRE(world.runtime()
                 .release(first.reservation, first.generation, world.fixture.release(), kStart)
                 .ok());
  const AllocationDecision fourth = world.runtime().allocate(world.request(1));
  WF_CHECK(fourth.allocated());
  WF_CHECK_EQ(fourth.outcome, AllocationOutcome::Allocated);
  WF_CHECK_EQ(world.runtime().reservations().size(), std::size_t(2));
  // The released record was the oldest terminal one and is gone; the live
  // reservations remain and the new one is present.
  WF_CHECK(!world.runtime().reservation(first.reservation).has_value());
  WF_CHECK(world.runtime().reservation(second.reservation).has_value());
  WF_CHECK(world.runtime().reservation(fourth.reservation).has_value());
  WF_CHECK_EQ(world.runtime().reservation(fourth.reservation)->state,
              ReservationState::Reserved);
  WF_CHECK(world.runtime().lastAuditSequence() > beforePrune);
  const std::vector<AuditRecord> history = world.runtime().audit(AuditSequence(0), 4096);
  bool sawReleased = false;
  bool sawCommitted = false;
  for (const AuditRecord& record : history) {
    if (record.kind == AuditKind::ReservationReleased &&
        record.reservation == first.reservation) {
      sawReleased = true;
    }
    if (record.kind == AuditKind::AllocationCommitted &&
        record.reservation == fourth.reservation) {
      sawCommitted = true;
    }
  }
  WF_CHECK(sawReleased);
  WF_CHECK(sawCommitted);
  WF_CHECK_EQ(world.runtime().stats().releases, 1);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(3));
}

WF_TEST(the_audit_history_is_bounded_while_sequences_keep_increasing) {
  RuntimeConfig config;
  config.maxAuditRecords = 4;
  World world(kSlots, config);

  const std::uint64_t firstSequence = world.runtime().lastAuditSequence().raw();
  WF_CHECK(firstSequence >= 8);
  WF_CHECK_EQ(world.runtime().auditSize(), std::size_t(4));
  std::vector<AuditRecord> records = world.runtime().audit(AuditSequence(0), 4096);
  WF_REQUIRE(records.size() == 4);
  for (std::size_t index = 0; index < records.size(); ++index) {
    WF_CHECK_EQ(records[index].sequence.raw(), firstSequence - 3 + index);
  }

  for (int attempt = 0; attempt < 6; ++attempt) {
    (void)world.runtime().allocate(world.request(1));
  }
  WF_CHECK_EQ(world.runtime().auditSize(), std::size_t(4));
  WF_CHECK(world.runtime().lastAuditSequence().raw() > firstSequence);
  records = world.runtime().audit(AuditSequence(0), 4096);
  WF_REQUIRE(records.size() == 4);
  const std::uint64_t last = world.runtime().lastAuditSequence().raw();
  for (std::size_t index = 0; index < records.size(); ++index) {
    WF_CHECK_EQ(records[index].sequence.raw(), last - 3 + index);
  }
  // Paging from a sequence returns only strictly newer records.
  const std::vector<AuditRecord> tail = world.runtime().audit(records[2].sequence, 4096);
  WF_CHECK_EQ(tail.size(), std::size_t(1));
  WF_CHECK_EQ(tail[0].sequence.raw(), last);
}

WF_TEST(candidate_enumeration_truncation_is_exactly_accounted) {
  constexpr std::uint32_t kHugeSlots = kMaxGridSlots;
  RuntimeConfig config;
  SpectrumRuntime runtime(config);
  const ChannelGrid grid = wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), 1, 1, kHugeSlots, 1,
                                             kMaxChannelSlotsPerChannel);
  WF_CHECK_EQ(runtime.registerGrid(grid).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.registerDomain(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1))).code,
              StatusCode::Ok);
  SpectrumCapability capability =
      wf_test::makeCapability(SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1),
                              GridGeneration(1), SpectrumSupport::Supported, 0, kHugeSlots,
                              runtime.fence());
  capability.minTunableMhz = 1;
  capability.maxTunableMhz = 1 + static_cast<std::int64_t>(kHugeSlots);
  WF_CHECK_EQ(runtime.publishCapability(capability).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.config().maxCandidatesPerRequest, kMaxCandidatesPerRequest);

  const SpectrumRequest request = wf_test::makeRequest(
      AllocationRequestId(1), OwnerId(1), {SpectrumDomainId(1)}, {SpectrumDomainGeneration(1)},
      ChannelGridId(1), GridGeneration(1), 1, kStart, kLease, eligibilityFor(runtime),
      reservationFor(runtime));
  const std::uint64_t totalStarts = kHugeSlots;

  const CandidateSet set = runtime.enumerateCandidates(request);
  WF_CHECK_EQ(set.status.code, StatusCode::Ok);
  WF_CHECK_EQ(set.candidates.size(), static_cast<std::size_t>(kMaxCandidatesPerRequest));
  WF_CHECK_EQ(set.eligibleCount, static_cast<std::size_t>(kMaxCandidatesPerRequest));
  WF_CHECK_EQ(set.omitted, static_cast<std::size_t>(totalStarts) - set.candidates.size());
  WF_CHECK(!set.complete);
  for (std::size_t index = 0; index < set.candidates.size(); ++index) {
    WF_CHECK_EQ(set.candidates[index].ordinal, index);
  }

  const AllocationDecision decision = runtime.allocate(request);
  WF_REQUIRE(decision.allocated());
  WF_CHECK_EQ(decision.explanation.selected.slots.first, std::uint32_t(0));
  WF_CHECK_EQ(decision.explanation.candidatesOmitted, set.omitted);
  WF_CHECK_EQ(runtime.stats().enumerationTruncations, std::uint64_t(1));
  WF_CHECK_EQ(runtime.stats().candidatesEvaluated, std::uint64_t(kMaxCandidatesPerRequest));

  // A configured bound of one keeps the list to its prefix and reports the rest
  // as omitted, and zero is clamped to one rather than meaning "unlimited".
  RuntimeConfig bounded;
  bounded.maxCandidatesPerRequest = 1;
  SpectrumRuntime small(bounded);
  WF_CHECK_EQ(small.config().maxCandidatesPerRequest, std::uint32_t(1));
  WF_CHECK_EQ(small.registerGrid(grid).code, StatusCode::Ok);
  WF_CHECK_EQ(small.registerDomain(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1))).code,
              StatusCode::Ok);
  capability.fence = small.fence();
  WF_CHECK_EQ(small.publishCapability(capability).code, StatusCode::Ok);
  const SpectrumRequest smallRequest = wf_test::makeRequest(
      AllocationRequestId(1), OwnerId(1), {SpectrumDomainId(1)}, {SpectrumDomainGeneration(1)},
      ChannelGridId(1), GridGeneration(1), 1, kStart, kLease, eligibilityFor(small),
      reservationFor(small));
  const CandidateSet smallSet = small.enumerateCandidates(smallRequest);
  WF_CHECK_EQ(smallSet.candidates.size(), std::size_t(1));
  WF_CHECK_EQ(smallSet.omitted, static_cast<std::size_t>(totalStarts) - 1);
  WF_CHECK(!smallSet.complete);

  RuntimeConfig clamped;
  clamped.maxCandidatesPerRequest = 0;
  SpectrumRuntime clamping(clamped);
  WF_CHECK_EQ(clamping.config().maxCandidatesPerRequest, std::uint32_t(1));
  RuntimeConfig oversized;
  oversized.maxCandidatesPerRequest = 1'000'000;
  SpectrumRuntime clampingLarge(oversized);
  WF_CHECK_EQ(clampingLarge.config().maxCandidatesPerRequest, kMaxCandidatesPerRequest);
}

WF_TEST(reset_returns_every_counter_to_its_initial_value) {
  World world;
  const AllocationDecision decision = world.runtime().allocate(world.request(2));
  WF_REQUIRE(decision.allocated());
  WF_CHECK(world.runtime().auditSize() > 1);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(1));

  WF_CHECK_EQ(world.runtime().reset().code, StatusCode::Ok);
  WF_CHECK(world.runtime().grids().empty());
  WF_CHECK(world.runtime().domains().empty());
  WF_CHECK(world.runtime().exclusionDomains().empty());
  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK(!world.runtime().reservation(decision.reservation).has_value());
  WF_CHECK(!world.runtime().usage(SpectrumDomainId(1), kStart).has_value());
  WF_CHECK_EQ(world.runtime().auditSize(), std::size_t(1));
  WF_CHECK_EQ(world.runtime().lastAuditSequence().raw(), std::uint64_t(1));
  const std::vector<AuditRecord> records = world.runtime().audit(AuditSequence(0), 16);
  WF_REQUIRE(records.size() == 1);
  WF_CHECK_EQ(records[0].kind, AuditKind::StateReset);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));
  WF_CHECK_EQ(world.runtime().stats().allocationsRefused, std::uint64_t(0));
  WF_CHECK_EQ(world.runtime().stats().releases, std::uint64_t(0));
  WF_CHECK(world.runtime().stats().persistenceWrites == 0);

  // The runtime stays usable and restarts its identity space.
  WF_CHECK_EQ(world.runtime()
                  .registerGrid(wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), kAnchorMhz,
                                                  kSlotWidthMhz, kSlots, 1, 8))
                  .code,
              StatusCode::Ok);
  WF_CHECK_EQ(world.runtime()
                  .registerDomain(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1)))
                  .code,
              StatusCode::Ok);
  SpectrumCapability capability = wf_test::makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, kSlots, world.runtime().fence());
  capability.minTunableMhz = kAnchorMhz;
  capability.maxTunableMhz = kEndMhz;
  WF_CHECK_EQ(world.runtime().publishCapability(capability).code, StatusCode::Ok);
  const AllocationDecision restarted = world.runtime().allocate(world.request(2));
  WF_REQUIRE(restarted.allocated());
  WF_CHECK_EQ(restarted.reservation.raw(), std::uint64_t(1));
}

WF_TEST(authority_advance_is_monotone_and_fence_checked) {
  World world;
  AuthorityState next = world.runtime().authorityState();
  WF_CHECK_EQ(world.runtime().advanceAuthority(next).code, StatusCode::Duplicate);

  next.eligibilityGeneration = EligibilityAuthorityGeneration(2);
  WF_CHECK_EQ(world.runtime().advanceAuthority(next).code, StatusCode::Ok);
  WF_CHECK_EQ(world.runtime().authorityState().eligibilityGeneration.raw(), std::uint64_t(2));

  AuthorityState backwards = next;
  backwards.eligibilityGeneration = EligibilityAuthorityGeneration(1);
  WF_CHECK_EQ(world.runtime().advanceAuthority(backwards).code, StatusCode::StaleGeneration);
  WF_CHECK_EQ(world.runtime().authorityState().eligibilityGeneration.raw(), std::uint64_t(2));

  AuthorityState zeroed = next;
  zeroed.reservationGeneration = ReservationAuthorityGeneration(0);
  WF_CHECK_EQ(world.runtime().advanceAuthority(zeroed).code, StatusCode::InvalidArgument);

  AuthorityState wrongIncarnation = next;
  wrongIncarnation.reservationGeneration = ReservationAuthorityGeneration(5);
  wrongIncarnation.fence.incarnation =
      ControllerIncarnation(world.runtime().fence().incarnation.raw() + 1);
  WF_CHECK_EQ(world.runtime().advanceAuthority(wrongIncarnation).code, StatusCode::StaleIncarnation);

  AuthorityState wrongEpoch = next;
  wrongEpoch.releaseGeneration = ReleaseAuthorityGeneration(5);
  wrongEpoch.fence.epoch = ControllerEpoch(world.runtime().fence().epoch.raw() + 1);
  WF_CHECK_EQ(world.runtime().advanceAuthority(wrongEpoch).code, StatusCode::StaleEpoch);

  AuthorityState invalidFence = next;
  invalidFence.activationGeneration = ActivationAuthorityGeneration(5);
  invalidFence.fence = ControllerFence{};
  WF_CHECK_EQ(world.runtime().advanceAuthority(invalidFence).code, StatusCode::InvalidArgument);

  // A token minted before the advance is now stale, and a token minted after it
  // is accepted.
  WF_CHECK_EQ(world.runtime().authorityState().reservationGeneration.raw(), std::uint64_t(1));
  SpectrumRequest request = world.request(1);
  WF_CHECK_EQ(world.runtime().allocate(request).status.code, StatusCode::StaleGeneration);
  request.eligibilityAuthority.generation = EligibilityAuthorityGeneration(2);
  WF_CHECK(request.eligibilityAuthority.valid());
  request.reservationAuthority.fence = world.runtime().fence();
  request.eligibilityAuthority.fence = world.runtime().fence();
  WF_CHECK_EQ(world.runtime().allocate(request).status.code, StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

WF_TEST(persistence_refuses_a_state_path_that_cannot_be_created) {
  const std::filesystem::path missing =
      temporaryDirectory("wf_adversarial_missing") / "nested" / "state.wvl";
  RuntimeConfig config;
  config.statePath = missing.string();
  SpectrumRuntime runtime(config);
  WF_CHECK(!std::filesystem::exists(missing));

  WF_CHECK_EQ(runtime.save().code, StatusCode::IoError);
  WF_CHECK_EQ(runtime.stats().persistenceFailures, std::uint64_t(1));
  WF_CHECK_EQ(runtime.stats().persistenceWrites, std::uint64_t(0));
  WF_CHECK(!std::filesystem::exists(missing));

  const RecoveryReport report = runtime.recover();
  WF_CHECK_EQ(report.status.code, StatusCode::NotFound);
  WF_CHECK(!report.recovered);
  WF_CHECK(!report.diagnostics.empty());
  WF_CHECK_EQ(runtime.stats().corruptionDetections, std::uint64_t(0));

  RuntimeConfig memoryOnly;
  SpectrumRuntime inMemory(memoryOnly);
  WF_CHECK_EQ(inMemory.save().code, StatusCode::InvalidArgument);
  const RecoveryReport noPath = inMemory.recover();
  WF_CHECK_EQ(noPath.status.code, StatusCode::InvalidArgument);
  WF_CHECK(!noPath.recovered);
}

WF_TEST(a_durable_commit_that_cannot_be_written_changes_nothing) {
  const std::filesystem::path missing =
      temporaryDirectory("wf_adversarial_rollback") / "nested" / "state.wvl";
  RuntimeConfig config;
  config.statePath = missing.string();
  config.durableCommits = true;
  World world(kSlots, config);

  const SpectrumRequest request = world.request(1);
  const AllocationDecision decision = world.runtime().allocate(request);
  WF_CHECK(!decision.allocated());
  WF_CHECK_EQ(decision.status.code, StatusCode::IoError);
  WF_CHECK_EQ(decision.outcome, AllocationOutcome::Unknown);
  WF_CHECK(decision.reservation.none());
  WF_CHECK(decision.generation.none());
  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));
  WF_CHECK_EQ(world.runtime().stats().persistenceFailures, std::uint64_t(1));
  const std::optional<SpectrumUsage> usage = world.runtime().usage(SpectrumDomainId(1), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t(0));

  // The rolled-back commit left no request identity behind either: the same
  // request is not a replay and fails exactly the same way.
  const AllocationDecision again = world.runtime().allocate(request);
  WF_CHECK_EQ(again.status.code, StatusCode::IoError);
  WF_CHECK(again.reservation.none());
  WF_CHECK(world.runtime().reservations().empty());
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(0));
}

WF_TEST(persistence_round_trips_and_corruption_is_detected) {
  const std::filesystem::path directory = temporaryDirectory("wf_adversarial_state");
  std::filesystem::create_directories(directory);
  const std::filesystem::path file = directory / "state.wvl";

  std::vector<std::pair<std::uint64_t, std::uint32_t>> saved;
  {
    RuntimeConfig config;
    config.statePath = file.string();
    World world(kSlots, config);
    const AllocationDecision first = world.runtime().allocate(world.request(2));
    const AllocationDecision second = world.runtime().allocate(world.request(4));
    WF_REQUIRE(first.allocated());
    WF_REQUIRE(second.allocated());
    WF_REQUIRE(world.runtime()
                   .activate(second.reservation, second.generation, world.fixture.activation(),
                             kStart)
                   .ok());
    for (const SpectrumReservation& reservation : world.runtime().reservations()) {
      saved.emplace_back(reservation.id.raw(), reservation.slots.count);
    }
    WF_CHECK_EQ(world.runtime().save().code, StatusCode::Ok);
    WF_CHECK_EQ(world.runtime().stats().persistenceWrites, std::uint64_t(1));
    WF_CHECK(std::filesystem::exists(file));
    WF_CHECK(std::filesystem::file_size(file) > 0);
  }

  {
    RuntimeConfig config;
    config.statePath = file.string();
    SpectrumRuntime runtime(config);
    const RecoveryReport report = runtime.recover();
    WF_CHECK_EQ(report.status.code, StatusCode::Ok);
    WF_CHECK(report.recovered);
    WF_CHECK_EQ(report.gridsRestored, std::size_t(1));
    WF_CHECK_EQ(report.domainsRestored, std::size_t(3));
    WF_CHECK_EQ(report.capabilitiesRestored, std::size_t(3));
    WF_CHECK_EQ(report.reservationsRestored, std::size_t(2));
    WF_CHECK(report.auditsRestored >= 1);
    WF_CHECK_EQ(report.demotedFromActive, std::size_t(1));
    WF_CHECK_EQ(report.recordsRejected, std::uint64_t(0));
    WF_CHECK_EQ(runtime.fence().epoch.raw(), std::uint64_t(2));
    WF_CHECK_EQ(runtime.reservations().size(), std::size_t(2));
    WF_CHECK_EQ(runtime.recoveryGeneration().raw(), std::uint64_t(1));

    for (const auto& entry : saved) {
      const std::optional<SpectrumReservation> restored =
          runtime.reservation(ReservationId(entry.first));
      WF_REQUIRE(restored.has_value());
      WF_CHECK_EQ(restored->slots.count, entry.second);
    }
    std::size_t revalidating = 0;
    for (const SpectrumReservation& reservation : runtime.reservations()) {
      WF_CHECK(reservation.state == ReservationState::Reserved);
      if (reservation.needsRevalidation) ++revalidating;
    }
    WF_CHECK_EQ(revalidating, std::size_t(1));

    // The restored ownership blocks the slots it owns.
    const std::optional<SpectrumUsage> usage = runtime.usage(SpectrumDomainId(1), kStart);
    WF_REQUIRE(usage.has_value());
    WF_CHECK_EQ(usage->liveSlots, std::uint32_t(6));
  }

  {
    std::filesystem::resize_file(file, 16);
    RuntimeConfig config;
    config.statePath = file.string();
    SpectrumRuntime runtime(config);
    const RecoveryReport report = runtime.recover();
    WF_CHECK_EQ(report.status.code, StatusCode::Corruption);
    WF_CHECK(!report.recovered);
    WF_CHECK(!report.diagnostics.empty());
    WF_CHECK_EQ(runtime.stats().corruptionDetections, std::uint64_t(1));
    WF_CHECK(runtime.reservations().empty());
  }

  {
    // A file that is not a state container at all is rejected the same way.
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << "not a wavelength fabric state container";
    stream.close();
    RuntimeConfig config;
    config.statePath = file.string();
    SpectrumRuntime runtime(config);
    const RecoveryReport report = runtime.recover();
    WF_CHECK_EQ(report.status.code, StatusCode::Corruption);
    WF_CHECK(!report.recovered);
  }

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

// ---------------------------------------------------------------------------
// Registration boundaries
// ---------------------------------------------------------------------------

WF_TEST(grid_registration_boundaries_are_enforced) {
  SpectrumRuntime runtime;
  const auto refused = [&runtime](ChannelGrid grid, StatusCode expected, const char* what) {
    const Status status = runtime.registerGrid(grid);
    ::wf_test::Harness::instance().noteCheck();
    if (status.code != expected) {
      ::wf_test::Harness::instance().fail(
          __FILE__, __LINE__, std::string(what) + ": actual=" + std::string(toToken(status.code)) +
                                  " expected=" + std::string(toToken(expected)));
    }
  };

  ChannelGrid grid = wf_test::flexGrid();
  grid.id = ChannelGridId(0);
  refused(grid, StatusCode::InvalidArgument, "a zero grid id");
  grid = wf_test::flexGrid();
  grid.generation = GridGeneration(0);
  refused(grid, StatusCode::InvalidArgument, "a zero grid generation");
  grid = wf_test::flexGrid();
  grid.kind = GridKind::Unknown;
  refused(grid, StatusCode::InvalidArgument, "an unknown grid kind");
  grid = wf_test::flexGrid();
  grid.anchorMhz = 0;
  refused(grid, StatusCode::InvalidArgument, "an anchor below the frequency floor");
  grid = wf_test::flexGrid();
  grid.anchorMhz = kMaxFrequencyMhz + 1;
  refused(grid, StatusCode::InvalidArgument, "an anchor above the frequency ceiling");
  grid = wf_test::flexGrid();
  grid.slotWidthMhz = 0;
  refused(grid, StatusCode::InvalidArgument, "a zero slot width");
  grid = wf_test::flexGrid();
  grid.slotWidthMhz = kMaxSlotWidthMhz + 1;
  refused(grid, StatusCode::InvalidArgument, "a slot width above the ceiling");
  grid = wf_test::flexGrid();
  grid.slotCount = 0;
  refused(grid, StatusCode::InvalidArgument, "a zero slot count");
  grid = wf_test::flexGrid();
  grid.slotCount = kMaxGridSlots + 1;
  refused(grid, StatusCode::InvalidArgument, "a slot count above the grid bound");
  grid = wf_test::fixedGrid();
  grid.minSlotsPerChannel = 2;
  refused(grid, StatusCode::InvalidArgument, "a fixed grid with two slots per channel");
  grid = wf_test::flexGrid();
  grid.minSlotsPerChannel = 0;
  refused(grid, StatusCode::InvalidArgument, "a flex grid with a zero minimum");
  grid = wf_test::flexGrid();
  grid.maxSlotsPerChannel = 0;
  refused(grid, StatusCode::InvalidArgument, "a flex grid whose maximum is below its minimum");
  grid = wf_test::flexGrid();
  grid.maxSlotsPerChannel = kMaxSlotsPerChannel + 1;
  refused(grid, StatusCode::InvalidArgument, "a flex grid above the per-channel bound");
  grid = wf_test::flexGrid();
  grid.label = std::string(257, 'x');
  refused(grid, StatusCode::InvalidArgument, "an over-long label");

  // The largest grid the model admits is exactly one million and forty-eight
  // thousand five hundred and seventy-six slots of one megahertz.
  grid = wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), 1, 1, kMaxGridSlots, 1, 8);
  WF_CHECK_EQ(runtime.registerGrid(grid).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.registerGrid(grid).code, StatusCode::Duplicate);
  grid.generation = GridGeneration(2);
  WF_CHECK_EQ(runtime.registerGrid(grid).code, StatusCode::Ok);
  grid.generation = GridGeneration(1);
  WF_CHECK_EQ(runtime.registerGrid(grid).code, StatusCode::StaleGeneration);
}

WF_TEST(domain_port_and_exclusion_registration_boundaries_are_enforced) {
  SpectrumRuntime runtime;
  const ChannelGrid grid = wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), kAnchorMhz,
                                             kSlotWidthMhz, kSlots, 1, 8);
  WF_CHECK_EQ(runtime.registerGrid(grid).code, StatusCode::Ok);

  SpectrumDomain domain = wf_test::makeDomain(SpectrumDomainId(0), ChannelGridId(1));
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  domain.generation = SpectrumDomainGeneration(0);
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  domain.klass = ResourceClass::Unknown;
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(0));
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(9));
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::NotFound);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1),
                               GridGeneration(2));
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::StaleGeneration);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  domain.label = std::string(257, 'x');
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  domain.span = SpanId(4);
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);
  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  domain.portA = PortId(4);
  domain.portAGeneration = PortGeneration(1);
  domain.portB = PortId(4);
  domain.portBGeneration = PortGeneration(1);
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::InvalidArgument);

  domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::Duplicate);
  domain.generation = SpectrumDomainGeneration(2);
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::Ok);
  domain.generation = SpectrumDomainGeneration(1);
  WF_CHECK_EQ(runtime.registerDomain(domain).code, StatusCode::StaleGeneration);

  Span span;
  span.id = SpanId(0);
  WF_CHECK_EQ(runtime.registerSpan(span).code, StatusCode::InvalidArgument);
  span.id = SpanId(1);
  span.generation = SpanGeneration(0);
  WF_CHECK_EQ(runtime.registerSpan(span).code, StatusCode::InvalidArgument);
  span.generation = SpanGeneration(1);
  span.label = std::string(257, 'x');
  WF_CHECK_EQ(runtime.registerSpan(span).code, StatusCode::InvalidArgument);
  span.label = "span";
  WF_CHECK_EQ(runtime.registerSpan(span).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.registerSpan(span).code, StatusCode::Duplicate);

  OpticalPort port;
  port.id = PortId(0);
  WF_CHECK_EQ(runtime.registerPort(port).code, StatusCode::InvalidArgument);
  port.id = PortId(1);
  port.generation = PortGeneration(1);
  port.label = "port";
  WF_CHECK_EQ(runtime.registerPort(port).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.registerPort(port).code, StatusCode::Duplicate);

  ExclusionDomain exclusion;
  exclusion.id = ExclusionDomainId(0);
  exclusion.generation = ExclusionDomainGeneration(1);
  exclusion.members = {SpectrumDomainId(1)};
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.id = ExclusionDomainId(1);
  exclusion.generation = ExclusionDomainGeneration(0);
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.generation = ExclusionDomainGeneration(1);
  exclusion.members.clear();
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.members = {SpectrumDomainId(1), SpectrumDomainId(1)};
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.members = {SpectrumDomainId(2), SpectrumDomainId(1)};
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.members = {SpectrumDomainId(0)};
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.members = {SpectrumDomainId(9999)};
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::NotFound);
  exclusion.members = {SpectrumDomainId(1)};
  exclusion.guardBandMhz = -1;
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.guardBandMhz = kMaxGuardBandMhz + 1;
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::InvalidArgument);
  exclusion.guardBandMhz = kMaxGuardBandMhz;
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::Ok);
  WF_CHECK_EQ(runtime.registerExclusionDomain(exclusion).code, StatusCode::Duplicate);
  WF_CHECK(runtime.exclusionDomain(ExclusionDomainId(1)).has_value());
  WF_CHECK(exclusionDomainContains(*runtime.exclusionDomain(ExclusionDomainId(1)),
                                   SpectrumDomainId(1)));
  WF_CHECK(!exclusionDomainContains(*runtime.exclusionDomain(ExclusionDomainId(1)),
                                    SpectrumDomainId(2)));
}

WF_TEST(explain_and_enumerate_never_mutate_ownership) {
  World world;
  const AllocationDecision decision = world.runtime().allocate(world.request(2));
  WF_REQUIRE(decision.allocated());
  const std::string before = describeRuntime(world.runtime().stats());
  const std::size_t count = world.runtime().reservations().size();

  const SpectrumRequest request = world.request(2);
  for (int pass = 0; pass < 3; ++pass) {
    const DecisionExplanation explanation = world.runtime().explain(request);
    WF_CHECK_EQ(explanation.outcome, AllocationOutcome::Allocated);
    WF_CHECK(explanation.selected.slots.count == 2);
    const CandidateSet set = world.runtime().enumerateCandidates(request);
    WF_CHECK_EQ(set.status.code, StatusCode::Ok);
    WF_CHECK(set.eligibleCount >= 1);
  }
  WF_CHECK(describeRuntime(world.runtime().stats()) == before);
  WF_CHECK_EQ(world.runtime().reservations().size(), count);
  WF_CHECK_EQ(world.runtime().stats().allocationsCommitted, std::uint64_t(1));
}

WF_TEST_MAIN()
