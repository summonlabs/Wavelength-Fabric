// Idempotent replay and supersede.
//
// Replaying a committed (requestId, requestGeneration) identity is a success that
// returns the reservation that already exists: no second reservation, no second
// commit statistic and no second commit audit record. The same identity with a
// different shape is a duplicate refusal, and a newer request generation
// supersedes the previous live reservation exactly once.

#include "test_common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace wavelength_fabric;

const Instant kStart = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::minutes(10);

[[nodiscard]] bool sameReservation(const SpectrumReservation& left,
                                   const SpectrumReservation& right) {
  return left.id == right.id && left.generation == right.generation &&
         left.requestId == right.requestId &&
         left.requestGeneration == right.requestGeneration && left.owner == right.owner &&
         left.ownerGeneration == right.ownerGeneration && left.state == right.state &&
         left.domains == right.domains &&
         left.domainGenerations == right.domainGenerations &&
         left.anchorDomain == right.anchorDomain && left.grid == right.grid &&
         left.gridGeneration == right.gridGeneration && left.slots == right.slots &&
         left.frequency == right.frequency && left.perDomainSlots == right.perDomainSlots &&
         left.crossGrid == right.crossGrid &&
         left.contiguityRequired == right.contiguityRequired &&
         left.continuityRequired == right.continuityRequired &&
         left.guardBandMhz == right.guardBandMhz &&
         left.exclusionDomain == right.exclusionDomain &&
         left.lease.generation == right.lease.generation &&
         left.lease.grantedAt == right.lease.grantedAt &&
         left.lease.expiresAt == right.lease.expiresAt &&
         left.lease.renewalCount == right.lease.renewalCount &&
         left.lease.maxRenewals == right.lease.maxRenewals &&
         left.createdAt == right.createdAt && left.updatedAt == right.updatedAt &&
         left.activatedAt == right.activatedAt && left.deactivatedAt == right.deactivatedAt &&
         left.releasedAt == right.releasedAt && left.reclaimedAt == right.reclaimedAt &&
         left.capabilityGeneration == right.capabilityGeneration &&
         left.commitFence == right.commitFence &&
         left.lastOperation == right.lastOperation &&
         left.needsRevalidation == right.needsRevalidation && left.detail == right.detail;
}

class Env {
 public:
  Env() {
    (void)fixture.runtime.registerGrid(wf_test::fixedGrid(grid_, GridGeneration(1)));
    (void)fixture.addDomain(domain_, grid_, 96);
  }

  [[nodiscard]] SpectrumRuntime& runtime() { return fixture.runtime; }
  [[nodiscard]] SpectrumDomainId domain() const { return domain_; }

  [[nodiscard]] SpectrumRequest request(AllocationRequestId id, Instant at, Duration lease) const {
    return wf_test::makeRequest(id, OwnerId(7), {domain_}, {SpectrumDomainGeneration(1)}, grid_,
                                GridGeneration(1), 1, at, lease, fixture.eligibility(),
                                fixture.reservation());
  }

  [[nodiscard]] AllocationDecision allocate(const SpectrumRequest& request) {
    return fixture.runtime.allocate(request);
  }

  [[nodiscard]] AllocationDecision allocate(AllocationRequestId id, Instant at, Duration lease) {
    return fixture.runtime.allocate(request(id, at, lease));
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

WF_TEST(an_identical_replay_returns_the_committed_reservation) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const SpectrumRequest original = env.request(AllocationRequestId(1), kStart, kLease);
  const AllocationDecision committed = env.allocate(original);
  WF_REQUIRE(committed.allocated());
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationCommitted), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().candidatesEvaluated, std::uint64_t{96});

  const AllocationDecision replay = env.allocate(original);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == committed.reservation);
  WF_CHECK_EQ(replay.generation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(replay.explanation.reservation.raw(), committed.reservation.raw());
  WF_CHECK_EQ(replay.explanation.selected.slots.first, std::uint32_t{0});
  WF_CHECK_EQ(replay.explanation.selected.slots.count, std::uint32_t{1});
  WF_CHECK(replay.explanation.conflicts.empty());
  WF_REQUIRE(!replay.explanation.reasons.empty());
  WF_CHECK(replay.explanation.reasons[0].find("idempotent replay") != std::string::npos);

  // No second reservation, no second commit statistic, no duplicate commit audit.
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{0});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationCommitted), std::size_t{1});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::RequestEvaluated), std::size_t{1});
  // A replay reports one evaluated placement per spanned domain; it does not
  // re-enumerate the grid.
  WF_CHECK_EQ(runtime.stats().candidatesEvaluated, std::uint64_t{97});
  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{95});
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{1});

  const std::optional<AuditRecord> audit = env.lastAudit();
  WF_REQUIRE(audit.has_value());
  WF_CHECK(audit->kind == AuditKind::RequestEvaluated);
  WF_CHECK(audit->outcome == AllocationOutcome::Allocated);
  WF_CHECK(audit->reservation == committed.reservation);
  WF_CHECK(audit->at == kStart);

  // Replaying again is still the same answer.
  const AllocationDecision third = env.allocate(original);
  WF_CHECK(third.allocated());
  WF_CHECK(third.reservation == committed.reservation);
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationCommitted), std::size_t{1});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::RequestEvaluated), std::size_t{2});
}

WF_TEST(a_replay_reports_the_generation_the_reservation_currently_carries) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const SpectrumRequest original = env.request(AllocationRequestId(1), kStart, kLease);
  const AllocationDecision committed = env.allocate(original);
  WF_REQUIRE(committed.allocated());

  WF_CHECK(runtime
               .renew(committed.reservation, ReservationGeneration(1), Duration::minutes(1),
                      env.fixture.reservation(), kStart, 0)
               .ok());
  WF_CHECK(runtime
               .activate(committed.reservation, ReservationGeneration(2),
                         env.fixture.activation(), kStart)
               .ok());

  const AllocationDecision replay = env.allocate(original);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == committed.reservation);
  WF_CHECK_EQ(replay.generation.raw(), std::uint64_t{2});
  const std::optional<SpectrumReservation> stored = env.stored(committed.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->state == ReservationState::Active);
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{2});
  WF_CHECK_EQ(stored->lease.renewalCount, std::uint32_t{1});
  WF_CHECK(stored->lease.expiresAt == kStart + kLease + Duration::minutes(1));
  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->activeReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->activeSlots, std::uint32_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().activations, std::uint64_t{1});
}

WF_TEST(a_replay_of_a_released_reservation_reports_its_terminal_state) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const std::optional<SpectrumUsage> baseline = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(baseline.has_value());

  const SpectrumRequest original = env.request(AllocationRequestId(1), kStart, kLease);
  const AllocationDecision committed = env.allocate(original);
  WF_REQUIRE(committed.allocated());
  const Instant releasedAt = kStart + Duration::minutes(1);
  WF_CHECK(runtime
               .release(committed.reservation, ReservationGeneration(1), env.fixture.release(),
                        releasedAt)
               .ok());

  const AllocationDecision replay = env.allocate(original);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == committed.reservation);
  WF_CHECK_EQ(replay.generation.raw(), std::uint64_t{1});
  const std::optional<SpectrumReservation> stored = env.stored(committed.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->state == ReservationState::Released);
  WF_CHECK(stored->releasedAt == releasedAt);
  WF_CHECK(stored->isLiveAt(kStart) == false);

  // The terminal reservation owns nothing and the replay did not revive it.
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().releases, std::uint64_t{1});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationCommitted), std::size_t{1});
  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->freeSlots, baseline->freeSlots);
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{0});
  WF_CHECK_EQ(usage->releasedReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{0});
  WF_CHECK_EQ(usage->freeRuns.size(), baseline->freeRuns.size());
  WF_CHECK(usage->freeRuns[0] == baseline->freeRuns[0]);

  // The released spectrum is really available again.
  const AllocationDecision next = env.allocate(AllocationRequestId(2), kStart, kLease);
  WF_REQUIRE(next.allocated());
  WF_CHECK(!(next.reservation == committed.reservation));
  const std::optional<SpectrumReservation> second = env.stored(next.reservation);
  WF_REQUIRE(second.has_value());
  WF_CHECK(second->slots.first == 0 && second->slots.count == 1);
}

WF_TEST(a_replay_of_a_reclaimed_reservation_reports_its_terminal_state) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const SpectrumRequest original = env.request(AllocationRequestId(1), kStart, kLease);
  const AllocationDecision committed = env.allocate(original);
  WF_REQUIRE(committed.allocated());
  const ReclaimReport report = runtime.reclaimExpired(kStart + kLease);
  WF_CHECK_EQ(report.reclaimed.size(), std::size_t{1});

  const AllocationDecision replay = env.allocate(original);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == committed.reservation);
  const std::optional<SpectrumReservation> stored = env.stored(committed.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->state == ReservationState::Reclaimed);
  WF_CHECK(stored->reclaimedAt == kStart + kLease);
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{1});
  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart + kLease);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{96});
  WF_CHECK_EQ(usage->reclaimedReservations, std::uint32_t{1});
}

WF_TEST(the_same_identity_with_a_different_shape_is_a_duplicate_refusal) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const SpectrumRequest original = env.request(AllocationRequestId(1), kStart, kLease);
  const AllocationDecision committed = env.allocate(original);
  WF_REQUIRE(committed.allocated());
  const std::optional<SpectrumReservation> before = env.stored(committed.reservation);
  WF_REQUIRE(before.has_value());

  std::vector<SpectrumRequest> different;
  {
    SpectrumRequest changed = original;
    changed.owner = OwnerId(8);
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.ownerGeneration = OwnerGeneration(2);
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.gridGeneration = GridGeneration(2);
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.guardBandMhz = 12'500;
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.maxRenewals = 4;
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.contiguity = ContiguityRequirement::Required;
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
    changed.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
    changed.continuity = ContinuityRequirement::Required;
    different.push_back(changed);
  }
  {
    SpectrumRequest changed = original;
    changed.slots = 2;
    changed.contiguity = ContiguityRequirement::Required;
    different.push_back(changed);
  }

  for (const SpectrumRequest& request : different) {
    const AllocationDecision duplicate = runtime.allocate(request);
    WF_CHECK(!duplicate.allocated());
    WF_CHECK(duplicate.outcome == AllocationOutcome::RefusedDuplicate);
    WF_CHECK_EQ(std::string(toToken(duplicate.status.code)), std::string("duplicate"));
    WF_CHECK(duplicate.reservation.none());
  }
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{different.size()});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationCommitted), std::size_t{1});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationRefused), different.size());
  const std::optional<SpectrumReservation> after = env.stored(committed.reservation);
  WF_REQUIRE(after.has_value());
  WF_CHECK(sameReservation(*before, *after));

  // The shape comparison deliberately ignores time: replaying the same identity
  // with another instant and another lease duration still returns the committed
  // reservation, whose lease is untouched.
  SpectrumRequest shifted = original;
  shifted.requestedAt = kStart + Duration::hours(3);
  shifted.notBefore = kStart + Duration::hours(3);
  shifted.leaseDuration = Duration::hours(1);
  const AllocationDecision replay = runtime.allocate(shifted);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == committed.reservation);
  const std::optional<SpectrumReservation> unchanged = env.stored(committed.reservation);
  WF_REQUIRE(unchanged.has_value());
  WF_CHECK(unchanged->lease.grantedAt == kStart);
  WF_CHECK(unchanged->lease.expiresAt == kStart + kLease);
  WF_CHECK(unchanged->createdAt == kStart);
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});

  // A different identity with the same shape is an ordinary allocation.
  SpectrumRequest other = env.request(AllocationRequestId(2), kStart, kLease);
  const AllocationDecision second = runtime.allocate(other);
  WF_REQUIRE(second.allocated());
  WF_CHECK(!(second.reservation == committed.reservation));
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{2});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});
}

WF_TEST(a_newer_request_generation_supersedes_the_live_reservation_exactly_once) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  SpectrumRequest first = env.request(AllocationRequestId(1), kStart, kLease);
  first.requestGeneration = AllocationRequestGeneration(1);
  const AllocationDecision firstDecision = env.allocate(first);
  WF_REQUIRE(firstDecision.allocated());
  const std::optional<SpectrumReservation> original = env.stored(firstDecision.reservation);
  WF_REQUIRE(original.has_value());
  WF_CHECK(original->slots.first == 0 && original->slots.count == 1);

  // The newer generation is evaluated while the previous reservation is still
  // live, so it lands on the next free slot; superseding then returns the
  // previous slot to the free pool.
  SpectrumRequest second = env.request(AllocationRequestId(1), kStart, kLease);
  second.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision secondDecision = env.allocate(second);
  WF_REQUIRE(secondDecision.allocated());
  WF_CHECK(!(secondDecision.reservation == firstDecision.reservation));
  WF_CHECK_EQ(secondDecision.explanation.reservation.raw(), secondDecision.reservation.raw());
  const std::optional<SpectrumReservation> replacement = env.stored(secondDecision.reservation);
  WF_REQUIRE(replacement.has_value());
  WF_CHECK(replacement->slots.first == 1 && replacement->slots.count == 1);
  WF_CHECK_EQ(replacement->requestGeneration.raw(), std::uint64_t{2});
  WF_CHECK_EQ(replacement->generation.raw(), std::uint64_t{1});

  const std::optional<SpectrumReservation> superseded = env.stored(firstDecision.reservation);
  WF_REQUIRE(superseded.has_value());
  WF_CHECK(superseded->state == ReservationState::Superseded);
  WF_CHECK(superseded->updatedAt == kStart);
  WF_CHECK(superseded->isLiveAt(kStart) == false);
  WF_CHECK_EQ(superseded->generation.raw(), std::uint64_t{1});
  WF_CHECK(superseded->lease.expiresAt == kStart + kLease);
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});

  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{95});
  WF_REQUIRE(usage->freeRuns.size() == std::size_t{2});
  WF_CHECK(usage->freeRuns[0].first == 0 && usage->freeRuns[0].count == 1);
  WF_CHECK(usage->freeRuns[1].first == 2 && usage->freeRuns[1].count == 94);
  WF_CHECK_EQ(runtime.reservationsForDomain(env.domain()).size(), std::size_t{2});

  // The superseded reservation owns no spectrum: every ownership operation on it
  // is an illegal transition, not a silent success.
  const Status release =
      runtime.release(firstDecision.reservation, ReservationGeneration(1), env.fixture.release(),
                      kStart);
  WF_CHECK_EQ(std::string(toToken(release.code)), std::string("illegal-transition"));
  const Status renew = runtime.renew(firstDecision.reservation, ReservationGeneration(1),
                                     Duration::minutes(1), env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(std::string(toToken(renew.code)), std::string("illegal-transition"));

  // Replaying generation 2 is idempotent: the supersede happened exactly once.
  const AllocationDecision replay = env.allocate(second);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == secondDecision.reservation);
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{2});

  // A third generation supersedes the second, and only the second.
  SpectrumRequest third = env.request(AllocationRequestId(1), kStart, kLease);
  third.requestGeneration = AllocationRequestGeneration(3);
  const AllocationDecision thirdDecision = env.allocate(third);
  WF_REQUIRE(thirdDecision.allocated());
  WF_CHECK(!(thirdDecision.reservation == secondDecision.reservation));
  const std::optional<SpectrumReservation> nowSuperseded = env.stored(secondDecision.reservation);
  WF_REQUIRE(nowSuperseded.has_value());
  WF_CHECK(nowSuperseded->state == ReservationState::Superseded);
  const std::optional<SpectrumReservation> stillSuperseded = env.stored(firstDecision.reservation);
  WF_REQUIRE(stillSuperseded.has_value());
  WF_CHECK(stillSuperseded->state == ReservationState::Superseded);
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{2});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{3});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{0});
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{3});
  const std::optional<SpectrumUsage> after = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(after.has_value());
  WF_CHECK_EQ(after->liveReservations, std::uint32_t{1});
  WF_CHECK_EQ(after->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(after->freeSlots, std::uint32_t{95});
}

WF_TEST(superseding_an_expired_but_unreclaimed_reservation_still_returns_its_spectrum) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  SpectrumRequest first = env.request(AllocationRequestId(1), kStart, kLease);
  first.requestGeneration = AllocationRequestGeneration(1);
  const AllocationDecision firstDecision = env.allocate(first);
  WF_REQUIRE(firstDecision.allocated());

  // At an instant after the lease lapsed the old reservation blocks nothing, so
  // the newer generation is placed on the same slot - and the old reservation is
  // still superseded rather than left owning spectrum.
  SpectrumRequest second = env.request(AllocationRequestId(1), kStart + kLease, kLease);
  second.requestGeneration = AllocationRequestGeneration(2);
  const AllocationDecision secondDecision = env.allocate(second);
  WF_REQUIRE(secondDecision.allocated());
  const std::optional<SpectrumReservation> replacement = env.stored(secondDecision.reservation);
  WF_REQUIRE(replacement.has_value());
  WF_CHECK(replacement->slots.first == 0 && replacement->slots.count == 1);
  const std::optional<SpectrumReservation> superseded = env.stored(firstDecision.reservation);
  WF_REQUIRE(superseded.has_value());
  WF_CHECK(superseded->state == ReservationState::Superseded);
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});
  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart + kLease);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->liveReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->liveSlots, std::uint32_t{1});
  WF_CHECK_EQ(usage->lapsedSlots, std::uint32_t{0});
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{95});
  WF_CHECK_EQ(runtime.stats().reclamations, std::uint64_t{0});
  WF_CHECK_EQ(runtime.stats().expirationSweeps, std::uint64_t{0});
}

WF_TEST(a_duplicate_after_a_supersede_is_judged_against_the_current_generation) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  SpectrumRequest first = env.request(AllocationRequestId(1), kStart, kLease);
  first.requestGeneration = AllocationRequestGeneration(1);
  WF_REQUIRE(env.allocate(first).allocated());
  SpectrumRequest second = env.request(AllocationRequestId(1), kStart, kLease);
  second.requestGeneration = AllocationRequestGeneration(2);
  WF_REQUIRE(env.allocate(second).allocated());

  // Same identity, same current generation, different shape: duplicate.
  SpectrumRequest duplicate = second;
  duplicate.owner = OwnerId(9);
  const AllocationDecision refused = env.allocate(duplicate);
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedDuplicate);
  WF_CHECK_EQ(std::string(toToken(refused.status.code)), std::string("duplicate"));

  // Same identity, older generation, different shape: stale wins, because the
  // request is not even allowed to speak about the superseded generation.
  SpectrumRequest stale = first;
  stale.owner = OwnerId(9);
  const AllocationDecision staleDecision = env.allocate(stale);
  WF_CHECK(staleDecision.outcome == AllocationOutcome::RefusedStaleGeneration);

  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{2});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{2});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});
}

}  // namespace

WF_TEST_MAIN()
