// Lease lifecycle: validity at an instant, the boundary between a live and a
// lapsed lease, conflict detection against a lapsed lease, the expiry sweep,
// reclamation accounting, and renewal (extension, counter, cap and fencing).
//
// Every instant is supplied by the test; nothing here reads the wall clock.

#include "test_common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace wavelength_fabric;

const Instant kStart = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::minutes(10);

// SlotRange has no stream operator and its braced form carries a comma, which a
// function-like macro would split; this comparison keeps WF_CHECK usable.
[[nodiscard]] bool isRange(SlotRange range, std::uint32_t first, std::uint32_t count) {
  return range.first == first && range.count == count;
}

// One fixed 96-slot grid carrying one fully allocatable domain, plus the request
// builders the cases below share.
class Env {
 public:
  // The allocatable window defaults to the whole 96-slot grid; a single-slot
  // window makes every competing request a conflict, which is what the expiry
  // cases need.
  explicit Env(std::uint32_t allocatableSlots = 96) {
    (void)fixture.runtime.registerGrid(wf_test::fixedGrid(grid_, GridGeneration(1)));
    (void)fixture.addDomain(domain_, grid_, 96, SpectrumDomainGeneration(1), GridGeneration(1),
                            allocatableSlots);
  }

  [[nodiscard]] SpectrumRuntime& runtime() { return fixture.runtime; }

  [[nodiscard]] SpectrumDomainId domain() const { return domain_; }

  [[nodiscard]] SpectrumRequest request(AllocationRequestId id, Instant at, Duration lease,
                                        std::uint32_t maxRenewals = 0) const {
    SpectrumRequest value =
        wf_test::makeRequest(id, OwnerId(7), {domain_}, {SpectrumDomainGeneration(1)}, grid_,
                             GridGeneration(1), 1, at, lease, fixture.eligibility(),
                             fixture.reservation());
    value.maxRenewals = maxRenewals;
    return value;
  }

  [[nodiscard]] AllocationDecision allocate(AllocationRequestId id, Instant at, Duration lease,
                                            std::uint32_t maxRenewals = 0) {
    return fixture.runtime.allocate(request(id, at, lease, maxRenewals));
  }

  [[nodiscard]] std::optional<SpectrumReservation> stored(ReservationId id) const {
    return fixture.runtime.reservation(id);
  }

  [[nodiscard]] std::size_t auditsOfKind(AuditKind kind) const {
    std::size_t count = 0;
    for (const AuditRecord& record : fixture.runtime.audit(AuditSequence(0), 0)) {
      if (record.kind == kind) ++count;
    }
    return count;
  }

  [[nodiscard]] std::optional<AuditRecord> lastAudit() const {
    const std::vector<AuditRecord> records = fixture.runtime.audit(AuditSequence(0), 0);
    if (records.empty()) return std::nullopt;
    return records.back();
  }

  wf_test::Fixture fixture;

 private:
  SpectrumDomainId domain_{SpectrumDomainId(1)};
  ChannelGridId grid_{ChannelGridId(1)};
};

WF_TEST(a_lease_is_valid_before_its_expiry_and_lapsed_at_it) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  WF_CHECK_EQ(decision.reservation.raw(), std::uint64_t{1});

  const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->state == ReservationState::Reserved);
  WF_CHECK(stored->lease.grantedAt == kStart);
  WF_CHECK(stored->lease.expiresAt == kStart + kLease);
  WF_CHECK(stored->createdAt == kStart);
  WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{0});
  WF_CHECK_EQ(stored->lease.maxRenewals, std::uint32_t{0});
  WF_CHECK_EQ(stored->lease.generation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});

  const Instant lastLiveInstant = (kStart + kLease) - Duration::nanos(1);
  const Instant expiryInstant = kStart + kLease;
  WF_CHECK(stored->lease.validAt(kStart));
  WF_CHECK(stored->lease.validAt(lastLiveInstant));
  WF_CHECK(stored->isLiveAt(kStart));
  WF_CHECK(stored->isLiveAt(lastLiveInstant));
  WF_CHECK(!stored->lease.validAt(expiryInstant));
  WF_CHECK(stored->lease.expiredAt(expiryInstant));
  WF_CHECK(!stored->isLiveAt(expiryInstant));

  const std::optional<SpectrumUsage> live = runtime.usage(env.domain(), lastLiveInstant);
  WF_REQUIRE(live.has_value());
  WF_CHECK_EQ(live->freeSlots, std::uint32_t{95});
  WF_CHECK_EQ(live->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(live->liveReservations, std::uint32_t{1});
  WF_CHECK_EQ(live->lapsedSlots, std::uint32_t{0});
  WF_CHECK_EQ(live->lapsedReservations, std::uint32_t{0});
  WF_CHECK_EQ(live->freeRuns.size(), std::size_t{1});
  WF_CHECK(isRange(live->freeRuns[0], 1, 95));

  // At the expiry instant the reservation owns no live spectrum, but nothing has
  // moved it out of Reserved: only an explicit sweep does that.
  const std::optional<SpectrumUsage> lapsed = runtime.usage(env.domain(), expiryInstant);
  WF_REQUIRE(lapsed.has_value());
  WF_CHECK_EQ(lapsed->liveSlots, std::uint32_t{0});
  WF_CHECK_EQ(lapsed->lapsedSlots, std::uint32_t{1});
  WF_CHECK_EQ(lapsed->lapsedReservations, std::uint32_t{1});
  WF_CHECK_EQ(lapsed->liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(lapsed->freeSlots, std::uint32_t{96});
  WF_CHECK_EQ(lapsed->reclaimedReservations, std::uint32_t{0});
  const std::optional<SpectrumReservation> untouched = env.stored(decision.reservation);
  WF_REQUIRE(untouched.has_value());
  WF_CHECK(untouched->state == ReservationState::Reserved);
  WF_CHECK_EQ(untouched->generation.raw(), std::uint64_t{1});
}

WF_TEST(a_lapsed_lease_stops_blocking_without_being_reclaimed) {
  Env env(1);
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision first = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(first.allocated());

  // One nanosecond before the expiry the lease is still live, so an identical
  // request under another identity conflicts and is refused.
  const AllocationDecision blocked =
      env.allocate(AllocationRequestId(2), (kStart + kLease) - Duration::nanos(1), kLease);
  WF_CHECK(!blocked.allocated());
  WF_CHECK(blocked.outcome == AllocationOutcome::RefusedConflict);
  WF_CHECK_EQ(std::string(toToken(blocked.status.code)), std::string("conflict"));
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{1});
  WF_REQUIRE(blocked.explanation.conflicts.size() == std::size_t{1});
  WF_CHECK(blocked.explanation.conflicts[0] == first.reservation);

  // At the expiry instant the same request is evaluated against a free pool: the
  // lapsed reservation is ignored by conflict detection and its slot is reused.
  const AllocationDecision reused = env.allocate(AllocationRequestId(3), kStart + kLease, kLease);
  WF_REQUIRE(reused.allocated());
  WF_CHECK(!(reused.reservation == first.reservation));
  const std::optional<SpectrumReservation> second = env.stored(reused.reservation);
  WF_REQUIRE(second.has_value());
  WF_CHECK(isRange(second->slots, 0, 1));
  WF_CHECK(second->lease.grantedAt == kStart + kLease);
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});

  // The lapsed reservation was not moved to Expired by the allocation itself.
  const std::optional<SpectrumReservation> original = env.stored(first.reservation);
  WF_REQUIRE(original.has_value());
  WF_CHECK(original->state == ReservationState::Reserved);
  WF_CHECK_EQ(original->generation.raw(), std::uint64_t{1});
  WF_CHECK(original->updatedAt == kStart);

  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart + kLease);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->allocatableSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->lapsedSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{0});
  WF_CHECK(usage->freeRuns.empty());
}

WF_TEST(the_expiry_sweep_marks_exactly_the_lapsed_reservations) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision shortLease =
      env.allocate(AllocationRequestId(1), kStart, Duration::minutes(5));
  const AllocationDecision longLease =
      env.allocate(AllocationRequestId(2), kStart, Duration::minutes(20));
  WF_REQUIRE(shortLease.allocated());
  WF_REQUIRE(longLease.allocated());

  const Instant sweepAt = kStart + Duration::minutes(5);
  const ReclaimReport report = runtime.expireLeases(sweepAt);
  WF_CHECK(report.status.ok());
  WF_CHECK(report.evaluatedAt == sweepAt);
  WF_CHECK(report.fence == runtime.fence());
  WF_CHECK_EQ(report.scanned, std::size_t{2});
  WF_CHECK_EQ(report.expired, std::size_t{1});
  WF_CHECK_EQ(report.lapsed.size(), std::size_t{1});
  WF_CHECK(report.lapsed[0] == shortLease.reservation);
  WF_CHECK(report.reclaimed.empty());
  WF_CHECK_EQ(runtime.stats().expirationSweeps, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{0});

  const std::optional<SpectrumReservation> lapsed = env.stored(shortLease.reservation);
  WF_REQUIRE(lapsed.has_value());
  WF_CHECK(lapsed->state == ReservationState::Expired);
  WF_CHECK(lapsed->updatedAt == sweepAt);
  WF_CHECK(lapsed->lease.expiresAt == kStart + Duration::minutes(5));
  WF_CHECK_EQ(lapsed->lease.renewalCount, std::uint32_t{0});
  WF_CHECK_EQ(lapsed->lastOperation.raw(), std::uint64_t{2});
  const std::optional<SpectrumReservation> live = env.stored(longLease.reservation);
  WF_REQUIRE(live.has_value());
  WF_CHECK(live->state == ReservationState::Reserved);
  WF_CHECK(live->updatedAt == kStart);

  // Expiring never returns spectrum: reclamation is a separate accounting step.
  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), sweepAt);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->lapsedSlots, std::uint32_t{0});
  WF_CHECK_EQ(usage->reclaimedReservations, std::uint32_t{0});
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{95});

  // A second sweep has nothing left to do and changes no state at all.
  const std::size_t auditsAfterFirstSweep = runtime.auditSize();
  const ReclaimReport repeated = runtime.expireLeases(sweepAt);
  WF_CHECK(repeated.status.ok());
  WF_CHECK_EQ(repeated.scanned, std::size_t{2});
  WF_CHECK_EQ(repeated.expired, std::size_t{0});
  WF_CHECK(repeated.lapsed.empty());
  WF_CHECK(repeated.reclaimed.empty());
  WF_CHECK_EQ(runtime.auditSize(), auditsAfterFirstSweep);
  WF_CHECK_EQ(runtime.stats().expirationSweeps, std::uint64_t{2});
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{0});
  const std::optional<SpectrumReservation> stillExpired = env.stored(shortLease.reservation);
  WF_REQUIRE(stillExpired.has_value());
  WF_CHECK(stillExpired->state == ReservationState::Expired);
  WF_CHECK(stillExpired->updatedAt == sweepAt);
  WF_CHECK_EQ(stillExpired->lastOperation.raw(), std::uint64_t{2});

  // Sweeping later catches the longer lease exactly once.
  const ReclaimReport later = runtime.expireLeases(kStart + Duration::minutes(20));
  WF_CHECK_EQ(later.expired, std::size_t{1});
  WF_CHECK_EQ(later.lapsed.size(), std::size_t{1});
  WF_CHECK(later.lapsed[0] == longLease.reservation);
}

WF_TEST(reclaiming_restores_the_exact_pre_allocation_free_pool) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const std::optional<SpectrumUsage> baseline = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(baseline.has_value());
  WF_CHECK_EQ(baseline->freeSlots, std::uint32_t{96});
  WF_CHECK_EQ(baseline->freeRuns.size(), std::size_t{1});
  WF_CHECK(isRange(baseline->freeRuns[0], 0, 96));

  const AllocationDecision first = env.allocate(AllocationRequestId(1), kStart, kLease);
  const AllocationDecision second = env.allocate(AllocationRequestId(2), kStart, kLease);
  WF_REQUIRE(first.allocated());
  WF_REQUIRE(second.allocated());
  const std::optional<SpectrumUsage> owned = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(owned.has_value());
  WF_CHECK_EQ(owned->freeSlots, std::uint32_t{94});
  WF_CHECK_EQ(owned->liveSlots, std::uint32_t{2});
  WF_CHECK_EQ(owned->freeRuns.size(), std::size_t{1});
  WF_CHECK(isRange(owned->freeRuns[0], 2, 94));

  const Instant reclaimAt = kStart + kLease;
  const ReclaimReport report = runtime.reclaimExpired(reclaimAt);
  WF_CHECK(report.status.ok());
  WF_CHECK(report.evaluatedAt == reclaimAt);
  WF_CHECK(report.fence == runtime.fence());
  WF_CHECK_EQ(report.scanned, std::size_t{2});
  WF_CHECK_EQ(report.expired, std::size_t{2});
  WF_CHECK_EQ(report.lapsed.size(), std::size_t{2});
  WF_CHECK_EQ(report.reclaimed.size(), std::size_t{2});
  WF_CHECK(report.lapsed[0] == first.reservation);
  WF_CHECK(report.lapsed[1] == second.reservation);
  WF_CHECK(report.reclaimed[0] == first.reservation);
  WF_CHECK(report.reclaimed[1] == second.reservation);

  for (const ReservationId id : {first.reservation, second.reservation}) {
    const std::optional<SpectrumReservation> stored = env.stored(id);
    WF_REQUIRE(stored.has_value());
    WF_CHECK(stored->state == ReservationState::Reclaimed);
    WF_CHECK(stored->reclaimedAt == reclaimAt);
    WF_CHECK(stored->updatedAt == reclaimAt);
  }

  const std::optional<SpectrumUsage> restored = runtime.usage(env.domain(), reclaimAt);
  WF_REQUIRE(restored.has_value());
  WF_CHECK_EQ(restored->freeSlots, baseline->freeSlots);
  WF_CHECK_EQ(restored->freeRuns.size(), baseline->freeRuns.size());
  WF_CHECK(restored->freeRuns[0] == baseline->freeRuns[0]);
  WF_CHECK_EQ(restored->liveSlots, std::uint32_t{0});
  WF_CHECK_EQ(restored->lapsedSlots, std::uint32_t{0});
  WF_CHECK_EQ(restored->liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(restored->reclaimedReservations, std::uint32_t{2});
  WF_CHECK_EQ(restored->releasedReservations, std::uint32_t{0});
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{2});

  const ReclaimReport repeated = runtime.reclaimExpired(reclaimAt);
  WF_CHECK_EQ(repeated.expired, std::size_t{0});
  WF_CHECK(repeated.reclaimed.empty());
  WF_CHECK(repeated.lapsed.empty());
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{2});

  // A reservation whose lease is still valid is never expired or reclaimed.
  const AllocationDecision third = env.allocate(AllocationRequestId(3), kStart, kLease);
  WF_REQUIRE(third.allocated());
  const ReclaimReport whileLive = runtime.reclaimExpired(kStart + Duration::minutes(1));
  WF_CHECK_EQ(whileLive.expired, std::size_t{0});
  WF_CHECK(whileLive.reclaimed.empty());
  const std::optional<SpectrumReservation> live = env.stored(third.reservation);
  WF_REQUIRE(live.has_value());
  WF_CHECK(live->state == ReservationState::Reserved);
  WF_CHECK(live->updatedAt == kStart);
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{2});
}

WF_TEST(renewal_extends_from_the_current_expiry_and_advances_generations) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease, 3);
  WF_REQUIRE(decision.allocated());
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK_EQ(stored->lease.maxRenewals, std::uint32_t{3});
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{0});
  }

  const Duration extension = Duration::minutes(2);
  const Instant firstRenewalAt = kStart + Duration::minutes(1);
  const Status first = runtime.renew(decision.reservation, ReservationGeneration(1), extension,
                                     env.fixture.reservation(), firstRenewalAt, 0);
  WF_CHECK(first.ok());
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK(stored->lease.expiresAt == kStart + kLease + extension);
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{1});
    WF_CHECK_EQ(stored->lease.maxRenewals, std::uint32_t{3});
    WF_CHECK_EQ(stored->lease.generation.raw(), std::uint64_t{2});
    WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{2});
    WF_CHECK(stored->updatedAt == firstRenewalAt);
    WF_CHECK(stored->state == ReservationState::Reserved);
    WF_CHECK(stored->lease.grantedAt == kStart);
  }

  // The second renewal is named at the generation the first one produced.
  const Status stale = runtime.renew(decision.reservation, ReservationGeneration(1), extension,
                                     env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(stale.code)), std::string("stale-generation"));
  const Status second = runtime.renew(decision.reservation, ReservationGeneration(2), extension,
                                      env.fixture.reservation(), kStart + Duration::minutes(2), 0);
  WF_CHECK(second.ok());
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK(stored->lease.expiresAt == kStart + kLease + extension + extension);
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{2});
    WF_CHECK_EQ(stored->lease.generation.raw(), std::uint64_t{3});
    WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{3});
  }
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{2});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationRenewed), std::size_t{2});

  const std::optional<AuditRecord> last = env.lastAudit();
  WF_REQUIRE(last.has_value());
  WF_CHECK(last->kind == AuditKind::ReservationRenewed);
  WF_CHECK_EQ(last->reservationGeneration.raw(), std::uint64_t{3});
  WF_CHECK(last->at == kStart + Duration::minutes(2));
}

WF_TEST(the_renewal_counter_is_bounded_by_the_lease_cap) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease, 2);
  WF_REQUIRE(decision.allocated());
  const Duration extension = Duration::minutes(1);

  WF_CHECK(runtime
               .renew(decision.reservation, ReservationGeneration(1), extension,
                      env.fixture.reservation(), kStart, 0)
               .ok());
  WF_CHECK(runtime
               .renew(decision.reservation, ReservationGeneration(2), extension,
                      env.fixture.reservation(), kStart, 0)
               .ok());
  const std::optional<SpectrumReservation> atCap = env.stored(decision.reservation);
  WF_REQUIRE(atCap.has_value());
  WF_CHECK_EQ(atCap->lease.renewalCount, std::uint32_t{2});
  WF_CHECK_EQ(atCap->lease.maxRenewals, std::uint32_t{2});
  const Instant expiryAtCap = atCap->lease.expiresAt;
  const std::size_t auditsAtCap = runtime.auditSize();

  // The third renewal is past the cap the lease carries. It is refused, no
  // counter moves, no expiry moves and no audit record is written.
  const Status capped = runtime.renew(decision.reservation, ReservationGeneration(3), extension,
                                      env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(capped.code)), std::string("limit-exceeded"));
  WF_CHECK(!capped.message.empty());
  WF_CHECK_EQ(runtime.auditSize(), auditsAtCap);
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK(stored->lease.expiresAt == expiryAtCap);
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{2});
    WF_CHECK_EQ(stored->lease.maxRenewals, std::uint32_t{2});
    WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{3});
    WF_CHECK(stored->state == ReservationState::Reserved);
  }
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{2});
  WF_CHECK_EQ(runtime.stats().replayRejections, std::uint64_t{0});

  // A caller-named cap at or below the current count is refused before it is applied.
  const Status tooLow = runtime.renew(decision.reservation, ReservationGeneration(3), extension,
                                      env.fixture.reservation(), kStart, 2);
  WF_CHECK_EQ(std::string(toToken(tooLow.code)), std::string("limit-exceeded"));
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{2});
}

WF_TEST(a_named_cap_bounds_a_lease_that_started_uncapped) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  // maxRenewals == 0 on the request means the lease carries no fixed cap.
  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  const Duration extension = Duration::seconds(30);

  const Status first = runtime.renew(decision.reservation, ReservationGeneration(1), extension,
                                     env.fixture.reservation(), kStart, 2);
  WF_CHECK(first.ok());
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{1});
    WF_CHECK_EQ(stored->lease.maxRenewals, std::uint32_t{2});
  }

  const Status second = runtime.renew(decision.reservation, ReservationGeneration(2), extension,
                                      env.fixture.reservation(), kStart, 0);
  WF_CHECK(second.ok());
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{2});
    WF_CHECK_EQ(stored->lease.maxRenewals, std::uint32_t{2});
    WF_CHECK(stored->lease.expiresAt == kStart + kLease + extension + extension);
  }

  const Status third = runtime.renew(decision.reservation, ReservationGeneration(3), extension,
                                     env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(third.code)), std::string("limit-exceeded"));
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{2});
}

WF_TEST(renewal_extensions_must_be_positive_and_bounded) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  const std::size_t auditsBefore = runtime.auditSize();

  const Status zero = runtime.renew(decision.reservation, ReservationGeneration(1), Duration::zero(),
                                    env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(zero.code)), std::string("invalid-argument"));
  const Status negative =
      runtime.renew(decision.reservation, ReservationGeneration(1), Duration::nanos(-1),
                    env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(negative.code)), std::string("invalid-argument"));
  WF_CHECK_EQ(runtime.auditSize(), auditsBefore);
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{0});

  // Exactly 3650 days is the documented ceiling and is accepted; one nanosecond
  // beyond it is not.
  const Duration tenYears = Duration::days(3650);
  const Status boundary = runtime.renew(decision.reservation, ReservationGeneration(1), tenYears,
                                        env.fixture.reservation(), kStart, 0);
  WF_CHECK(boundary.ok());
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK(stored->lease.expiresAt == kStart + kLease + tenYears);
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{1});
  }
  const Status beyond =
      runtime.renew(decision.reservation, ReservationGeneration(2),
                    Duration::nanos(tenYears.nanos() + 1), env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(beyond.code)), std::string("invalid-argument"));
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{1});

  // An extension whose result is not representable is refused as an overflow.
  SpectrumRequest far = env.request(AllocationRequestId(2), kStart, kLease);
  far.requestedAt = Instant::max() - Duration::days(2);
  const AllocationDecision last = runtime.allocate(far);
  WF_REQUIRE(last.allocated());
  const Status overflow = runtime.renew(last.reservation, ReservationGeneration(1), tenYears,
                                        env.fixture.reservation(), far.requestedAt, 0);
  WF_CHECK_EQ(std::string(toToken(overflow.code)), std::string("overflow"));
  const std::optional<SpectrumReservation> stored = env.stored(last.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->lease.expiresAt == far.requestedAt + kLease);
  WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{0});
}

WF_TEST(a_lapsed_lease_cannot_be_renewed_and_nothing_mutates) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  const std::optional<SpectrumReservation> before = env.stored(decision.reservation);
  WF_REQUIRE(before.has_value());
  const std::size_t auditsBefore = runtime.auditSize();

  for (const Instant lapsed : {kStart + kLease, kStart + kLease + Duration::seconds(1)}) {
    const Status status =
        runtime.renew(decision.reservation, ReservationGeneration(1), Duration::minutes(1),
                      env.fixture.reservation(), lapsed, 0);
    WF_CHECK_EQ(std::string(toToken(status.code)), std::string("refused"));
    WF_CHECK(status.message.find("lapsed") != std::string::npos);
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK(stored->lease.expiresAt == before->lease.expiresAt);
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{0});
    WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});
    WF_CHECK(stored->updatedAt == before->updatedAt);
    WF_CHECK(stored->state == ReservationState::Reserved);
  }
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{0});
  WF_CHECK_EQ(runtime.stats().replayRejections, std::uint64_t{2});
  WF_CHECK_EQ(runtime.auditSize(), auditsBefore + 2);
  const std::optional<AuditRecord> last = env.lastAudit();
  WF_REQUIRE(last.has_value());
  WF_CHECK(last->kind == AuditKind::ReplayRejected);
  WF_CHECK(last->outcome == AllocationOutcome::RefusedLeaseExpired);
  WF_CHECK(last->at == kStart + kLease + Duration::seconds(1));
}

WF_TEST(a_renewal_is_refused_once_the_reservation_stopped_owning_spectrum) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());

  const ReclaimReport expired = runtime.expireLeases(kStart + kLease);
  WF_CHECK_EQ(expired.expired, std::size_t{1});
  const Status onExpired =
      runtime.renew(decision.reservation, ReservationGeneration(1), Duration::minutes(1),
                    env.fixture.reservation(), kStart + kLease, 0);
  WF_CHECK_EQ(std::string(toToken(onExpired.code)), std::string("illegal-transition"));
  WF_CHECK(onExpired.message.find("expired") != std::string::npos);

  const ReclaimReport reclaimed = runtime.reclaimExpired(kStart + kLease);
  WF_CHECK_EQ(reclaimed.reclaimed.size(), std::size_t{1});
  const Status onReclaimed =
      runtime.renew(decision.reservation, ReservationGeneration(1), Duration::minutes(1),
                    env.fixture.reservation(), kStart + kLease, 0);
  WF_CHECK_EQ(std::string(toToken(onReclaimed.code)), std::string("illegal-transition"));
  WF_CHECK(onReclaimed.message.find("reclaimed") != std::string::npos);

  const Status missing = runtime.renew(ReservationId(99), ReservationGeneration(1),
                                       Duration::minutes(1), env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(missing.code)), std::string("not-found"));
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{0});
  const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->state == ReservationState::Reclaimed);
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});
}

WF_TEST(the_lease_starts_at_the_later_of_requested_at_and_not_before) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  SpectrumRequest delayed = env.request(AllocationRequestId(1), kStart, kLease);
  delayed.notBefore = kStart + Duration::minutes(7);
  const AllocationDecision decision = runtime.allocate(delayed);
  WF_REQUIRE(decision.allocated());
  const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->lease.grantedAt == kStart + Duration::minutes(7));
  WF_CHECK(stored->lease.expiresAt == kStart + Duration::minutes(7) + kLease);
  WF_CHECK(stored->createdAt == kStart + Duration::minutes(7));
  WF_CHECK(stored->updatedAt == kStart);

  // Conflict detection runs at requestedAt, not at the lease start, so a request
  // placed at the same instant still sees the reservation that starts later.
  const AllocationDecision overlapping = env.allocate(AllocationRequestId(2), kStart, kLease);
  WF_REQUIRE(overlapping.allocated());
  const std::optional<SpectrumReservation> second = env.stored(overlapping.reservation);
  WF_REQUIRE(second.has_value());
  WF_CHECK(isRange(second->slots, 1, 1));

  // A notBefore in the past is ignored: the lease starts at requestedAt.
  SpectrumRequest early = env.request(AllocationRequestId(3), kStart, kLease);
  early.notBefore = kStart - Duration::hours(3);
  const AllocationDecision immediate = runtime.allocate(early);
  WF_REQUIRE(immediate.allocated());
  const std::optional<SpectrumReservation> third = env.stored(immediate.reservation);
  WF_REQUIRE(third.has_value());
  WF_CHECK(third->lease.grantedAt == kStart);
  WF_CHECK(third->lease.expiresAt == kStart + kLease);
}

WF_TEST(a_renewal_requires_the_current_reservation_authority_generation) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  env.fixture.advanceTo(2);

  ReservationAuthority stale;
  stale.generation = ReservationAuthorityGeneration(1);
  stale.fence = runtime.fence();
  const Status refused = runtime.renew(decision.reservation, ReservationGeneration(1),
                                       Duration::minutes(1), stale, kStart, 0);
  WF_CHECK_EQ(std::string(toToken(refused.code)), std::string("stale-generation"));
  WF_CHECK(refused.message.find("reservation authority") != std::string::npos);
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{0});
  {
    const std::optional<SpectrumReservation> stored = env.stored(decision.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});
    WF_CHECK(stored->lease.expiresAt == kStart + kLease);
    WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{0});
  }
  const std::optional<AuditRecord> refusal = env.lastAudit();
  WF_REQUIRE(refusal.has_value());
  WF_CHECK(refusal->kind == AuditKind::ReplayRejected);
  WF_CHECK(refusal->outcome == AllocationOutcome::RefusedStaleAuthority);

  // The current generation is accepted.
  WF_CHECK(runtime
               .renew(decision.reservation, ReservationGeneration(1), Duration::minutes(1),
                      env.fixture.reservation(), kStart, 0)
               .ok());
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{1});

  // A generation ahead of the runtime is stale too: the token must match exactly.
  ReservationAuthority ahead;
  ahead.generation = ReservationAuthorityGeneration(3);
  ahead.fence = runtime.fence();
  const Status future = runtime.renew(decision.reservation, ReservationGeneration(2),
                                      Duration::minutes(1), ahead, kStart, 0);
  WF_CHECK_EQ(std::string(toToken(future.code)), std::string("stale-generation"));

  // An absent token is invalid, never treated as an implicit authority.
  const Status absent = runtime.renew(decision.reservation, ReservationGeneration(2),
                                      Duration::minutes(1), ReservationAuthority{}, kStart, 0);
  WF_CHECK_EQ(std::string(toToken(absent.code)), std::string("invalid-argument"));
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{1});
}

}  // namespace

WF_TEST_MAIN()
