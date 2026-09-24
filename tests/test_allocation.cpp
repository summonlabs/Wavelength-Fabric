// Allocation tests: deterministic first-fit selection, committed reservation and
// request identity generations, lease arithmetic, per-domain commit accounting,
// exact usage() accounting, saturation refusals and the fact that a refused
// request creates no ownership at all.

#include "test_common.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace wavelength_fabric;
using namespace wf_test;

// WF_CHECK compares a boolean. These cases assert exact values, so the macro
// below compares two values and streams both sides when they differ.
#define WF_SAME(actual, expected)                                                \
  do {                                                                           \
    ::wf_test::Harness::instance().noteCheck();                                  \
    const auto wf_actual_value = (actual);                                       \
    const auto wf_expected_value = (expected);                                   \
    if (!(wf_actual_value == wf_expected_value)) {                               \
      std::ostringstream wf_message;                                             \
      wf_message << #actual " != " #expected " (actual=" << wf_actual_value      \
                 << ", expected=" << wf_expected_value << ")";                   \
      ::wf_test::Harness::instance().fail(__FILE__, __LINE__, wf_message.str()); \
    }                                                                            \
  } while (false)

namespace {

// The fixtures default to a 96-slot 50 GHz fixed grid anchored at 191300000 MHz
// and a 384-slot 12.5 GHz flex grid anchored at the same frequency. Every
// expectation below is derived from those two arithmetic facts.
constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::int64_t kFixedSlotMhz = 50'000;
constexpr std::int64_t kFlexSlotMhz = 12'500;

const Instant t0 = Instant::fromSeconds(1'800'000'000);
const Duration oneHour = Duration::hours(1);
const Duration hundredSeconds = Duration::seconds(100);

const ChannelGridId fixedGridId{ChannelGridId(1)};
const GridGeneration fixedGridGen{GridGeneration(1)};
const ChannelGridId flexGridId{ChannelGridId(2)};
const GridGeneration flexGridGen{GridGeneration(1)};

[[nodiscard]] std::string codeOf(const Status& status) { return std::string(toToken(status.code)); }

[[nodiscard]] std::string outcomeOf(AllocationOutcome outcome) {
  return std::string(toToken(outcome));
}

[[nodiscard]] std::string stateOf(ReservationState state) { return std::string(toToken(state)); }

[[nodiscard]] std::string eligibilityOf(CandidateEligibility eligibility) {
  return std::string(toToken(eligibility));
}

[[nodiscard]] bool mentions(const std::vector<std::string>& reasons, std::string_view needle) {
  for (const std::string& reason : reasons) {
    if (reason.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Lower edge of the fixed grid slot the given index starts at.
[[nodiscard]] std::int64_t fixedLow(std::uint32_t slot) {
  return kAnchorMhz + kFixedSlotMhz * static_cast<std::int64_t>(slot);
}

[[nodiscard]] std::int64_t flexLow(std::uint32_t slot) {
  return kAnchorMhz + kFlexSlotMhz * static_cast<std::int64_t>(slot);
}

// The "fixed" identifier carries a flex grid with exactly the same 50 GHz
// arithmetic: a fixed grid expresses exactly one slot per channel, so every
// multi-slot channel in this file lives on the flex grid. The fixed-grid
// channel-width rule has its own case below.
void registerGrids(SpectrumRuntime& runtime) {
  (void)runtime.registerGrid(flexGrid(fixedGridId, fixedGridGen, kAnchorMhz, kFixedSlotMhz, 96, 1, 32));
  (void)runtime.registerGrid(flexGrid(flexGridId, flexGridGen));
}

SpectrumRequest singleDomainRequest(AllocationRequestId requestId, SpectrumDomainId domain,
                                    ChannelGridId grid, GridGeneration gridGeneration,
                                    std::uint32_t slots, Instant requestedAt, Duration lease,
                                    const Fixture& fixture) {
  return makeRequest(requestId, OwnerId(7), {domain}, {SpectrumDomainGeneration(1)}, grid,
                     gridGeneration, slots, requestedAt, lease, fixture.eligibility(),
                     fixture.reservation(), ContiguityRequirement::Required,
                     ContinuityRequirement::Unspecified);
}

// Restricts a single-slot request to one flex slot so that a case can place a
// reservation at a chosen index instead of relying on first fit.
void pinToFlexSlot(SpectrumRequest& request, std::uint32_t slot) {
  request.frequencyWindows.push_back(FrequencyRange{flexLow(slot), flexLow(slot + 1)});
}

// Every field a caller can observe about a committed reservation, asserted
// against the expected value in one place.
void expectCommitted(const SpectrumRuntime& runtime, const AllocationDecision& decision,
                     ReservationId id, std::uint32_t first, std::uint32_t count, Instant start,
                     Instant requestedAt, Duration lease, std::uint32_t domainCount) {
  const std::optional<SpectrumReservation> stored = runtime.reservation(id);
  WF_REQUIRE(stored.has_value());
  const SpectrumReservation& reservation = *stored;
  WF_SAME(reservation.id.raw(), id.raw());
  WF_SAME(reservation.generation.raw(), std::uint64_t{1});
  WF_SAME(stateOf(reservation.state), "reserved");
  WF_SAME(reservation.slots.first, first);
  WF_SAME(reservation.slots.count, count);
  WF_SAME(reservation.frequency.lowMhz, fixedLow(first));
  WF_SAME(reservation.frequency.highMhz, fixedLow(first + count));
  WF_SAME(reservation.domains.size(), static_cast<std::size_t>(domainCount));
  WF_SAME(reservation.perDomainSlots.size(), static_cast<std::size_t>(domainCount));
  for (std::size_t index = 0; index < reservation.perDomainSlots.size(); ++index) {
    WF_SAME(reservation.perDomainSlots[index].first, first);
    WF_SAME(reservation.perDomainSlots[index].count, count);
  }
  WF_SAME(reservation.anchorDomain.raw(), reservation.domains.front().raw());
  WF_SAME(reservation.grid.raw(), fixedGridId.raw());
  WF_SAME(reservation.gridGeneration.raw(), fixedGridGen.raw());
  WF_CHECK(!reservation.crossGrid);
  WF_CHECK(reservation.contiguityRequired);
  WF_CHECK(!reservation.continuityRequired);
  WF_SAME(reservation.guardBandMhz, std::int64_t{0});
  WF_CHECK(!reservation.exclusionDomain.valid());
  WF_SAME(reservation.lease.generation.raw(), std::uint64_t{1});
  WF_SAME(reservation.lease.grantedAt.nanos(), start.nanos());
  WF_SAME(reservation.lease.expiresAt.nanos(), (start + lease).nanos());
  WF_SAME(reservation.lease.renewalCount, 0u);
  WF_SAME(reservation.lease.maxRenewals, 0u);
  WF_SAME(reservation.createdAt.nanos(), start.nanos());
  WF_SAME(reservation.updatedAt.nanos(), requestedAt.nanos());
  WF_SAME(reservation.activatedAt.nanos(), std::int64_t{0});
  WF_SAME(reservation.deactivatedAt.nanos(), std::int64_t{0});
  WF_SAME(reservation.releasedAt.nanos(), std::int64_t{0});
  WF_SAME(reservation.reclaimedAt.nanos(), std::int64_t{0});
  WF_CHECK(!reservation.needsRevalidation);
  WF_SAME(reservation.commitFence.epoch.raw(), runtime.fence().epoch.raw());
  WF_SAME(reservation.commitFence.incarnation.raw(), runtime.fence().incarnation.raw());
  WF_SAME(reservation.lastOperation.raw(), std::uint64_t{1});
  WF_SAME(decision.reservation.raw(), id.raw());
  WF_SAME(decision.generation.raw(), std::uint64_t{1});
}

}  // namespace

// ---------------------------------------------------------------------------
// First fit, committed values and exact accounting
// ---------------------------------------------------------------------------

WF_TEST(allocation_commits_first_fit_with_exact_values_and_usage) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  const std::optional<SpectrumUsage> before = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(before.has_value());
  WF_SAME(before->domain.raw(), std::uint64_t{1});
  WF_SAME(before->domainGeneration.raw(), std::uint64_t{1});
  WF_SAME(before->grid.raw(), fixedGridId.raw());
  WF_SAME(before->gridGeneration.raw(), fixedGridGen.raw());
  WF_SAME(before->totalSlots, 96u);
  WF_SAME(before->allocatableSlots, 96u);
  WF_SAME(before->liveSlots, 0u);
  WF_SAME(before->activeSlots, 0u);
  WF_SAME(before->reservedSlots, 0u);
  WF_SAME(before->freeSlots, 96u);
  WF_SAME(before->lapsedSlots, 0u);
  WF_SAME(before->liveReservations, 0u);
  WF_SAME(before->activeReservations, 0u);
  WF_SAME(before->lapsedReservations, 0u);
  WF_SAME(before->reclaimedReservations, 0u);
  WF_SAME(before->releasedReservations, 0u);
  WF_REQUIRE(before->freeRuns.size() == 1);
  WF_SAME(before->freeRuns.front().first, 0u);
  WF_SAME(before->freeRuns.front().count, 96u);

  const AllocationDecision first =
      fx.runtime.allocate(singleDomainRequest(AllocationRequestId(1), SpectrumDomainId(1), fixedGridId,
                                              fixedGridGen, 3, t0, oneHour, fx));
  WF_SAME(codeOf(first.status), "ok");
  WF_SAME(outcomeOf(first.outcome), "allocated");
  WF_CHECK(first.allocated());
  WF_SAME(outcomeOf(first.explanation.outcome), "allocated");
  WF_SAME(first.explanation.reservation.raw(), std::uint64_t{1});
  WF_SAME(first.explanation.generation.raw(), std::uint64_t{1});
  WF_SAME(first.explanation.fence.epoch.raw(), fx.runtime.epoch().raw());
  WF_SAME(first.explanation.fence.incarnation.raw(), fx.runtime.incarnation().raw());
  WF_SAME(first.explanation.candidatesEnumerated, std::size_t{94});
  WF_SAME(first.explanation.candidatesRejected, std::size_t{0});
  WF_SAME(first.explanation.candidatesOmitted, std::size_t{0});
  WF_SAME(first.explanation.candidateOrdinal, std::size_t{0});
  WF_SAME(first.explanation.selected.ordinal, std::size_t{0});
  WF_SAME(first.explanation.selected.anchorDomain.raw(), std::uint64_t{1});
  WF_SAME(first.explanation.selected.slots.first, 0u);
  WF_SAME(first.explanation.selected.slots.count, 3u);
  WF_SAME(first.explanation.selected.frequency.lowMhz, fixedLow(0));
  WF_SAME(first.explanation.selected.frequency.highMhz, fixedLow(3));
  WF_SAME(eligibilityOf(first.explanation.selected.eligibility), "eligible");
  WF_REQUIRE(first.explanation.selected.perDomainSlots.size() == 1);
  WF_SAME(first.explanation.selected.perDomainSlots.front().first, 0u);
  WF_SAME(first.explanation.selected.perDomainSlots.front().count, 3u);
  WF_CHECK(first.explanation.conflicts.empty());
  WF_CHECK(first.explanation.rejected.empty());
  WF_REQUIRE(first.explanation.reasons.size() == 1);
  WF_CHECK(first.explanation.reasons.front().find("3 slot(s) starting at 0") != std::string::npos);
  expectCommitted(fx.runtime, first, ReservationId(1), 0, 3, t0, t0, oneHour, 1);

  const std::optional<SpectrumReservation> stored = fx.runtime.reservation(ReservationId(1));
  WF_REQUIRE(stored.has_value());
  WF_SAME(stored->requestId.raw(), std::uint64_t{1});
  WF_SAME(stored->requestGeneration.raw(), std::uint64_t{1});
  WF_SAME(stored->owner.raw(), std::uint64_t{7});
  WF_SAME(stored->ownerGeneration.raw(), std::uint64_t{1});
  WF_SAME(stored->domainGenerations.size(), std::size_t{1});
  WF_SAME(stored->domainGenerations.front().raw(), std::uint64_t{1});
  WF_SAME(stored->capabilityGeneration.raw(), std::uint64_t{1});
  WF_SAME(stored->lease.maxRenewals, 0u);

  const std::optional<SpectrumUsage> after = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(after.has_value());
  WF_SAME(after->liveSlots, 3u);
  WF_SAME(after->activeSlots, 0u);
  WF_SAME(after->reservedSlots, 3u);
  WF_SAME(after->freeSlots, 93u);
  WF_SAME(after->liveReservations, 1u);
  WF_REQUIRE(after->freeRuns.size() == 1);
  WF_SAME(after->freeRuns.front().first, 3u);
  WF_SAME(after->freeRuns.front().count, 93u);

  // The next request starts immediately after the committed range: first fit.
  const AllocationDecision second =
      fx.runtime.allocate(singleDomainRequest(AllocationRequestId(2), SpectrumDomainId(1), fixedGridId,
                                              fixedGridGen, 1, t0, oneHour, fx));
  WF_CHECK(second.allocated());
  WF_SAME(second.reservation.raw(), std::uint64_t{2});
  WF_SAME(second.explanation.candidatesEnumerated, std::size_t{93});
  WF_SAME(second.explanation.selected.slots.first, 3u);
  WF_SAME(second.explanation.selected.slots.count, 1u);
  expectCommitted(fx.runtime, second, ReservationId(2), 3, 1, t0, t0, oneHour, 1);

  const RuntimeStats stats = fx.runtime.stats();
  WF_SAME(stats.allocationsCommitted, std::uint64_t{2});
  WF_SAME(stats.allocationsRefused, std::uint64_t{0});
  WF_SAME(stats.candidatesEvaluated, std::uint64_t{187});
  WF_SAME(stats.enumerationTruncations, std::uint64_t{0});
  WF_SAME(fx.runtime.reservations().size(), std::size_t{2});
  WF_SAME(fx.runtime.reservationsForDomain(SpectrumDomainId(1)).size(), std::size_t{2});
  WF_SAME(fx.runtime.reservationsForDomain(SpectrumDomainId(2)).size(), std::size_t{0});

  const std::optional<SpectrumUsage> finalUsage = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(finalUsage.has_value());
  WF_SAME(finalUsage->liveSlots, 4u);
  WF_SAME(finalUsage->reservedSlots, 4u);
  WF_SAME(finalUsage->freeSlots, 92u);
  WF_REQUIRE(finalUsage->freeRuns.size() == 1);
  WF_SAME(finalUsage->freeRuns.front().first, 4u);
  WF_SAME(finalUsage->freeRuns.front().count, 92u);
}

WF_TEST(allocation_is_deterministic_for_identical_inputs) {
  Fixture left;
  Fixture right;
  registerGrids(left.runtime);
  registerGrids(right.runtime);
  (void)left.addDomain(SpectrumDomainId(1), fixedGridId, 96);
  (void)right.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  const std::uint32_t widths[3] = {3, 1, 2};
  for (std::size_t index = 0; index < 3; ++index) {
    const AllocationDecision a = left.runtime.allocate(singleDomainRequest(
        AllocationRequestId(40 + index), SpectrumDomainId(1), fixedGridId, fixedGridGen, widths[index],
        t0 + Duration::seconds(static_cast<std::int64_t>(index)), oneHour, left));
    const AllocationDecision b = right.runtime.allocate(singleDomainRequest(
        AllocationRequestId(40 + index), SpectrumDomainId(1), fixedGridId, fixedGridGen, widths[index],
        t0 + Duration::seconds(static_cast<std::int64_t>(index)), oneHour, right));
    WF_CHECK(a.allocated());
    WF_CHECK(b.allocated());
    WF_SAME(a.reservation.raw(), b.reservation.raw());
    WF_SAME(a.generation.raw(), b.generation.raw());
    WF_SAME(a.explanation.selected.slots.first, b.explanation.selected.slots.first);
    WF_SAME(a.explanation.selected.slots.count, b.explanation.selected.slots.count);
    WF_SAME(a.explanation.candidatesEnumerated, b.explanation.candidatesEnumerated);
    WF_SAME(a.explanation.candidateOrdinal, b.explanation.candidateOrdinal);
  }

  const std::vector<SpectrumReservation> leftReservations = left.runtime.reservations();
  const std::vector<SpectrumReservation> rightReservations = right.runtime.reservations();
  WF_REQUIRE(leftReservations.size() == rightReservations.size());
  for (std::size_t index = 0; index < leftReservations.size(); ++index) {
    WF_SAME(leftReservations[index].id.raw(), rightReservations[index].id.raw());
    WF_SAME(stateOf(leftReservations[index].state), stateOf(rightReservations[index].state));
    WF_SAME(leftReservations[index].slots.first, rightReservations[index].slots.first);
    WF_SAME(leftReservations[index].slots.count, rightReservations[index].slots.count);
    WF_SAME(leftReservations[index].frequency.lowMhz, rightReservations[index].frequency.lowMhz);
    WF_SAME(leftReservations[index].frequency.highMhz, rightReservations[index].frequency.highMhz);
    WF_SAME(leftReservations[index].lease.expiresAt.nanos(),
            rightReservations[index].lease.expiresAt.nanos());
  }
  WF_SAME(left.runtime.stats().allocationsCommitted, right.runtime.stats().allocationsCommitted);
  WF_SAME(left.runtime.stats().candidatesEvaluated, right.runtime.stats().candidatesEvaluated);
}

// ---------------------------------------------------------------------------
// Lease arithmetic
// ---------------------------------------------------------------------------

WF_TEST(lease_starts_at_the_later_of_requested_at_and_not_before) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  SpectrumRequest later =
      singleDomainRequest(AllocationRequestId(10), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0,
                          hundredSeconds, fx);
  later.notBefore = t0 + Duration::seconds(10);
  const AllocationDecision deferred = fx.runtime.allocate(later);
  WF_REQUIRE(deferred.allocated());
  expectCommitted(fx.runtime, deferred, ReservationId(1), 0, 1, t0 + Duration::seconds(10), t0,
                  hundredSeconds, 1);
  const std::optional<SpectrumReservation> deferredStored = fx.runtime.reservation(ReservationId(1));
  WF_REQUIRE(deferredStored.has_value());
  WF_SAME(deferredStored->lease.grantedAt.nanos(), (t0 + Duration::seconds(10)).nanos());
  WF_SAME(deferredStored->lease.expiresAt.nanos(), (t0 + Duration::seconds(110)).nanos());
  WF_SAME(deferredStored->createdAt.nanos(), (t0 + Duration::seconds(10)).nanos());
  WF_SAME(deferredStored->updatedAt.nanos(), t0.nanos());

  SpectrumRequest earlier =
      singleDomainRequest(AllocationRequestId(11), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0,
                          hundredSeconds, fx);
  earlier.notBefore = t0 - Duration::seconds(50);
  const AllocationDecision immediate = fx.runtime.allocate(earlier);
  WF_REQUIRE(immediate.allocated());
  expectCommitted(fx.runtime, immediate, ReservationId(2), 1, 1, t0, t0, hundredSeconds, 1);

  // Exactly 3650 days is accepted; one nanosecond more is refused structurally.
  const AllocationDecision ceiling = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(12), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, Duration::days(3650), fx));
  WF_REQUIRE(ceiling.allocated());
  const std::optional<SpectrumReservation> ceilingStored = fx.runtime.reservation(ceiling.reservation);
  WF_REQUIRE(ceilingStored.has_value());
  WF_SAME(ceilingStored->lease.expiresAt.nanos(), (t0 + Duration::days(3650)).nanos());

  const AllocationDecision overCeiling = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(13), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0,
      Duration::nanos(Duration::days(3650).nanos() + 1), fx));
  WF_SAME(codeOf(overCeiling.status), "invalid-argument");
  WF_SAME(outcomeOf(overCeiling.outcome), "refused-invalid-request");
  WF_CHECK(!overCeiling.allocated());
  WF_CHECK(mentions(overCeiling.explanation.reasons, "exceeds the maximum"));
  WF_SAME(overCeiling.reservation.raw(), std::uint64_t{0});
  WF_SAME(overCeiling.explanation.candidatesEnumerated, std::size_t{0});
  WF_SAME(fx.runtime.reservations().size(), std::size_t{3});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});

  for (const Duration& rejected : {Duration::zero(), Duration::nanos(-1)}) {
    const AllocationDecision decision = fx.runtime.allocate(singleDomainRequest(
        AllocationRequestId(14), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, rejected, fx));
    WF_SAME(codeOf(decision.status), "invalid-argument");
    WF_SAME(outcomeOf(decision.outcome), "refused-invalid-request");
  }
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{3});
  WF_SAME(fx.runtime.reservations().size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// Request identity, replay and supersede
// ---------------------------------------------------------------------------

WF_TEST(replay_is_idempotent_and_newer_generations_supersede) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  const SpectrumRequest first =
      singleDomainRequest(AllocationRequestId(20), SpectrumDomainId(1), fixedGridId, fixedGridGen, 3, t0,
                          oneHour, fx);
  const AllocationDecision committed = fx.runtime.allocate(first);
  WF_REQUIRE(committed.allocated());
  WF_SAME(committed.reservation.raw(), std::uint64_t{1});
  WF_SAME(committed.explanation.candidatesEnumerated, std::size_t{94});

  // Identical identity and identical shape: an idempotent success.
  const AllocationDecision replay = fx.runtime.allocate(first);
  WF_CHECK(replay.allocated());
  WF_SAME(replay.reservation.raw(), std::uint64_t{1});
  WF_SAME(replay.generation.raw(), std::uint64_t{1});
  WF_SAME(replay.explanation.reservation.raw(), std::uint64_t{1});
  WF_SAME(replay.explanation.selected.slots.first, 0u);
  WF_SAME(replay.explanation.selected.slots.count, 3u);
  WF_SAME(replay.explanation.candidatesEnumerated, std::size_t{1});
  WF_CHECK(mentions(replay.explanation.reasons, "idempotent replay"));
  WF_SAME(fx.runtime.reservations().size(), std::size_t{1});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{0});
  const std::optional<SpectrumUsage> afterReplay = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(afterReplay.has_value());
  WF_SAME(afterReplay->liveSlots, 3u);

  // Same identity, different structural shape: refused as a duplicate.
  SpectrumRequest wider = first;
  wider.slots = 2;
  const AllocationDecision duplicate = fx.runtime.allocate(wider);
  WF_SAME(codeOf(duplicate.status), "duplicate");
  WF_SAME(outcomeOf(duplicate.outcome), "refused-duplicate");
  WF_CHECK(mentions(duplicate.explanation.reasons, "different shape"));
  WF_SAME(duplicate.reservation.raw(), std::uint64_t{0});

  SpectrumRequest fragmented = first;
  fragmented.contiguity = ContiguityRequirement::NotRequired;
  const AllocationDecision duplicateContiguity = fx.runtime.allocate(fragmented);
  WF_SAME(codeOf(duplicateContiguity.status), "duplicate");
  WF_SAME(outcomeOf(duplicateContiguity.outcome), "refused-duplicate");

  // A stale request generation is refused.
  SpectrumRequest stale = first;
  stale.requestGeneration = AllocationRequestGeneration(0);
  const AllocationDecision zeroGeneration = fx.runtime.allocate(stale);
  WF_SAME(codeOf(zeroGeneration.status), "invalid-argument");
  WF_SAME(outcomeOf(zeroGeneration.outcome), "refused-invalid-request");

  // A newer generation supersedes the live reservation. The candidate is chosen
  // before the supersede takes effect, so the replacement lands after the
  // spectrum the earlier generation still owned at evaluation time.
  SpectrumRequest second = first;
  second.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision replacement = fx.runtime.allocate(second);
  WF_REQUIRE(replacement.allocated());
  WF_SAME(replacement.reservation.raw(), std::uint64_t{2});
  WF_SAME(replacement.explanation.candidatesEnumerated, std::size_t{91});
  WF_SAME(replacement.explanation.selected.slots.first, 3u);
  WF_SAME(replacement.explanation.selected.slots.count, 3u);
  const std::optional<SpectrumReservation> superseded = fx.runtime.reservation(ReservationId(1));
  WF_REQUIRE(superseded.has_value());
  WF_SAME(stateOf(superseded->state), "superseded");
  WF_SAME(superseded->lastOperation.raw(), std::uint64_t{2});
  WF_SAME(superseded->updatedAt.nanos(), t0.nanos());
  WF_SAME(superseded->slots.first, 0u);
  WF_SAME(superseded->slots.count, 3u);
  const std::optional<SpectrumReservation> live = fx.runtime.reservation(ReservationId(2));
  WF_REQUIRE(live.has_value());
  WF_SAME(live->requestGeneration.raw(), std::uint64_t{2});
  WF_SAME(stateOf(live->state), "reserved");
  WF_SAME(live->frequency.lowMhz, fixedLow(3));
  WF_SAME(live->frequency.highMhz, fixedLow(6));

  // Only the newest generation owns spectrum, and the superseded range is free.
  const std::optional<SpectrumUsage> afterSupersede = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(afterSupersede.has_value());
  WF_SAME(afterSupersede->liveSlots, 3u);
  WF_SAME(afterSupersede->liveReservations, 1u);
  WF_SAME(afterSupersede->freeSlots, 93u);
  WF_REQUIRE(afterSupersede->freeRuns.size() == 2);
  WF_SAME(afterSupersede->freeRuns[0].first, 0u);
  WF_SAME(afterSupersede->freeRuns[0].count, 3u);
  WF_SAME(afterSupersede->freeRuns[1].first, 6u);
  WF_SAME(afterSupersede->freeRuns[1].count, 90u);

  // A third generation now reuses the range the superseded reservation released.
  SpectrumRequest third = first;
  third.requestGeneration = AllocationRequestGeneration(3);
  const AllocationDecision newest = fx.runtime.allocate(third);
  WF_REQUIRE(newest.allocated());
  WF_SAME(newest.reservation.raw(), std::uint64_t{3});
  WF_SAME(newest.explanation.selected.slots.first, 0u);
  WF_SAME(newest.explanation.selected.slots.count, 3u);
  WF_SAME(newest.explanation.candidatesEnumerated, std::size_t{89});
  const std::optional<SpectrumReservation> secondSuperseded = fx.runtime.reservation(ReservationId(2));
  WF_REQUIRE(secondSuperseded.has_value());
  WF_SAME(stateOf(secondSuperseded->state), "superseded");
  const std::optional<SpectrumUsage> afterThird = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(afterThird.has_value());
  WF_SAME(afterThird->liveSlots, 3u);
  WF_SAME(afterThird->liveReservations, 1u);
  WF_REQUIRE(afterThird->freeRuns.size() == 1);
  WF_SAME(afterThird->freeRuns.front().first, 3u);
  WF_SAME(afterThird->freeRuns.front().count, 93u);

  // A generation below the committed one is refused as stale and changes nothing.
  SpectrumRequest backdated = first;
  backdated.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision backwards = fx.runtime.allocate(backdated);
  WF_SAME(codeOf(backwards.status), "stale-generation");
  WF_SAME(outcomeOf(backwards.outcome), "refused-stale-generation");
  WF_CHECK(mentions(backwards.explanation.reasons, "is stale"));
  WF_SAME(backwards.reservation.raw(), std::uint64_t{0});
  WF_SAME(fx.runtime.reservations().size(), std::size_t{3});
  const std::optional<SpectrumReservation> thirdStored = fx.runtime.reservation(ReservationId(3));
  WF_REQUIRE(thirdStored.has_value());
  WF_SAME(stateOf(thirdStored->state), "reserved");
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{3});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{4});
}

WF_TEST(a_refused_supersede_leaves_the_previous_reservation_live) {
  Fixture fx;
  registerGrids(fx.runtime);
  // A three-slot allocatable window is exactly filled by the first generation.
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, 96, SpectrumDomainGeneration(1), flexGridGen, 3);

  SpectrumRequest request = makeRequest(AllocationRequestId(30), OwnerId(7), {SpectrumDomainId(1)},
                                        {SpectrumDomainGeneration(1)}, flexGridId, flexGridGen, 3, t0,
                                        oneHour, fx.eligibility(), fx.reservation(),
                                        ContiguityRequirement::Required,
                                        ContinuityRequirement::Unspecified);
  const AllocationDecision committed = fx.runtime.allocate(request);
  WF_REQUIRE(committed.allocated());
  WF_SAME(committed.explanation.candidatesEnumerated, std::size_t{1});

  SpectrumRequest newer = request;
  newer.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision refused = fx.runtime.allocate(newer);
  WF_SAME(codeOf(refused.status), "conflict");
  WF_SAME(outcomeOf(refused.outcome), "refused-conflict");
  WF_SAME(refused.explanation.conflicts.size(), std::size_t{1});
  WF_SAME(refused.explanation.conflicts.front().raw(), std::uint64_t{1});
  WF_REQUIRE(refused.explanation.rejected.size() == 1);
  WF_SAME(eligibilityOf(refused.explanation.rejected.front().eligibility), "ineligible-conflict");
  WF_SAME(refused.explanation.rejected.front().conflicts.size(), std::size_t{1});
  WF_SAME(fx.runtime.reservations().size(), std::size_t{1});
  const std::optional<SpectrumReservation> survivor = fx.runtime.reservation(ReservationId(1));
  WF_REQUIRE(survivor.has_value());
  WF_SAME(stateOf(survivor->state), "reserved");
  WF_SAME(survivor->requestGeneration.raw(), std::uint64_t{1});
  WF_SAME(survivor->lastOperation.raw(), std::uint64_t{1});
  const std::optional<SpectrumUsage> usage = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(usage.has_value());
  WF_SAME(usage->liveSlots, 3u);
  WF_SAME(usage->allocatableSlots, 3u);
  WF_SAME(usage->freeSlots, 0u);
  WF_CHECK(usage->freeRuns.empty());
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});
}

// ---------------------------------------------------------------------------
// Multi-domain requests
// ---------------------------------------------------------------------------

WF_TEST(a_multi_domain_request_values_every_domain) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);
  (void)fx.addDomain(SpectrumDomainId(2), fixedGridId, 96);
  (void)fx.addDomain(SpectrumDomainId(3), fixedGridId, 96);

  const AllocationDecision seed =
      fx.runtime.allocate(singleDomainRequest(AllocationRequestId(50), SpectrumDomainId(2), fixedGridId,
                                              fixedGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(seed.allocated());
  WF_SAME(seed.reservation.raw(), std::uint64_t{1});

  const SpectrumRequest request = makeRequest(
      AllocationRequestId(51), OwnerId(7),
      {SpectrumDomainId(1), SpectrumDomainId(2), SpectrumDomainId(3)},
      {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)}, fixedGridId,
      fixedGridGen, 2, t0, oneHour, fx.eligibility(), fx.reservation(), ContiguityRequirement::Required,
      ContinuityRequirement::Required);
  const AllocationDecision decision = fx.runtime.allocate(request);
  WF_REQUIRE(decision.allocated());
  WF_SAME(decision.reservation.raw(), std::uint64_t{2});
  WF_SAME(decision.explanation.candidatesEnumerated, std::size_t{94});
  // Domain 2 holds slot 0, so the first fit that is free on every spanned
  // domain starts at slot 1.
  WF_SAME(decision.explanation.selected.slots.first, 1u);
  WF_SAME(decision.explanation.selected.slots.count, 2u);
  WF_SAME(decision.explanation.selected.anchorDomain.raw(), std::uint64_t{1});
  WF_REQUIRE(decision.explanation.selected.perDomainSlots.size() == 3);
  for (const SlotRange& range : decision.explanation.selected.perDomainSlots) {
    WF_SAME(range.first, 1u);
    WF_SAME(range.count, 2u);
  }
  WF_CHECK(!decision.explanation.selected.crossGrid);

  const std::optional<SpectrumReservation> stored = fx.runtime.reservation(ReservationId(2));
  WF_REQUIRE(stored.has_value());
  WF_SAME(stored->domains.size(), std::size_t{3});
  WF_SAME(stored->domains[0].raw(), std::uint64_t{1});
  WF_SAME(stored->domains[1].raw(), std::uint64_t{2});
  WF_SAME(stored->domains[2].raw(), std::uint64_t{3});
  WF_SAME(stored->domainGenerations.size(), std::size_t{3});
  WF_SAME(stored->perDomainSlots.size(), std::size_t{3});
  WF_SAME(stored->slots.first, 1u);
  WF_SAME(stored->slots.count, 2u);
  WF_SAME(stored->frequency.lowMhz, fixedLow(1));
  WF_SAME(stored->frequency.highMhz, fixedLow(3));
  WF_SAME(stored->anchorDomain.raw(), std::uint64_t{1});
  WF_CHECK(stored->continuityRequired);
  WF_CHECK(stored->contiguityRequired);

  const std::optional<SpectrumUsage> domainOne = fx.runtime.usage(SpectrumDomainId(1), t0);
  const std::optional<SpectrumUsage> domainTwo = fx.runtime.usage(SpectrumDomainId(2), t0);
  const std::optional<SpectrumUsage> domainThree = fx.runtime.usage(SpectrumDomainId(3), t0);
  WF_REQUIRE(domainOne.has_value() && domainTwo.has_value() && domainThree.has_value());
  WF_SAME(domainOne->liveSlots, 2u);
  WF_SAME(domainOne->liveReservations, 1u);
  WF_REQUIRE(domainOne->freeRuns.size() == 2);
  WF_SAME(domainOne->freeRuns[0].first, 0u);
  WF_SAME(domainOne->freeRuns[0].count, 1u);
  WF_SAME(domainOne->freeRuns[1].first, 3u);
  WF_SAME(domainOne->freeRuns[1].count, 93u);
  WF_SAME(domainTwo->liveSlots, 3u);
  WF_SAME(domainTwo->liveReservations, 2u);
  WF_REQUIRE(domainTwo->freeRuns.size() == 1);
  WF_SAME(domainTwo->freeRuns.front().first, 3u);
  WF_SAME(domainThree->liveSlots, 2u);
  WF_SAME(domainThree->liveReservations, 1u);

  // A multi-resource request that does not state continuity is structurally
  // invalid rather than guessed.
  SpectrumRequest unstated = request;
  unstated.requestId = AllocationRequestId(52);
  unstated.continuity = ContinuityRequirement::Unspecified;
  const AllocationDecision refused = fx.runtime.allocate(unstated);
  WF_SAME(codeOf(refused.status), "invalid-argument");
  WF_SAME(outcomeOf(refused.outcome), "refused-invalid-request");
  WF_CHECK(mentions(refused.explanation.reasons, "continuity must be stated explicitly"));
  WF_SAME(fx.runtime.reservations().size(), std::size_t{2});

  // Members must be strictly ascending and unique.
  SpectrumRequest repeated = makeRequest(
      AllocationRequestId(53), OwnerId(7), {SpectrumDomainId(1), SpectrumDomainId(1)},
      {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)}, fixedGridId, fixedGridGen, 1, t0, oneHour,
      fx.eligibility(), fx.reservation(), ContiguityRequirement::Required, ContinuityRequirement::Required);
  WF_SAME(codeOf(fx.runtime.allocate(repeated).status), "invalid-argument");
  SpectrumRequest descending = makeRequest(
      AllocationRequestId(54), OwnerId(7), {SpectrumDomainId(2), SpectrumDomainId(1)},
      {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)}, fixedGridId, fixedGridGen, 1, t0, oneHour,
      fx.eligibility(), fx.reservation(), ContiguityRequirement::Required, ContinuityRequirement::Required);
  WF_SAME(codeOf(fx.runtime.allocate(descending).status), "invalid-argument");
  SpectrumRequest mismatched = makeRequest(
      AllocationRequestId(55), OwnerId(7), {SpectrumDomainId(1), SpectrumDomainId(3)},
      {SpectrumDomainGeneration(1)}, fixedGridId, fixedGridGen, 1, t0, oneHour, fx.eligibility(),
      fx.reservation(), ContiguityRequirement::Required, ContinuityRequirement::Required);
  WF_SAME(codeOf(fx.runtime.allocate(mismatched).status), "invalid-argument");
  WF_SAME(fx.runtime.reservations().size(), std::size_t{2});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{4});

  // Releasing the multi-domain reservation frees its spectrum on every domain.
  WF_SAME(codeOf(fx.runtime.release(ReservationId(2), ReservationGeneration(1), fx.release(),
                                    t0 + Duration::seconds(1))),
          "ok");
  const std::optional<SpectrumUsage> releasedOne = fx.runtime.usage(SpectrumDomainId(1), t0);
  const std::optional<SpectrumUsage> releasedTwo = fx.runtime.usage(SpectrumDomainId(2), t0);
  const std::optional<SpectrumUsage> releasedThree = fx.runtime.usage(SpectrumDomainId(3), t0);
  WF_REQUIRE(releasedOne.has_value() && releasedTwo.has_value() && releasedThree.has_value());
  WF_SAME(releasedOne->liveSlots, 0u);
  WF_SAME(releasedOne->freeSlots, 96u);
  WF_SAME(releasedOne->releasedReservations, 1u);
  WF_SAME(releasedOne->freeRuns.size(), std::size_t{1});
  WF_SAME(releasedOne->freeRuns.front().count, 96u);
  WF_SAME(releasedTwo->liveSlots, 1u);
  WF_SAME(releasedTwo->releasedReservations, 1u);
  WF_SAME(releasedThree->liveSlots, 0u);
  WF_SAME(releasedThree->releasedReservations, 1u);
}

// ---------------------------------------------------------------------------
// Saturation
// ---------------------------------------------------------------------------

WF_TEST(a_saturated_domain_refuses_without_creating_ownership) {
  Fixture fx;
  registerGrids(fx.runtime);
  // Four allocatable flex slots; single-slot reservations are pinned to slots 1
  // and 3, leaving no run that can hold a two-slot channel.
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, 96, SpectrumDomainGeneration(1), flexGridGen, 4);

  SpectrumRequest one =
      singleDomainRequest(AllocationRequestId(60), SpectrumDomainId(1), flexGridId, flexGridGen, 1, t0,
                          oneHour, fx);
  pinToFlexSlot(one, 1);
  const AllocationDecision first = fx.runtime.allocate(one);
  WF_REQUIRE(first.allocated());
  WF_SAME(first.reservation.raw(), std::uint64_t{1});
  WF_SAME(first.explanation.candidatesEnumerated, std::size_t{1});
  const std::optional<SpectrumReservation> pinned = fx.runtime.reservation(ReservationId(1));
  WF_REQUIRE(pinned.has_value());
  WF_SAME(pinned->slots.first, 1u);

  SpectrumRequest three = one;
  three.requestId = AllocationRequestId(61);
  three.frequencyWindows.clear();
  pinToFlexSlot(three, 3);
  const AllocationDecision second = fx.runtime.allocate(three);
  WF_REQUIRE(second.allocated());
  WF_SAME(second.reservation.raw(), std::uint64_t{2});
  const std::optional<SpectrumReservation> pinnedThree = fx.runtime.reservation(ReservationId(2));
  WF_REQUIRE(pinnedThree.has_value());
  WF_SAME(pinnedThree->slots.first, 3u);

  const std::optional<SpectrumUsage> fragmented = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(fragmented.has_value());
  WF_SAME(fragmented->allocatableSlots, 4u);
  WF_SAME(fragmented->liveSlots, 2u);
  WF_SAME(fragmented->freeSlots, 2u);
  WF_REQUIRE(fragmented->freeRuns.size() == 2);
  WF_SAME(fragmented->freeRuns[0].first, 0u);
  WF_SAME(fragmented->freeRuns[0].count, 1u);
  WF_SAME(fragmented->freeRuns[1].first, 2u);
  WF_SAME(fragmented->freeRuns[1].count, 1u);

  // No free run is wide enough and no single blocking interval can host the
  // channel either, so the refusal is reported as no capacity, not as conflict.
  SpectrumRequest wide = makeRequest(AllocationRequestId(62), OwnerId(7), {SpectrumDomainId(1)},
                                     {SpectrumDomainGeneration(1)}, flexGridId, flexGridGen, 2, t0,
                                     oneHour, fx.eligibility(), fx.reservation(),
                                     ContiguityRequirement::Required, ContinuityRequirement::Unspecified);
  const CandidateSet starvedSet = fx.runtime.enumerateCandidates(wide);
  WF_SAME(codeOf(starvedSet.status), "ok");
  WF_CHECK(starvedSet.candidates.empty());
  WF_CHECK(starvedSet.firstEligible() == nullptr);
  WF_SAME(starvedSet.eligibleCount, std::size_t{0});

  const AllocationDecision starved = fx.runtime.allocate(wide);
  WF_SAME(codeOf(starved.status), "unavailable");
  WF_SAME(outcomeOf(starved.outcome), "refused-no-capacity");
  WF_CHECK(!starved.allocated());
  WF_SAME(starved.reservation.raw(), std::uint64_t{0});
  WF_SAME(starved.explanation.candidatesEnumerated, std::size_t{0});
  WF_SAME(starved.explanation.candidatesRejected, std::size_t{0});
  WF_CHECK(starved.explanation.conflicts.empty());
  WF_CHECK(starved.explanation.rejected.empty());
  WF_REQUIRE(starved.explanation.reasons.size() == 1);
  WF_SAME(starved.explanation.reasons.front(), std::string("no candidate satisfies the request"));
  WF_SAME(fx.runtime.reservations().size(), std::size_t{2});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{2});

  // A refusal consumes no reservation identity and records no request identity:
  // once capacity exists the same request id commits under a fresh identity.
  const std::optional<SpectrumUsage> unchanged = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(unchanged.has_value());
  WF_SAME(unchanged->liveSlots, 2u);
  WF_SAME(unchanged->liveReservations, 2u);
  WF_SAME(codeOf(fx.runtime.release(ReservationId(1), ReservationGeneration(1), fx.release(), t0)), "ok");
  WF_SAME(codeOf(fx.runtime.release(ReservationId(2), ReservationGeneration(1), fx.release(), t0)), "ok");
  const AllocationDecision retried = fx.runtime.allocate(wide);
  WF_REQUIRE(retried.allocated());
  WF_SAME(retried.reservation.raw(), std::uint64_t{3});
  WF_SAME(retried.explanation.selected.slots.first, 0u);
  WF_SAME(retried.explanation.selected.slots.count, 2u);
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{2});

  // A domain that is fully owned by narrow reservations reports the blocking
  // identities instead of no capacity.
  Fixture blocked;
  registerGrids(blocked.runtime);
  (void)blocked.addDomain(SpectrumDomainId(1), flexGridId, 96, SpectrumDomainGeneration(1), flexGridGen, 2);
  const AllocationDecision fillOne = blocked.runtime.allocate(singleDomainRequest(
      AllocationRequestId(63), SpectrumDomainId(1), flexGridId, flexGridGen, 1, t0, oneHour, blocked));
  const AllocationDecision fillTwo = blocked.runtime.allocate(singleDomainRequest(
      AllocationRequestId(64), SpectrumDomainId(1), flexGridId, flexGridGen, 1, t0, oneHour, blocked));
  WF_REQUIRE(fillOne.allocated() && fillTwo.allocated());
  WF_SAME(fillOne.explanation.selected.slots.first, 0u);
  WF_SAME(fillTwo.explanation.selected.slots.first, 1u);
  SpectrumRequest full = makeRequest(AllocationRequestId(65), OwnerId(7), {SpectrumDomainId(1)},
                                     {SpectrumDomainGeneration(1)}, flexGridId, flexGridGen, 2, t0,
                                     oneHour, blocked.eligibility(), blocked.reservation(),
                                     ContiguityRequirement::Required, ContinuityRequirement::Unspecified);
  const AllocationDecision conflict = blocked.runtime.allocate(full);
  WF_SAME(codeOf(conflict.status), "conflict");
  WF_SAME(outcomeOf(conflict.outcome), "refused-conflict");
  WF_REQUIRE(conflict.explanation.conflicts.size() == 2);
  WF_SAME(conflict.explanation.conflicts[0].raw(), std::uint64_t{1});
  WF_SAME(conflict.explanation.conflicts[1].raw(), std::uint64_t{2});
  WF_SAME(conflict.explanation.candidatesEnumerated, std::size_t{1});
  WF_SAME(conflict.explanation.candidatesRejected, std::size_t{1});
  WF_SAME(conflict.explanation.candidatesOmitted, std::size_t{0});
  WF_REQUIRE(conflict.explanation.rejected.size() == 1);
  WF_SAME(eligibilityOf(conflict.explanation.rejected.front().eligibility), "ineligible-conflict");
  WF_REQUIRE(conflict.explanation.rejected.front().conflicts.size() == 2);
  WF_SAME(blocked.runtime.reservations().size(), std::size_t{2});
  WF_SAME(blocked.runtime.stats().allocationsRefused, std::uint64_t{1});
}

// ---------------------------------------------------------------------------
// Refusal taxonomy
// ---------------------------------------------------------------------------

WF_TEST(refusals_report_exact_typed_outcomes_and_create_nothing) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  // Unknown grid: the request names a grid the runtime never registered.
  SpectrumRequest unknownGrid = singleDomainRequest(AllocationRequestId(70), SpectrumDomainId(1),
                                                    fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  unknownGrid.grid = ChannelGridId(99);
  const AllocationDecision gridMissing = fx.runtime.allocate(unknownGrid);
  WF_SAME(codeOf(gridMissing.status), "invalid-argument");
  WF_SAME(outcomeOf(gridMissing.outcome), "refused-invalid-request");
  WF_CHECK(mentions(gridMissing.explanation.reasons, "is not registered"));

  // Unknown domain.
  SpectrumRequest unknownDomain = singleDomainRequest(AllocationRequestId(71), SpectrumDomainId(9),
                                                      fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  const AllocationDecision domainMissing = fx.runtime.allocate(unknownDomain);
  WF_SAME(codeOf(domainMissing.status), "not-found");
  WF_SAME(outcomeOf(domainMissing.outcome), "refused-unknown-domain");
  WF_CHECK(mentions(domainMissing.explanation.reasons, "is not registered"));

  // Registered domain without any published capability.
  (void)fx.runtime.registerDomain(makeDomain(SpectrumDomainId(2), fixedGridId));
  const AllocationDecision noCapability = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(72), SpectrumDomainId(2), fixedGridId, fixedGridGen, 1, t0, oneHour, fx));
  WF_SAME(codeOf(noCapability.status), "unknown");
  WF_SAME(outcomeOf(noCapability.outcome), "refused-unknown-capability");
  WF_CHECK(mentions(noCapability.explanation.reasons, "no registered spectrum capability"));

  // Stale grid generation and stale domain generation.
  SpectrumRequest staleGrid = singleDomainRequest(AllocationRequestId(73), SpectrumDomainId(1),
                                                  fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  staleGrid.gridGeneration = GridGeneration(2);
  const AllocationDecision gridStale = fx.runtime.allocate(staleGrid);
  WF_SAME(codeOf(gridStale.status), "stale-generation");
  WF_SAME(outcomeOf(gridStale.outcome), "refused-stale-generation");
  WF_CHECK(mentions(gridStale.explanation.reasons, "registered at generation"));

  SpectrumRequest staleDomain = singleDomainRequest(AllocationRequestId(74), SpectrumDomainId(1),
                                                    fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  staleDomain.domainGenerations[0] = SpectrumDomainGeneration(2);
  const AllocationDecision domainStale = fx.runtime.allocate(staleDomain);
  WF_SAME(codeOf(domainStale.status), "stale-generation");
  WF_SAME(outcomeOf(domainStale.outcome), "refused-stale-generation");

  // A capability published for a domain generation the domain has left behind.
  Fixture moving;
  registerGrids(moving.runtime);
  (void)moving.addDomain(SpectrumDomainId(1), fixedGridId, 96);
  WF_SAME(codeOf(moving.runtime.registerDomain(makeDomain(SpectrumDomainId(1), fixedGridId,
                                                         SpectrumDomainGeneration(2)))),
          "ok");
  SpectrumRequest orphaned = singleDomainRequest(AllocationRequestId(75), SpectrumDomainId(1),
                                                 fixedGridId, fixedGridGen, 1, t0, oneHour, moving);
  orphaned.domainGenerations[0] = SpectrumDomainGeneration(2);
  const AllocationDecision staleCapability = moving.runtime.allocate(orphaned);
  WF_SAME(codeOf(staleCapability.status), "stale-generation");
  WF_SAME(outcomeOf(staleCapability.outcome), "refused-stale-capability");
  WF_CHECK(mentions(staleCapability.explanation.reasons, "capability for domain"));

  // UNSUPPORTED and UNKNOWN capabilities are distinct refusals.
  Fixture unsupported;
  registerGrids(unsupported.runtime);
  (void)unsupported.runtime.registerDomain(makeDomain(SpectrumDomainId(1), fixedGridId));
  const SpectrumCapability unsupportedCapability =
      makeCapability(SpectrumDomainId(1), fixedGridId, SpectrumDomainGeneration(1), fixedGridGen,
                     SpectrumSupport::Unsupported, 0, 0, unsupported.runtime.fence());
  WF_SAME(codeOf(unsupported.runtime.publishCapability(unsupportedCapability)), "ok");
  const AllocationDecision unsupportedDecision = unsupported.runtime.allocate(singleDomainRequest(
      AllocationRequestId(76), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour, unsupported));
  WF_SAME(codeOf(unsupportedDecision.status), "unsupported");
  WF_SAME(outcomeOf(unsupportedDecision.outcome), "refused-unsupported");

  Fixture unknownSupport;
  registerGrids(unknownSupport.runtime);
  (void)unknownSupport.runtime.registerDomain(makeDomain(SpectrumDomainId(1), fixedGridId));
  const SpectrumCapability unknownCapability =
      makeCapability(SpectrumDomainId(1), fixedGridId, SpectrumDomainGeneration(1), fixedGridGen,
                     SpectrumSupport::Unknown, 0, 0, unknownSupport.runtime.fence());
  WF_SAME(codeOf(unknownSupport.runtime.publishCapability(unknownCapability)), "ok");
  const AllocationDecision unknownDecision = unknownSupport.runtime.allocate(singleDomainRequest(
      AllocationRequestId(77), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour,
      unknownSupport));
  WF_SAME(codeOf(unknownDecision.status), "unknown");
  WF_SAME(outcomeOf(unknownDecision.outcome), "refused-unknown-capability");

  // Authority staleness keeps its distinct codes: a token minted by a dead
  // incarnation or a dead epoch is refused even when its generation matches.
  SpectrumRequest staleIncarnation = singleDomainRequest(AllocationRequestId(78), SpectrumDomainId(1),
                                                         fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  staleIncarnation.eligibilityAuthority.fence.incarnation =
      ControllerIncarnation(fx.runtime.incarnation().raw() + 1);
  const AllocationDecision incarnationDecision = fx.runtime.allocate(staleIncarnation);
  WF_SAME(codeOf(incarnationDecision.status), "stale-incarnation");
  WF_SAME(outcomeOf(incarnationDecision.outcome), "refused-stale-incarnation");

  SpectrumRequest staleEpoch = singleDomainRequest(AllocationRequestId(79), SpectrumDomainId(1),
                                                   fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  staleEpoch.reservationAuthority.fence.epoch = ControllerEpoch(fx.runtime.epoch().raw() + 1);
  const AllocationDecision epochDecision = fx.runtime.allocate(staleEpoch);
  WF_SAME(codeOf(epochDecision.status), "stale-epoch");
  WF_SAME(outcomeOf(epochDecision.outcome), "refused-stale-epoch");

  SpectrumRequest staleGeneration = singleDomainRequest(AllocationRequestId(80), SpectrumDomainId(1),
                                                        fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  staleGeneration.eligibilityAuthority.generation = EligibilityAuthorityGeneration(4);
  const AllocationDecision generationDecision = fx.runtime.allocate(staleGeneration);
  WF_SAME(codeOf(generationDecision.status), "stale-generation");
  WF_SAME(outcomeOf(generationDecision.outcome), "refused-stale-generation");

  SpectrumRequest missingToken = singleDomainRequest(AllocationRequestId(81), SpectrumDomainId(1),
                                                     fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  missingToken.eligibilityAuthority = EligibilityAuthority{};
  const AllocationDecision tokenDecision = fx.runtime.allocate(missingToken);
  WF_SAME(codeOf(tokenDecision.status), "invalid-argument");
  WF_SAME(outcomeOf(tokenDecision.outcome), "refused-invalid-request");

  // Contiguity must be stated for a channel wider than one slot.
  SpectrumRequest unstated = singleDomainRequest(AllocationRequestId(82), SpectrumDomainId(1), fixedGridId,
                                                 fixedGridGen, 2, t0, oneHour, fx);
  unstated.contiguity = ContiguityRequirement::Unspecified;
  const AllocationDecision unstatedDecision = fx.runtime.allocate(unstated);
  WF_SAME(codeOf(unstatedDecision.status), "invalid-argument");
  WF_SAME(outcomeOf(unstatedDecision.outcome), "refused-invalid-request");
  WF_CHECK(mentions(unstatedDecision.explanation.reasons, "contiguity must be stated explicitly"));

  // Every refusal left ownership untouched, and none of them evaluated a
  // candidate: the request never became eligible.
  WF_CHECK(fx.runtime.reservations().empty());
  WF_CHECK(fx.runtime.reservationsForDomain(SpectrumDomainId(1)).empty());
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{10});
  WF_SAME(fx.runtime.stats().candidatesEvaluated, std::uint64_t{0});
  const std::optional<SpectrumUsage> usage = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(usage.has_value());
  WF_SAME(usage->liveSlots, 0u);
  WF_SAME(usage->freeSlots, 96u);
  WF_SAME(usage->liveReservations, 0u);

  // The refused request ids were never consumed: the same identity commits now.
  const AllocationDecision late = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(78), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(late.allocated());
  WF_SAME(late.reservation.raw(), std::uint64_t{1});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{1});
}

// ---------------------------------------------------------------------------
// Enumeration bounds and read-only explanation
// ---------------------------------------------------------------------------

WF_TEST(enumeration_truncation_is_accounted_exactly) {
  RuntimeConfig config;
  config.maxCandidatesPerRequest = 4;
  Fixture fx(config);
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  SpectrumRequest request = singleDomainRequest(AllocationRequestId(90), SpectrumDomainId(1), fixedGridId,
                                                fixedGridGen, 1, t0, oneHour, fx);
  const CandidateSet set = fx.runtime.enumerateCandidates(request);
  WF_SAME(codeOf(set.status), "ok");
  WF_SAME(set.candidates.size(), std::size_t{4});
  WF_SAME(set.eligibleCount, std::size_t{4});
  WF_SAME(set.omitted, std::size_t{92});
  WF_CHECK(!set.complete);
  WF_SAME(set.candidates.front().ordinal, std::size_t{0});
  WF_SAME(set.candidates.front().slots.first, 0u);
  WF_SAME(set.candidates.back().slots.first, 3u);
  WF_CHECK(fx.runtime.reservations().empty());
  WF_REQUIRE(set.firstEligible() != nullptr);
  WF_SAME(set.firstEligible()->slots.first, 0u);
  WF_SAME(set.firstEligible()->slots.count, 1u);

  const AllocationDecision decision = fx.runtime.allocate(request);
  WF_REQUIRE(decision.allocated());
  WF_SAME(decision.explanation.candidatesEnumerated, std::size_t{4});  WF_SAME(decision.explanation.candidatesOmitted, std::size_t{92});
  WF_SAME(decision.explanation.selected.slots.first, 0u);
  WF_SAME(fx.runtime.stats().candidatesEvaluated, std::uint64_t{4});
  WF_SAME(fx.runtime.stats().enumerationTruncations, std::uint64_t{1});

  const AllocationDecision next = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(91), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(next.allocated());
  WF_SAME(next.explanation.selected.slots.first, 1u);
  WF_SAME(fx.runtime.stats().candidatesEvaluated, std::uint64_t{8});
  WF_SAME(fx.runtime.stats().enumerationTruncations, std::uint64_t{2});
}

WF_TEST(channel_width_is_bounded_by_the_grid) {
  Fixture fx;
  (void)fx.runtime.registerGrid(fixedGrid(fixedGridId, fixedGridGen));
  (void)fx.runtime.registerGrid(flexGrid(flexGridId, flexGridGen, kAnchorMhz, kFlexSlotMhz, 384, 1, 4));
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);
  (void)fx.addDomain(SpectrumDomainId(2), flexGridId, 96);

  // One slot per channel is what a fixed grid expresses.
  const AllocationDecision single =
      fx.runtime.allocate(singleDomainRequest(AllocationRequestId(96), SpectrumDomainId(1), fixedGridId,
                                              fixedGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(single.allocated());
  WF_SAME(single.explanation.selected.slots.count, 1u);

  SpectrumRequest wide = singleDomainRequest(AllocationRequestId(97), SpectrumDomainId(1), fixedGridId,
                                             fixedGridGen, 2, t0, oneHour, fx);
  const AllocationDecision refused = fx.runtime.allocate(wide);
  WF_SAME(codeOf(refused.status), "invalid-argument");
  WF_SAME(outcomeOf(refused.outcome), "refused-channel-width");
  WF_CHECK(!refused.allocated());
  WF_SAME(refused.reservation.raw(), std::uint64_t{0});
  WF_SAME(refused.explanation.candidatesEnumerated, std::size_t{0});
  WF_CHECK(mentions(refused.explanation.reasons, "does not support a channel of"));
  WF_SAME(fx.runtime.reservations().size(), std::size_t{1});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});

  // A flex grid is bounded by its own per-channel slot limits.
  const AllocationDecision ceiling = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(98), SpectrumDomainId(2), flexGridId, flexGridGen, 4, t0, oneHour, fx));
  WF_REQUIRE(ceiling.allocated());
  WF_SAME(ceiling.explanation.selected.slots.count, 4u);
  const AllocationDecision overCeiling = fx.runtime.allocate(singleDomainRequest(
      AllocationRequestId(99), SpectrumDomainId(2), flexGridId, flexGridGen, 5, t0, oneHour, fx));
  WF_SAME(codeOf(overCeiling.status), "invalid-argument");
  WF_SAME(outcomeOf(overCeiling.outcome), "refused-channel-width");
  WF_CHECK(mentions(overCeiling.explanation.reasons, "does not support a channel of"));
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{2});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{2});

  const std::optional<SpectrumUsage> usage = fx.runtime.usage(SpectrumDomainId(2), t0);
  WF_REQUIRE(usage.has_value());
  WF_SAME(usage->liveSlots, 4u);
  WF_SAME(usage->liveReservations, 1u);
  WF_SAME(usage->freeSlots, 92u);
}

WF_TEST(explain_is_read_only_and_matches_the_committed_values) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, 96);

  SpectrumRequest request = singleDomainRequest(AllocationRequestId(95), SpectrumDomainId(1), fixedGridId,
                                                fixedGridGen, 2, t0, oneHour, fx);
  const DecisionExplanation predicted = fx.runtime.explain(request);
  WF_SAME(outcomeOf(predicted.outcome), "allocated");
  WF_SAME(predicted.selected.slots.first, 0u);
  WF_SAME(predicted.selected.slots.count, 2u);
  WF_CHECK(predicted.rejected.empty());
  WF_CHECK(predicted.conflicts.empty());
  WF_CHECK(fx.runtime.reservations().empty());
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{0});
  WF_SAME(fx.runtime.stats().candidatesEvaluated, std::uint64_t{0});

  const AllocationDecision committed = fx.runtime.allocate(request);
  WF_REQUIRE(committed.allocated());
  WF_SAME(committed.explanation.selected.slots.first, predicted.selected.slots.first);
  WF_SAME(committed.explanation.selected.slots.count, predicted.selected.slots.count);
  WF_SAME(committed.explanation.selected.frequency.lowMhz, predicted.selected.frequency.lowMhz);
  WF_SAME(committed.explanation.selected.frequency.highMhz, predicted.selected.frequency.highMhz);
  WF_SAME(committed.explanation.candidatesEnumerated, predicted.candidatesEnumerated);

  // Explaining a committed identity reports the replay without mutating it.
  const DecisionExplanation replay = fx.runtime.explain(request);
  WF_SAME(outcomeOf(replay.outcome), "allocated");
  WF_SAME(replay.reservation.raw(), std::uint64_t{1});
  WF_SAME(replay.generation.raw(), std::uint64_t{1});
  WF_SAME(replay.selected.slots.first, 0u);
  WF_SAME(replay.selected.slots.count, 2u);
  WF_SAME(fx.runtime.reservations().size(), std::size_t{1});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{1});

  // Explaining a shape clash reports the duplicate refusal and still owns
  // nothing new.
  SpectrumRequest clash = request;
  clash.slots = 3;
  const DecisionExplanation duplicate = fx.runtime.explain(clash);
  WF_SAME(outcomeOf(duplicate.outcome), "refused-duplicate");
  WF_SAME(duplicate.reservation.raw(), std::uint64_t{0});
  WF_SAME(fx.runtime.reservations().size(), std::size_t{1});
}

WF_TEST_MAIN()
