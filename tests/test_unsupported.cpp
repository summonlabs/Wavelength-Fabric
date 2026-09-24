#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// UNSUPPORTED and UNKNOWN are first-class published values.
//
// A domain that is published UNSUPPORTED, published UNKNOWN, or that has no
// published capability at all never yields a candidate and never yields a
// reservation, and nothing is ever approximated with synthetic channels. The
// three refusals stay distinguishable, the accounting is untouched by them, and
// republishing at a newer generation changes the answer without revoking
// spectrum that is already owned.

using namespace wavelength_fabric;
using namespace wf_test;

namespace {

template <class Enum>
[[nodiscard]] std::string token(Enum value) {
  return std::string(toToken(value));
}

const Instant kRequestedAt = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::hours(1);

struct Rig {
  SpectrumRuntime runtime;
  ControllerFence fence;

  Rig() {
    (void)runtime.registerGrid(fixedGrid());
    fence = runtime.fence();
  }

  void addDomain(SpectrumDomainId id, SpectrumDomainGeneration generation = SpectrumDomainGeneration(1)) {
    (void)runtime.registerDomain(makeDomain(id, ChannelGridId(1), generation));
  }

  Status publish(SpectrumDomainId id, SpectrumSupport support, CapabilityGeneration generation,
                 std::uint32_t slots, SpectrumDomainGeneration domainGeneration = SpectrumDomainGeneration(1)) {
    return runtime.publishCapability(makeCapability(id, ChannelGridId(1), domainGeneration,
                                                    GridGeneration(1), support, 0, slots, fence,
                                                    ControllerId(1), generation));
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

  [[nodiscard]] SpectrumRequest request(AllocationRequestId requestId,
                                        const std::vector<SpectrumDomainId>& domains,
                                        std::uint32_t slots = 1,
                                        SpectrumDomainGeneration generation = SpectrumDomainGeneration(1)) const {
    const std::vector<SpectrumDomainGeneration> generations(domains.size(), generation);
    return makeRequest(requestId, OwnerId(1), domains, generations, ChannelGridId(1),
                       GridGeneration(1), slots, kRequestedAt, kLease, eligibility(), reservation(),
                       slots > 1 ? ContiguityRequirement::Required
                                 : ContiguityRequirement::Unspecified,
                       domains.size() > 1 ? ContinuityRequirement::Required
                                          : ContinuityRequirement::Unspecified);
  }
};

}  // namespace

WF_TEST(unsupported_domain_yields_no_candidate) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());

  const std::optional<SpectrumCapability> stored = rig.runtime.capability(SpectrumDomainId(1));
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->support == SpectrumSupport::Unsupported);
  WF_CHECK(allocatableWindow(*stored).empty());

  const SpectrumRequest request = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)});
  const CandidateSet candidates = rig.runtime.enumerateCandidates(request);
  WF_CHECK(candidates.status.code == StatusCode::Unsupported);
  WF_CHECK(!candidates.status.message.empty());
  WF_CHECK(candidates.candidates.empty());
  WF_CHECK_EQ(candidates.eligibleCount, std::size_t{0});
  WF_CHECK_EQ(candidates.omitted, std::size_t{0});
  WF_CHECK(!candidates.complete);
  WF_CHECK(!candidates.summary.empty());

  // Enumerating is read-only: no ownership, no accounting.
  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{0});
}

WF_TEST(unsupported_domain_yields_no_reservation) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());

  const SpectrumRequest request = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)});
  const AllocationDecision decision = rig.runtime.allocate(request);
  WF_CHECK(!decision.allocated());
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(decision.status.code == StatusCode::Unsupported);
  WF_CHECK(!decision.reservation.valid());
  WF_CHECK(!decision.generation.valid());
  WF_CHECK(decision.explanation.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(!decision.explanation.reasons.empty());
  WF_CHECK(decision.explanation.selected.slots.empty());
  WF_CHECK_EQ(decision.explanation.selected.perDomainSlots.size(), std::size_t{0});

  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK(rig.runtime.reservationsForDomain(SpectrumDomainId(1)).empty());
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{1});
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{0});

  const std::optional<SpectrumUsage> usage = rig.runtime.usage(SpectrumDomainId(1), kRequestedAt);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{0});

  const std::vector<AuditRecord> records = rig.runtime.audit(AuditSequence(0), 0);
  WF_REQUIRE(!records.empty());
  const AuditRecord& last = records.back();
  WF_CHECK(last.kind == AuditKind::AllocationRefused);
  WF_CHECK(last.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK_EQ(last.domain.raw(), std::uint64_t{1});
  WF_CHECK(last.fence == rig.fence);
}

WF_TEST(unknown_capability_is_not_unsupported) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  rig.addDomain(SpectrumDomainId(2));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());
  WF_REQUIRE(rig.publish(SpectrumDomainId(2), SpectrumSupport::Unknown, CapabilityGeneration(1), 0).ok());

  const std::optional<SpectrumCapability> unknown = rig.runtime.capability(SpectrumDomainId(2));
  WF_REQUIRE(unknown.has_value());
  WF_CHECK(unknown->support == SpectrumSupport::Unknown);
  WF_CHECK(allocatableWindow(*unknown).empty());

  const SpectrumRequest unsupportedRequest = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)});
  const SpectrumRequest unknownRequest = rig.request(AllocationRequestId(2), {SpectrumDomainId(2)});

  const CandidateSet unsupportedCandidates = rig.runtime.enumerateCandidates(unsupportedRequest);
  const CandidateSet unknownCandidates = rig.runtime.enumerateCandidates(unknownRequest);
  WF_CHECK(unsupportedCandidates.status.code == StatusCode::Unsupported);
  WF_CHECK(unknownCandidates.status.code == StatusCode::Unknown);
  WF_CHECK(unknownCandidates.candidates.empty());
  WF_CHECK(!unknownCandidates.complete);

  const AllocationDecision unsupported = rig.runtime.allocate(unsupportedRequest);
  const AllocationDecision unknownDecision = rig.runtime.allocate(unknownRequest);
  WF_CHECK(unsupported.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(unknownDecision.outcome == AllocationOutcome::RefusedUnknownCapability);
  WF_CHECK(unknownDecision.status.code == StatusCode::Unknown);
  WF_CHECK(!unknownDecision.reservation.valid());
  WF_CHECK(!unknownDecision.explanation.reasons.empty());
  WF_CHECK_EQ(unknownDecision.explanation.selected.perDomainSlots.size(), std::size_t{0});

  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{2});
}

WF_TEST(domain_without_published_capability_is_unknown_capability) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  WF_CHECK(!rig.runtime.capability(SpectrumDomainId(1)).has_value());

  const SpectrumRequest request = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)});
  const CandidateSet candidates = rig.runtime.enumerateCandidates(request);
  WF_CHECK(candidates.status.code == StatusCode::NotFound);
  WF_CHECK(candidates.candidates.empty());
  WF_CHECK_EQ(candidates.eligibleCount, std::size_t{0});
  WF_CHECK(!candidates.complete);

  const AllocationDecision decision = rig.runtime.allocate(request);
  WF_CHECK(!decision.allocated());
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedUnknownCapability);
  // The allocation status mirrors the typed outcome; the enumeration status is
  // where the absence of a capability is reported as NotFound.
  WF_CHECK(decision.status.code == StatusCode::Unknown);
  WF_CHECK_EQ(decision.explanation.selected.perDomainSlots.size(), std::size_t{0});
  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{1});
}

WF_TEST(one_unsupported_domain_refuses_the_whole_request) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  rig.addDomain(SpectrumDomainId(2));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Supported, CapabilityGeneration(1), 96).ok());
  WF_REQUIRE(rig.publish(SpectrumDomainId(2), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());

  const SpectrumRequest request =
      rig.request(AllocationRequestId(1), {SpectrumDomainId(1), SpectrumDomainId(2)});
  const CandidateSet candidates = rig.runtime.enumerateCandidates(request);
  WF_CHECK(candidates.status.code == StatusCode::Unsupported);
  WF_CHECK(candidates.candidates.empty());
  WF_CHECK_EQ(candidates.eligibleCount, std::size_t{0});
  WF_CHECK(!candidates.complete);

  const AllocationDecision decision = rig.runtime.allocate(request);
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(decision.status.code == StatusCode::Unsupported);
  WF_CHECK(!decision.reservation.valid());
  // The supported domain is not partially allocated.
  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK(rig.runtime.reservationsForDomain(SpectrumDomainId(1)).empty());
  const std::optional<SpectrumUsage> first = rig.runtime.usage(SpectrumDomainId(1), kRequestedAt);
  WF_REQUIRE(first.has_value());
  WF_CHECK_EQ(first->liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(first->freeSlots, std::uint32_t{96});

  // The same request succeeds once the second domain is supported: nothing but
  // the published support value changes the outcome.
  Rig twin;
  twin.addDomain(SpectrumDomainId(1));
  twin.addDomain(SpectrumDomainId(2));
  WF_REQUIRE(twin.publish(SpectrumDomainId(1), SpectrumSupport::Supported, CapabilityGeneration(1), 96).ok());
  WF_REQUIRE(twin.publish(SpectrumDomainId(2), SpectrumSupport::Supported, CapabilityGeneration(1), 96).ok());
  const AllocationDecision allocated =
      twin.runtime.allocate(twin.request(AllocationRequestId(1), {SpectrumDomainId(1), SpectrumDomainId(2)}));
  WF_CHECK(allocated.allocated());
  WF_CHECK_EQ(allocated.explanation.selected.perDomainSlots.size(), std::size_t{2});
}

WF_TEST(stale_capability_is_its_own_refusal) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Supported, CapabilityGeneration(1), 96).ok());

  // The domain moves to a newer generation while the capability still describes
  // the older one: a stale capability is neither UNSUPPORTED nor UNKNOWN.
  rig.addDomain(SpectrumDomainId(1), SpectrumDomainGeneration(2));
  const std::optional<SpectrumDomain> domain = rig.runtime.domain(SpectrumDomainId(1));
  WF_REQUIRE(domain.has_value());
  WF_CHECK_EQ(domain->generation.raw(), std::uint64_t{2});

  const SpectrumRequest request = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)}, 1,
                                              SpectrumDomainGeneration(2));
  const CandidateSet candidates = rig.runtime.enumerateCandidates(request);
  WF_CHECK(candidates.status.code == StatusCode::StaleGeneration);
  WF_CHECK(candidates.candidates.empty());
  WF_CHECK(!candidates.complete);

  const AllocationDecision decision = rig.runtime.allocate(request);
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedStaleCapability);
  WF_CHECK(decision.status.code == StatusCode::StaleGeneration);
  WF_CHECK(!decision.reservation.valid());
  WF_CHECK(rig.runtime.reservations().empty());
}

WF_TEST(republishing_changes_future_allocations_only) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());

  const SpectrumRequest first = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)});
  WF_CHECK(rig.runtime.allocate(first).outcome == AllocationOutcome::RefusedUnsupported);

  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Supported, CapabilityGeneration(2), 96).ok());
  const AllocationDecision allocated = rig.runtime.allocate(first);
  WF_CHECK(allocated.allocated());
  WF_REQUIRE(allocated.reservation.valid());
  WF_CHECK_EQ(allocated.explanation.selected.slots.first, std::uint32_t{0});
  WF_CHECK_EQ(allocated.explanation.selected.slots.count, std::uint32_t{1});

  const std::optional<SpectrumReservation> reservation = rig.runtime.reservation(allocated.reservation);
  WF_REQUIRE(reservation.has_value());
  WF_CHECK(reservation->state == ReservationState::Reserved);
  WF_CHECK_EQ(reservation->capabilityGeneration.raw(), std::uint64_t{2});

  // Publishing UNSUPPORTED again refuses new requests without revoking the
  // spectrum that is already owned.
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Unsupported, CapabilityGeneration(3), 0).ok());
  const SpectrumRequest second = rig.request(AllocationRequestId(2), {SpectrumDomainId(1)});
  WF_CHECK(rig.runtime.enumerateCandidates(second).status.code == StatusCode::Unsupported);
  WF_CHECK(rig.runtime.allocate(second).outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{1});
  const std::optional<SpectrumReservation> stillOwned = rig.runtime.reservation(allocated.reservation);
  WF_REQUIRE(stillOwned.has_value());
  WF_CHECK(stillOwned->state == ReservationState::Reserved);
  const std::optional<SpectrumUsage> usage = rig.runtime.usage(SpectrumDomainId(1), kRequestedAt);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{1});

  // Supporting the domain again places the next request beside the owned slot.
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Supported, CapabilityGeneration(4), 96).ok());
  const AllocationDecision next = rig.runtime.allocate(rig.request(AllocationRequestId(3), {SpectrumDomainId(1)}));
  WF_CHECK(next.allocated());
  WF_CHECK_EQ(next.explanation.selected.slots.first, std::uint32_t{1});
}

WF_TEST(unsupported_refusals_leave_accounting_isolated) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));  // published UNSUPPORTED
  rig.addDomain(SpectrumDomainId(2));  // published UNKNOWN
  rig.addDomain(SpectrumDomainId(3));  // no capability at all
  rig.addDomain(SpectrumDomainId(4));  // published SUPPORTED
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());
  WF_REQUIRE(rig.publish(SpectrumDomainId(2), SpectrumSupport::Unknown, CapabilityGeneration(1), 0).ok());
  WF_REQUIRE(rig.publish(SpectrumDomainId(4), SpectrumSupport::Supported, CapabilityGeneration(1), 96).ok());

  const AllocationDecision unsupported =
      rig.runtime.allocate(rig.request(AllocationRequestId(1), {SpectrumDomainId(1)}));
  const AllocationDecision unknown =
      rig.runtime.allocate(rig.request(AllocationRequestId(2), {SpectrumDomainId(2)}));
  const AllocationDecision missing =
      rig.runtime.allocate(rig.request(AllocationRequestId(3), {SpectrumDomainId(3)}));
  WF_CHECK(unsupported.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(unknown.outcome == AllocationOutcome::RefusedUnknownCapability);
  WF_CHECK(missing.outcome == AllocationOutcome::RefusedUnknownCapability);
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{3});
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{0});

  const AllocationDecision allocated =
      rig.runtime.allocate(rig.request(AllocationRequestId(4), {SpectrumDomainId(4)}));
  WF_CHECK(allocated.allocated());
  WF_CHECK_EQ(rig.runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{3});
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{96});
  for (const SpectrumDomainId id : {SpectrumDomainId(1), SpectrumDomainId(2), SpectrumDomainId(3)}) {
    WF_CHECK(rig.runtime.reservationsForDomain(id).empty());
    const std::optional<SpectrumUsage> usage = rig.runtime.usage(id, kRequestedAt);
    WF_REQUIRE(usage.has_value());
    WF_CHECK_EQ(usage->liveReservations, std::uint32_t{0});
    WF_CHECK_EQ(usage->liveSlots, std::uint32_t{0});
  }
  const std::optional<SpectrumUsage> fourth = rig.runtime.usage(SpectrumDomainId(4), kRequestedAt);
  WF_REQUIRE(fourth.has_value());
  WF_CHECK_EQ(fourth->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(fourth->freeSlots, std::uint32_t{95});
}

WF_TEST(channel_width_refusal_is_not_an_unsupported_refusal) {
  Rig rig;
  rig.addDomain(SpectrumDomainId(1));
  rig.addDomain(SpectrumDomainId(2));
  WF_REQUIRE(rig.publish(SpectrumDomainId(1), SpectrumSupport::Supported, CapabilityGeneration(1), 96).ok());
  WF_REQUIRE(rig.publish(SpectrumDomainId(2), SpectrumSupport::Unsupported, CapabilityGeneration(1), 0).ok());

  // Two slots on a one-slot grid: the grid bound refuses the request and no
  // single-slot channel is substituted for it.
  const SpectrumRequest wideSupported = rig.request(AllocationRequestId(1), {SpectrumDomainId(1)}, 2);
  const CandidateSet wideCandidates = rig.runtime.enumerateCandidates(wideSupported);
  WF_CHECK(wideCandidates.status.code == StatusCode::InvalidArgument);
  WF_CHECK(wideCandidates.candidates.empty());
  WF_CHECK_EQ(wideCandidates.eligibleCount, std::size_t{0});
  WF_CHECK(!wideCandidates.complete);
  const AllocationDecision wideDecision = rig.runtime.allocate(wideSupported);
  WF_CHECK(wideDecision.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(wideDecision.status.code == StatusCode::InvalidArgument);
  WF_CHECK(!wideDecision.reservation.valid());

  // The width gate is a grid-level check, so it is reported for a spanned
  // UNSUPPORTED domain too, before that domain's capability is consulted.
  const AllocationDecision wideUnsupported =
      rig.runtime.allocate(rig.request(AllocationRequestId(2), {SpectrumDomainId(2)}, 2));
  WF_CHECK(wideUnsupported.outcome == AllocationOutcome::RefusedChannelWidth);
  WF_CHECK(wideUnsupported.status.code == StatusCode::InvalidArgument);
  WF_CHECK(!wideUnsupported.reservation.valid());

  // With a representable width the same domain is refused for its capability,
  // so the two refusals never collapse into one another.
  const SpectrumRequest narrowUnsupported =
      rig.request(AllocationRequestId(3), {SpectrumDomainId(2)}, 1);
  const CandidateSet narrowCandidates = rig.runtime.enumerateCandidates(narrowUnsupported);
  WF_CHECK(narrowCandidates.status.code == StatusCode::Unsupported);
  WF_CHECK(narrowCandidates.candidates.empty());
  const AllocationDecision narrowDecision = rig.runtime.allocate(narrowUnsupported);
  WF_CHECK(narrowDecision.outcome == AllocationOutcome::RefusedUnsupported);
  WF_CHECK(narrowDecision.status.code == StatusCode::Unsupported);
  WF_CHECK(!narrowDecision.reservation.valid());

  WF_CHECK(rig.runtime.reservations().empty());
  WF_CHECK_EQ(rig.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().allocationsRefused, std::uint64_t{3});
  WF_CHECK_EQ(rig.runtime.stats().candidatesEvaluated, std::uint64_t{0});
  WF_CHECK_EQ(rig.runtime.stats().enumerationTruncations, std::uint64_t{0});
}

WF_TEST_MAIN()
