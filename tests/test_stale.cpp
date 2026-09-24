// Stale authority and stale generations.
//
// A token that names a fence the runtime does not currently hold, or a
// generation that is not the current one, must be refused with a typed stale
// outcome - and a refused path must leave ownership, usage and statistics
// exactly as they were.

#include "test_common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace wavelength_fabric;

const Instant kStart = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::minutes(10);

[[nodiscard]] EligibilityAuthority eligibilityAt(const ControllerFence& fence,
                                                 std::uint64_t generation) {
  EligibilityAuthority token;
  token.generation = EligibilityAuthorityGeneration(generation);
  token.fence = fence;
  return token;
}

[[nodiscard]] ReservationAuthority reservationAt(const ControllerFence& fence,
                                                 std::uint64_t generation) {
  ReservationAuthority token;
  token.generation = ReservationAuthorityGeneration(generation);
  token.fence = fence;
  return token;
}

[[nodiscard]] ActivationAuthority activationAt(const ControllerFence& fence,
                                               std::uint64_t generation) {
  ActivationAuthority token;
  token.generation = ActivationAuthorityGeneration(generation);
  token.fence = fence;
  return token;
}

[[nodiscard]] ReleaseAuthority releaseAt(const ControllerFence& fence, std::uint64_t generation) {
  ReleaseAuthority token;
  token.generation = ReleaseAuthorityGeneration(generation);
  token.fence = fence;
  return token;
}

[[nodiscard]] std::string token(StatusCode code) { return std::string(toToken(code)); }

// None of the runtime's records define operator==, and these cases need to prove
// that a stale path left every observable field untouched, so each record is
// compared field by field.
[[nodiscard]] bool sameLease(const Lease& left, const Lease& right) {
  return left.generation == right.generation && left.grantedAt == right.grantedAt &&
         left.expiresAt == right.expiresAt && left.renewalCount == right.renewalCount &&
         left.maxRenewals == right.maxRenewals;
}

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
         left.exclusionDomain == right.exclusionDomain && sameLease(left.lease, right.lease) &&
         left.createdAt == right.createdAt && left.updatedAt == right.updatedAt &&
         left.activatedAt == right.activatedAt && left.deactivatedAt == right.deactivatedAt &&
         left.releasedAt == right.releasedAt && left.reclaimedAt == right.reclaimedAt &&
         left.capabilityGeneration == right.capabilityGeneration &&
         left.commitFence == right.commitFence &&
         left.lastOperation == right.lastOperation &&
         left.needsRevalidation == right.needsRevalidation && left.detail == right.detail;
}

[[nodiscard]] bool sameUsage(const SpectrumUsage& left, const SpectrumUsage& right) {
  return left.domain == right.domain && left.domainGeneration == right.domainGeneration &&
         left.grid == right.grid && left.gridGeneration == right.gridGeneration &&
         left.totalSlots == right.totalSlots &&
         left.allocatableSlots == right.allocatableSlots && left.liveSlots == right.liveSlots &&
         left.activeSlots == right.activeSlots && left.reservedSlots == right.reservedSlots &&
         left.freeSlots == right.freeSlots && left.lapsedSlots == right.lapsedSlots &&
         left.liveReservations == right.liveReservations &&
         left.activeReservations == right.activeReservations &&
         left.lapsedReservations == right.lapsedReservations &&
         left.reclaimedReservations == right.reclaimedReservations &&
         left.releasedReservations == right.releasedReservations &&
         left.freeRuns == right.freeRuns;
}

[[nodiscard]] bool sameStats(const RuntimeStats& left, const RuntimeStats& right) {
  return left.allocationsCommitted == right.allocationsCommitted &&
         left.allocationsRefused == right.allocationsRefused &&
         left.candidatesEvaluated == right.candidatesEvaluated &&
         left.enumerationTruncations == right.enumerationTruncations &&
         left.renewals == right.renewals && left.releases == right.releases &&
         left.activations == right.activations && left.deactivations == right.deactivations &&
         left.expirationSweeps == right.expirationSweeps &&
         left.reclamations == right.reclamations &&
         left.persistenceWrites == right.persistenceWrites &&
         left.persistenceFailures == right.persistenceFailures &&
         left.replayRejections == right.replayRejections &&
         left.corruptionDetections == right.corruptionDetections;
}

[[nodiscard]] bool sameReservations(const std::vector<SpectrumReservation>& left,
                                    const std::vector<SpectrumReservation>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!sameReservation(left[index], right[index])) return false;
  }
  return true;
}

class Env {
 public:
  Env() {
    (void)fixture.runtime.registerGrid(wf_test::fixedGrid(grid_, GridGeneration(1)));
    (void)fixture.addDomain(domain_, grid_, 96);
  }

  [[nodiscard]] SpectrumRuntime& runtime() { return fixture.runtime; }
  [[nodiscard]] SpectrumDomainId domain() const { return domain_; }
  [[nodiscard]] ChannelGridId grid() const { return grid_; }

  [[nodiscard]] SpectrumRequest request(AllocationRequestId id, Instant at, Duration lease) const {
    return wf_test::makeRequest(id, OwnerId(7), {domain_}, {SpectrumDomainGeneration(1)}, grid_,
                                GridGeneration(1), 1, at, lease, fixture.eligibility(),
                                fixture.reservation());
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

  [[nodiscard]] std::size_t refusalsWithOutcome(AllocationOutcome outcome) const {
    std::size_t count = 0;
    for (const AuditRecord& record : fixture.runtime.audit(AuditSequence(0), 0)) {
      if (record.outcome == outcome) ++count;
    }
    return count;
  }

  wf_test::Fixture fixture;

 private:
  SpectrumDomainId domain_{SpectrumDomainId(1)};
  ChannelGridId grid_{ChannelGridId(1)};
};

WF_TEST(allocation_refuses_tokens_minted_under_a_foreign_fence) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const AllocationDecision baseline = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(baseline.allocated());
  const ControllerFence current = runtime.fence();

  // A second runtime boot has its own incarnation; its tokens are stale here.
  RuntimeConfig otherConfig;
  SpectrumRuntime otherRuntime(otherConfig);
  WF_CHECK(!(otherRuntime.fence() == current));

  SpectrumRequest foreignEpoch = env.request(AllocationRequestId(2), kStart, kLease);
  foreignEpoch.eligibilityAuthority.fence.epoch = ControllerEpoch(current.epoch.raw() + 1);
  const AllocationDecision epochDecision = runtime.allocate(foreignEpoch);
  WF_CHECK(!epochDecision.allocated());
  WF_CHECK(epochDecision.outcome == AllocationOutcome::RefusedStaleEpoch);
  WF_CHECK_EQ(token(epochDecision.status.code), std::string("stale-epoch"));

  SpectrumRequest foreignIncarnation = env.request(AllocationRequestId(3), kStart, kLease);
  foreignIncarnation.eligibilityAuthority.fence.incarnation =
      ControllerIncarnation(current.incarnation.raw() + 1);
  const AllocationDecision incarnationDecision = runtime.allocate(foreignIncarnation);
  WF_CHECK(!incarnationDecision.allocated());
  WF_CHECK(incarnationDecision.outcome == AllocationOutcome::RefusedStaleIncarnation);
  WF_CHECK_EQ(token(incarnationDecision.status.code), std::string("stale-incarnation"));

  // The epoch is compared before the incarnation.
  SpectrumRequest both = env.request(AllocationRequestId(4), kStart, kLease);
  both.eligibilityAuthority.fence.epoch = ControllerEpoch(current.epoch.raw() + 1);
  both.eligibilityAuthority.fence.incarnation = ControllerIncarnation(current.incarnation.raw() + 1);
  const AllocationDecision bothDecision = runtime.allocate(both);
  WF_CHECK(bothDecision.outcome == AllocationOutcome::RefusedStaleEpoch);

  // The reservation authority token is fenced independently of the eligibility
  // token, and is checked after it.
  SpectrumRequest foreignReservation = env.request(AllocationRequestId(5), kStart, kLease);
  foreignReservation.reservationAuthority.fence.epoch = ControllerEpoch(current.epoch.raw() + 1);
  const AllocationDecision reservationDecision = runtime.allocate(foreignReservation);
  WF_CHECK(reservationDecision.outcome == AllocationOutcome::RefusedStaleEpoch);

  SpectrumRequest bootToken = env.request(AllocationRequestId(6), kStart, kLease);
  bootToken.eligibilityAuthority.fence = otherRuntime.fence();
  const AllocationDecision bootDecision = runtime.allocate(bootToken);
  WF_CHECK(bootDecision.outcome == AllocationOutcome::RefusedStaleIncarnation);
  WF_CHECK_EQ(token(bootDecision.status.code), std::string("stale-incarnation"));

  // Nothing was created, nothing was reclassified and the only counter that moved
  // is the refusal counter.
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{5});
  // The only enumeration that ran is the one the committed allocation performed.
  WF_CHECK_EQ(runtime.stats().candidatesEvaluated, std::uint64_t{96});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::AllocationRefused), std::size_t{5});
  WF_CHECK_EQ(env.refusalsWithOutcome(AllocationOutcome::RefusedStaleEpoch), std::size_t{3});
  WF_CHECK_EQ(env.refusalsWithOutcome(AllocationOutcome::RefusedStaleIncarnation), std::size_t{2});

  const std::optional<SpectrumUsage> usage = runtime.usage(env.domain(), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->freeSlots, std::uint32_t{95});
  WF_CHECK_EQ(usage->freeRuns.size(), std::size_t{1});
  WF_CHECK(usage->freeRuns[0].first == 1 && usage->freeRuns[0].count == 95);
  const std::optional<SpectrumReservation> original = env.stored(baseline.reservation);
  WF_REQUIRE(original.has_value());
  WF_CHECK(original->state == ReservationState::Reserved);
  WF_CHECK_EQ(original->generation.raw(), std::uint64_t{1});
  WF_CHECK(original->commitFence == current);
}

WF_TEST(lifecycle_operations_refuse_a_token_from_a_foreign_fence) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  const std::optional<SpectrumReservation> before = env.stored(decision.reservation);
  WF_REQUIRE(before.has_value());

  ControllerFence foreign = runtime.fence();
  foreign.incarnation = ControllerIncarnation(foreign.incarnation.raw() + 1);

  const Status renew = runtime.renew(decision.reservation, ReservationGeneration(1),
                                     Duration::minutes(1), reservationAt(foreign, 1), kStart, 0);
  WF_CHECK_EQ(token(renew.code), std::string("stale-incarnation"));
  const Status activate =
      runtime.activate(decision.reservation, ReservationGeneration(1), activationAt(foreign, 1), kStart);
  WF_CHECK_EQ(token(activate.code), std::string("stale-incarnation"));
  // The authority check runs before the state check, so a deactivation of a
  // reservation that is not even active is still reported as a stale token.
  const Status deactivate = runtime.deactivate(decision.reservation, ReservationGeneration(1),
                                               activationAt(foreign, 1), kStart);
  WF_CHECK_EQ(token(deactivate.code), std::string("stale-incarnation"));
  const Status release =
      runtime.release(decision.reservation, ReservationGeneration(1), releaseAt(foreign, 1), kStart);
  WF_CHECK_EQ(token(release.code), std::string("stale-incarnation"));

  const std::optional<SpectrumReservation> after = env.stored(decision.reservation);
  WF_REQUIRE(after.has_value());
  WF_CHECK(sameReservation(*before, *after));
  const RuntimeStats stats = runtime.stats();
  WF_CHECK_EQ(stats.renewals, std::uint64_t{0});
  WF_CHECK_EQ(stats.activations, std::uint64_t{0});
  WF_CHECK_EQ(stats.deactivations, std::uint64_t{0});
  WF_CHECK_EQ(stats.releases, std::uint64_t{0});
  WF_CHECK_EQ(stats.replayRejections, std::uint64_t{0});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReplayRejected), std::size_t{4});
  WF_CHECK_EQ(env.refusalsWithOutcome(AllocationOutcome::RefusedStaleAuthority), std::size_t{4});
}

WF_TEST(advancing_authority_fences_every_older_token_in_its_domain) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  WF_REQUIRE(env.allocate(AllocationRequestId(1), kStart, kLease).allocated());

  AuthorityState next;
  next.eligibilityGeneration = EligibilityAuthorityGeneration(2);
  next.reservationGeneration = ReservationAuthorityGeneration(2);
  next.activationGeneration = ActivationAuthorityGeneration(2);
  next.releaseGeneration = ReleaseAuthorityGeneration(2);
  next.fence = runtime.fence();
  WF_CHECK(runtime.advanceAuthority(next).ok());
  WF_CHECK_EQ(runtime.authorityState().reservationGeneration.raw(), std::uint64_t{2});

  const AllocationDecision stale = env.allocate(AllocationRequestId(2), kStart, kLease);
  WF_CHECK(!stale.allocated());
  WF_CHECK(stale.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK_EQ(token(stale.status.code), std::string("stale-generation"));
  WF_CHECK(stale.status.message.find("eligibility authority generation 1") != std::string::npos);

  // A generation ahead of the runtime is stale too: the token must match exactly.
  SpectrumRequest ahead = env.request(AllocationRequestId(3), kStart, kLease);
  ahead.eligibilityAuthority = eligibilityAt(runtime.fence(), 3);
  ahead.reservationAuthority = reservationAt(runtime.fence(), 3);
  const AllocationDecision aheadDecision = runtime.allocate(ahead);
  WF_CHECK(aheadDecision.outcome == AllocationOutcome::RefusedStaleGeneration);

  // A token that names one current generation and one stale generation is refused
  // because of the stale one.
  SpectrumRequest mixed = env.request(AllocationRequestId(4), kStart, kLease);
  mixed.eligibilityAuthority = eligibilityAt(runtime.fence(), 2);
  mixed.reservationAuthority = reservationAt(runtime.fence(), 1);
  const AllocationDecision mixedDecision = runtime.allocate(mixed);
  WF_CHECK(mixedDecision.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK(mixedDecision.status.message.find("reservation authority generation 1") !=
           std::string::npos);

  // The current generations are accepted.
  SpectrumRequest current = env.request(AllocationRequestId(5), kStart, kLease);
  current.eligibilityAuthority = eligibilityAt(runtime.fence(), 2);
  current.reservationAuthority = reservationAt(runtime.fence(), 2);
  WF_CHECK(runtime.allocate(current).allocated());

  // The authority counters never move backwards, never accept a no-op and never
  // accept a zero generation.
  AuthorityState backwards = runtime.authorityState();
  backwards.eligibilityGeneration = EligibilityAuthorityGeneration(1);
  WF_CHECK_EQ(token(runtime.advanceAuthority(backwards).code), std::string("stale-generation"));
  WF_CHECK_EQ(token(runtime.advanceAuthority(runtime.authorityState()).code),
              std::string("duplicate"));
  AuthorityState zeroed = runtime.authorityState();
  zeroed.releaseGeneration = ReleaseAuthorityGeneration(0);
  WF_CHECK_EQ(token(runtime.advanceAuthority(zeroed).code), std::string("invalid-argument"));

  // An advance carrying a stale fence changes nothing.
  AuthorityState foreign = runtime.authorityState();
  foreign.eligibilityGeneration = EligibilityAuthorityGeneration(9);
  foreign.fence.epoch = ControllerEpoch(runtime.fence().epoch.raw() + 1);
  WF_CHECK_EQ(token(runtime.advanceAuthority(foreign).code), std::string("stale-epoch"));
  WF_CHECK_EQ(runtime.authorityState().eligibilityGeneration.raw(), std::uint64_t{2});

  // A partial advance fences only the authority domain that moved.
  AuthorityState partial = runtime.authorityState();
  partial.eligibilityGeneration = EligibilityAuthorityGeneration(3);
  WF_CHECK(runtime.advanceAuthority(partial).ok());
  SpectrumRequest eligibilityStale = env.request(AllocationRequestId(6), kStart, kLease);
  eligibilityStale.eligibilityAuthority = eligibilityAt(runtime.fence(), 2);
  eligibilityStale.reservationAuthority = reservationAt(runtime.fence(), 2);
  const AllocationDecision eligibilityDecision = runtime.allocate(eligibilityStale);
  WF_CHECK(eligibilityDecision.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK(eligibilityDecision.status.message.find("eligibility authority generation 2") !=
           std::string::npos);
  SpectrumRequest stillValid = env.request(AllocationRequestId(7), kStart, kLease);
  stillValid.eligibilityAuthority = eligibilityAt(runtime.fence(), 3);
  stillValid.reservationAuthority = reservationAt(runtime.fence(), 2);
  WF_CHECK(runtime.allocate(stillValid).allocated());
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{3});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{4});
}

WF_TEST(operations_naming_an_older_reservation_generation_are_rejected) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();
  const AllocationDecision decision = env.allocate(AllocationRequestId(1), kStart, kLease);
  WF_REQUIRE(decision.allocated());
  const Duration extension = Duration::minutes(1);
  WF_CHECK(runtime
               .renew(decision.reservation, ReservationGeneration(1), extension,
                      env.fixture.reservation(), kStart, 0)
               .ok());
  const std::optional<SpectrumReservation> afterRenew = env.stored(decision.reservation);
  WF_REQUIRE(afterRenew.has_value());
  WF_CHECK_EQ(afterRenew->generation.raw(), std::uint64_t{2});
  const std::size_t auditsBefore = runtime.auditSize();

  const Status renew = runtime.renew(decision.reservation, ReservationGeneration(1), extension,
                                     env.fixture.reservation(), kStart, 0);
  WF_CHECK_EQ(token(renew.code), std::string("stale-generation"));
  WF_CHECK(renew.message.find("is at generation 2") != std::string::npos);
  const Status activate =
      runtime.activate(decision.reservation, ReservationGeneration(1), env.fixture.activation(), kStart);
  WF_CHECK_EQ(token(activate.code), std::string("stale-generation"));
  const Status deactivate = runtime.deactivate(decision.reservation, ReservationGeneration(1),
                                               env.fixture.activation(), kStart);
  WF_CHECK_EQ(token(deactivate.code), std::string("stale-generation"));
  const Status release =
      runtime.release(decision.reservation, ReservationGeneration(1), env.fixture.release(), kStart);
  WF_CHECK_EQ(token(release.code), std::string("stale-generation"));
  // A generation ahead of the reservation is not "newer authority", it is simply
  // not the current generation.
  const Status ahead = runtime.activate(decision.reservation, ReservationGeneration(3),
                                        env.fixture.activation(), kStart);
  WF_CHECK_EQ(token(ahead.code), std::string("stale-generation"));

  const std::optional<SpectrumReservation> afterRefusals = env.stored(decision.reservation);
  WF_REQUIRE(afterRefusals.has_value());
  WF_CHECK(sameReservation(*afterRenew, *afterRefusals));
  WF_CHECK_EQ(runtime.auditSize(), auditsBefore + 5);
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReplayRejected), std::size_t{5});
  WF_CHECK_EQ(env.refusalsWithOutcome(AllocationOutcome::RefusedStaleGeneration), std::size_t{5});
  const RuntimeStats refused = runtime.stats();
  WF_CHECK_EQ(refused.activations, std::uint64_t{0});
  WF_CHECK_EQ(refused.deactivations, std::uint64_t{0});
  WF_CHECK_EQ(refused.releases, std::uint64_t{0});
  WF_CHECK_EQ(refused.renewals, std::uint64_t{1});

  // The current generation still works, and activation does not advance it.
  WF_CHECK(runtime.activate(decision.reservation, ReservationGeneration(2), env.fixture.activation(), kStart).ok());
  WF_CHECK(runtime.deactivate(decision.reservation, ReservationGeneration(2), env.fixture.activation(), kStart).ok());
  WF_CHECK(runtime.release(decision.reservation, ReservationGeneration(2), env.fixture.release(), kStart).ok());
  const std::optional<SpectrumReservation> released = env.stored(decision.reservation);
  WF_REQUIRE(released.has_value());
  WF_CHECK(released->state == ReservationState::Released);
  WF_CHECK_EQ(released->generation.raw(), std::uint64_t{2});
  WF_CHECK(released->activatedAt == kStart);
  WF_CHECK(released->deactivatedAt == kStart);
  WF_CHECK(released->releasedAt == kStart);
  const RuntimeStats applied = runtime.stats();
  WF_CHECK_EQ(applied.activations, std::uint64_t{1});
  WF_CHECK_EQ(applied.deactivations, std::uint64_t{1});
  WF_CHECK_EQ(applied.releases, std::uint64_t{1});

  // Releasing twice is an illegal transition, not a stale one.
  const Status twice =
      runtime.release(decision.reservation, ReservationGeneration(2), env.fixture.release(), kStart);
  WF_CHECK_EQ(token(twice.code), std::string("illegal-transition"));
  WF_CHECK_EQ(runtime.stats().releases, std::uint64_t{1});
  const Status unknown = runtime.activate(ReservationId(99), ReservationGeneration(1),
                                          env.fixture.activation(), kStart);
  WF_CHECK_EQ(token(unknown.code), std::string("not-found"));
}

WF_TEST(a_request_naming_a_stale_grid_generation_is_refused) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  SpectrumRequest stale = env.request(AllocationRequestId(1), kStart, kLease);
  stale.gridGeneration = GridGeneration(2);
  const AllocationDecision refused = runtime.allocate(stale);
  WF_CHECK(!refused.allocated());
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK_EQ(token(refused.status.code), std::string("stale-generation"));
  WF_CHECK(refused.status.message.find("grid") != std::string::npos);
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{0});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{1});

  // After the grid is re-registered at a new generation both directions are
  // fenced: the old generation is stale and the new one is current.
  WF_CHECK(runtime.registerGrid(wf_test::fixedGrid(env.grid(), GridGeneration(2))).ok());
  SpectrumRequest oldGrid = env.request(AllocationRequestId(2), kStart, kLease);
  const AllocationDecision stillStale = runtime.allocate(oldGrid);
  WF_CHECK(stillStale.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK(stillStale.status.message.find("generation 2") != std::string::npos);
  SpectrumRequest newGrid = env.request(AllocationRequestId(3), kStart, kLease);
  newGrid.gridGeneration = GridGeneration(2);
  WF_CHECK(runtime.allocate(newGrid).allocated());
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{2});
}

WF_TEST(a_request_naming_a_stale_domain_generation_is_refused) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  SpectrumRequest stale = env.request(AllocationRequestId(1), kStart, kLease);
  stale.domainGenerations = {SpectrumDomainGeneration(2)};
  const AllocationDecision refused = runtime.allocate(stale);
  WF_CHECK(!refused.allocated());
  WF_CHECK(refused.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK_EQ(token(refused.status.code), std::string("stale-generation"));
  WF_CHECK(refused.status.message.find("domain") != std::string::npos);
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{0});

  // The domain is re-registered at generation 2 while its capability still
  // describes generation 1: the stale domain generation and the stale capability
  // stay separately observable.
  const SpectrumDomain upgraded =
      wf_test::makeDomain(env.domain(), env.grid(), SpectrumDomainGeneration(2), GridGeneration(1));
  WF_CHECK(runtime.registerDomain(upgraded).ok());
  SpectrumRequest oldDomain = env.request(AllocationRequestId(2), kStart, kLease);
  const AllocationDecision oldDomainDecision = runtime.allocate(oldDomain);
  WF_CHECK(oldDomainDecision.outcome == AllocationOutcome::RefusedStaleGeneration);
  WF_CHECK(oldDomainDecision.status.message.find("domain") != std::string::npos);
  SpectrumRequest newDomain = env.request(AllocationRequestId(3), kStart, kLease);
  newDomain.domainGenerations = {SpectrumDomainGeneration(2)};
  const AllocationDecision staleCapability = runtime.allocate(newDomain);
  WF_CHECK(staleCapability.outcome == AllocationOutcome::RefusedStaleCapability);
  WF_CHECK_EQ(token(staleCapability.status.code), std::string("stale-generation"));
  WF_CHECK(staleCapability.status.message.find("capability") != std::string::npos);

  // Publishing a capability for the current domain generation restores service.
  const SpectrumCapability capability =
      wf_test::makeCapability(env.domain(), env.grid(), SpectrumDomainGeneration(2),
                              GridGeneration(1), SpectrumSupport::Supported, 0, 96,
                              runtime.fence(), ControllerId(1), CapabilityGeneration(2));
  WF_CHECK(runtime.publishCapability(capability).ok());
  const AllocationDecision allocated = runtime.allocate(newDomain);
  WF_REQUIRE(allocated.allocated());
  const std::optional<SpectrumReservation> stored = env.stored(allocated.reservation);
  WF_REQUIRE(stored.has_value());
  WF_CHECK(stored->domainGenerations == std::vector<SpectrumDomainGeneration>{SpectrumDomainGeneration(2)});
  WF_CHECK_EQ(stored->capabilityGeneration.raw(), std::uint64_t{2});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{3});

  // A capability published under a foreign fence is refused and the registry is
  // left exactly as it was.
  SpectrumCapability foreign = wf_test::makeCapability(
      env.domain(), env.grid(), SpectrumDomainGeneration(2), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, runtime.fence(), ControllerId(1), CapabilityGeneration(3));
  foreign.fence.epoch = ControllerEpoch(runtime.fence().epoch.raw() + 1);
  WF_CHECK_EQ(token(runtime.publishCapability(foreign).code), std::string("stale-epoch"));
  const std::optional<SpectrumCapability> published = runtime.capability(env.domain());
  WF_REQUIRE(published.has_value());
  WF_CHECK_EQ(published->generation.raw(), std::uint64_t{2});
  // Republishing the same capability generation is a duplicate, not a refresh.
  WF_CHECK_EQ(token(runtime.publishCapability(capability).code), std::string("duplicate"));
  // An older capability generation is stale.
  const SpectrumCapability older =
      wf_test::makeCapability(env.domain(), env.grid(), SpectrumDomainGeneration(2),
                              GridGeneration(1), SpectrumSupport::Supported, 0, 96,
                              runtime.fence(), ControllerId(1), CapabilityGeneration(1));
  WF_CHECK_EQ(token(runtime.publishCapability(older).code), std::string("stale-generation"));
}

WF_TEST(unknown_domains_grids_and_capabilities_stay_distinguishable) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  SpectrumRequest unknownDomain = env.request(AllocationRequestId(1), kStart, kLease);
  unknownDomain.domains = {SpectrumDomainId(9)};
  unknownDomain.domainGenerations = {SpectrumDomainGeneration(1)};
  const AllocationDecision domainDecision = runtime.allocate(unknownDomain);
  WF_CHECK(!domainDecision.allocated());
  WF_CHECK(domainDecision.outcome == AllocationOutcome::RefusedUnknownDomain);
  WF_CHECK_EQ(token(domainDecision.status.code), std::string("not-found"));

  // An unregistered grid is an invalid request, not an unknown domain: the
  // request names a grid the runtime cannot even express the request in.
  SpectrumRequest unknownGrid = env.request(AllocationRequestId(2), kStart, kLease);
  unknownGrid.grid = ChannelGridId(9);
  const AllocationDecision gridDecision = runtime.allocate(unknownGrid);
  WF_CHECK(!gridDecision.allocated());
  WF_CHECK(gridDecision.outcome == AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK_EQ(token(gridDecision.status.code), std::string("invalid-argument"));
  WF_CHECK(gridDecision.status.message.find("grid") != std::string::npos);

  // A registered domain with no published capability is a distinct refusal from
  // an unknown domain.
  const SpectrumDomain bare =
      wf_test::makeDomain(SpectrumDomainId(2), env.grid(), SpectrumDomainGeneration(1),
                          GridGeneration(1));
  WF_CHECK(runtime.registerDomain(bare).ok());
  SpectrumRequest noCapability = env.request(AllocationRequestId(3), kStart, kLease);
  noCapability.domains = {SpectrumDomainId(2)};
  noCapability.domainGenerations = {SpectrumDomainGeneration(1)};
  const AllocationDecision capabilityDecision = runtime.allocate(noCapability);
  WF_CHECK(capabilityDecision.outcome == AllocationOutcome::RefusedUnknownCapability);
  WF_CHECK_EQ(token(capabilityDecision.status.code), std::string("unknown"));

  // A coexistence constraint that names a reservation the runtime never
  // committed is refused as a constraint, not silently ignored.
  SpectrumRequest badConstraint = env.request(AllocationRequestId(4), kStart, kLease);
  badConstraint.constraints.mustNotConflictWith = {ReservationId(77)};
  const AllocationDecision constraintDecision = runtime.allocate(badConstraint);
  WF_CHECK(!constraintDecision.allocated());
  WF_CHECK(constraintDecision.outcome == AllocationOutcome::RefusedConstraint);
  WF_CHECK_EQ(token(constraintDecision.status.code), std::string("refused"));
  WF_CHECK(constraintDecision.status.message.find("mustNotConflictWith") != std::string::npos);

  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{0});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK_EQ(runtime.stats().allocationsRefused, std::uint64_t{4});
}

WF_TEST(an_older_request_generation_is_refused_and_never_re_supersedes) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  SpectrumRequest first = env.request(AllocationRequestId(1), kStart, kLease);
  first.requestGeneration = AllocationRequestGeneration(1);
  const AllocationDecision firstDecision = runtime.allocate(first);
  WF_REQUIRE(firstDecision.allocated());

  SpectrumRequest third = env.request(AllocationRequestId(1), kStart, kLease);
  third.requestGeneration = AllocationRequestGeneration(3);
  const AllocationDecision thirdDecision = runtime.allocate(third);
  WF_REQUIRE(thirdDecision.allocated());
  WF_CHECK(!(thirdDecision.reservation == firstDecision.reservation));
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});

  const std::vector<SpectrumReservation> before = runtime.reservations();
  const RuntimeStats statsBefore = runtime.stats();

  for (const std::uint64_t staleGeneration : {std::uint64_t{1}, std::uint64_t{2}}) {
    SpectrumRequest replay = env.request(AllocationRequestId(1), kStart, kLease);
    replay.requestGeneration = AllocationRequestGeneration(staleGeneration);
    const AllocationDecision decision = runtime.allocate(replay);
    WF_CHECK(!decision.allocated());
    WF_CHECK(decision.outcome == AllocationOutcome::RefusedStaleGeneration);
    WF_CHECK_EQ(token(decision.status.code), std::string("stale-generation"));
    WF_CHECK(decision.status.message.find("is committed at generation 3") != std::string::npos);
    WF_CHECK_EQ(decision.reservation.raw(), std::uint64_t{0});
  }

  WF_CHECK(sameReservations(runtime.reservations(), before));
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});
  RuntimeStats expected = statsBefore;
  expected.allocationsRefused += 2;
  WF_CHECK(sameStats(runtime.stats(), expected));
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});
  WF_CHECK_EQ(runtime.reservations().size(), std::size_t{2});

  // The current generation is still an idempotent replay of the live reservation.
  SpectrumRequest current = env.request(AllocationRequestId(1), kStart, kLease);
  current.requestGeneration = AllocationRequestGeneration(3);
  const AllocationDecision replay = runtime.allocate(current);
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == thirdDecision.reservation);
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{2});
  WF_CHECK_EQ(env.auditsOfKind(AuditKind::ReservationSuperseded), std::size_t{1});
}

WF_TEST(no_stale_path_touches_ownership_usage_or_statistics) {
  Env env;
  SpectrumRuntime& runtime = env.runtime();

  SpectrumRequest live = env.request(AllocationRequestId(1), kStart, kLease);
  live.requestGeneration = AllocationRequestGeneration(3);
  const AllocationDecision committed = runtime.allocate(live);
  WF_REQUIRE(committed.allocated());
  WF_CHECK(runtime
               .activate(committed.reservation, ReservationGeneration(1),
                         env.fixture.activation(), kStart + Duration::minutes(1))
               .ok());

  const std::vector<SpectrumReservation> reservationsBefore = runtime.reservations();
  const std::optional<SpectrumUsage> usageBefore = runtime.usage(env.domain(), kStart);
  const std::optional<SpectrumUsage> afterExpiryBefore =
      runtime.usage(env.domain(), kStart + kLease);
  WF_REQUIRE(usageBefore.has_value());
  WF_REQUIRE(afterExpiryBefore.has_value());
  const RuntimeStats statsBefore = runtime.stats();

  // (a) an allocation token from a dead incarnation
  SpectrumRequest foreign = env.request(AllocationRequestId(2), kStart, kLease);
  foreign.eligibilityAuthority.fence.incarnation =
      ControllerIncarnation(runtime.fence().incarnation.raw() + 1);
  WF_CHECK(runtime.allocate(foreign).outcome == AllocationOutcome::RefusedStaleIncarnation);

  // (b) an allocation naming a stale domain generation
  SpectrumRequest staleDomain = env.request(AllocationRequestId(3), kStart, kLease);
  staleDomain.domainGenerations = {SpectrumDomainGeneration(4)};
  WF_CHECK(runtime.allocate(staleDomain).outcome == AllocationOutcome::RefusedStaleGeneration);

  // (c) an allocation replaying a request generation that was superseded
  SpectrumRequest staleRequest = env.request(AllocationRequestId(1), kStart, kLease);
  staleRequest.requestGeneration = AllocationRequestGeneration(2);
  WF_CHECK(runtime.allocate(staleRequest).outcome == AllocationOutcome::RefusedStaleGeneration);

  // (d) a renewal naming an older reservation generation
  WF_CHECK_EQ(token(runtime
                        .renew(committed.reservation, ReservationGeneration(9),
                               Duration::minutes(1), env.fixture.reservation(), kStart, 0)
                        .code),
              std::string("stale-generation"));

  // (e) an activation naming an older reservation generation
  WF_CHECK_EQ(token(runtime
                        .activate(committed.reservation, ReservationGeneration(9),
                                  env.fixture.activation(), kStart)
                        .code),
              std::string("stale-generation"));

  // (f) a release naming an older reservation generation
  WF_CHECK_EQ(token(runtime
                        .release(committed.reservation, ReservationGeneration(9),
                                 env.fixture.release(), kStart)
                        .code),
              std::string("stale-generation"));

  // (g) a renewal under a stale reservation authority generation
  WF_CHECK_EQ(token(runtime
                        .renew(committed.reservation, ReservationGeneration(1),
                               Duration::minutes(1), reservationAt(runtime.fence(), 9), kStart, 0)
                        .code),
              std::string("stale-generation"));

  // (h) an authority advance carrying a stale fence
  AuthorityState foreignAdvance = runtime.authorityState();
  foreignAdvance.eligibilityGeneration = EligibilityAuthorityGeneration(5);
  foreignAdvance.fence.epoch = ControllerEpoch(runtime.fence().epoch.raw() + 1);
  WF_CHECK_EQ(token(runtime.advanceAuthority(foreignAdvance).code), std::string("stale-epoch"));
  WF_CHECK_EQ(runtime.authorityState().eligibilityGeneration.raw(), std::uint64_t{1});

  WF_CHECK(sameReservations(runtime.reservations(), reservationsBefore));
  const std::optional<SpectrumUsage> usageAfter = runtime.usage(env.domain(), kStart);
  const std::optional<SpectrumUsage> afterExpiryAfter = runtime.usage(env.domain(), kStart + kLease);
  WF_REQUIRE(usageAfter.has_value());
  WF_REQUIRE(afterExpiryAfter.has_value());
  WF_CHECK(sameUsage(*usageBefore, *usageAfter));
  WF_CHECK(sameUsage(*afterExpiryBefore, *afterExpiryAfter));
  WF_CHECK_EQ(usageAfter->activeSlots, std::uint32_t{1});
  WF_CHECK_EQ(usageAfter->freeSlots, std::uint32_t{95});

  // The only counter a stale path moves is the refusal counter of the three
  // refused allocations; no ownership counter moves at all.
  RuntimeStats expected = statsBefore;
  expected.allocationsRefused += 3;
  WF_CHECK(sameStats(runtime.stats(), expected));
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().activations, std::uint64_t{1});
  WF_CHECK_EQ(runtime.stats().renewals, std::uint64_t{0});
  WF_CHECK_EQ(runtime.stats().releases, std::uint64_t{0});
}

}  // namespace

WF_TEST_MAIN()
