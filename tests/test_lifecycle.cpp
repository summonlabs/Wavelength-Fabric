// Lifecycle tests: the guarded reservation state machine through the public
// API, illegal transitions, duplicate release, leases that lapse, the expiry and
// reclamation sweeps, plus the complete transition matrix and the state token
// round trips.

#include "test_common.hpp"

#include <array>
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

constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::int64_t kSlotMhz = 50'000;

const Instant t0 = Instant::fromSeconds(1'800'000'000);
const Duration oneHour = Duration::hours(1);
const Duration hundredSeconds = Duration::seconds(100);

const ChannelGridId gridId{ChannelGridId(1)};
const GridGeneration gridGen{GridGeneration(1)};

constexpr std::size_t kStateCount = 18;

[[nodiscard]] std::string codeOf(const Status& status) { return std::string(toToken(status.code)); }

[[nodiscard]] std::string stateOf(ReservationState state) { return std::string(toToken(state)); }

[[nodiscard]] bool mentions(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// A flex grid with 50 GHz slots: multi-slot channels need a grid that can
// express them, and the flex grid keeps the arithmetic identical.
void registerGrid(SpectrumRuntime& runtime) {
  (void)runtime.registerGrid(flexGrid(gridId, gridGen, kAnchorMhz, kSlotMhz, 96, 1, 32));
}

SpectrumRequest makeAllocation(AllocationRequestId requestId, std::uint32_t slots, Instant requestedAt,
                               Duration lease, const Fixture& fixture) {
  return makeRequest(requestId, OwnerId(7), {SpectrumDomainId(1)}, {SpectrumDomainGeneration(1)}, gridId,
                     gridGen, slots, requestedAt, lease, fixture.eligibility(), fixture.reservation(),
                     ContiguityRequirement::Required, ContinuityRequirement::Unspecified);
}

// The complete guarded state machine: the states each state may move to.
// Everything absent from a row must be refused. Rows are in ReservationState
// order: none, requested, evaluated, committing, reserved, activating, active,
// deactivating, renewing, releasing, reclaiming, recovering, released, expired,
// reclaimed, superseded, refused, retired.
[[nodiscard]] const std::array<std::vector<ReservationState>, kStateCount>& transitionTable() {
  static const std::array<std::vector<ReservationState>, kStateCount> table = {{
      {ReservationState::Requested},
      {ReservationState::Evaluated, ReservationState::Refused},
      {ReservationState::Committing, ReservationState::Refused},
      {ReservationState::Reserved, ReservationState::Refused},
      {ReservationState::Activating, ReservationState::Renewing, ReservationState::Releasing,
       ReservationState::Reclaiming, ReservationState::Expired, ReservationState::Superseded},
      {ReservationState::Active, ReservationState::Reserved, ReservationState::Reclaiming},
      {ReservationState::Deactivating, ReservationState::Renewing, ReservationState::Releasing,
       ReservationState::Reclaiming, ReservationState::Expired, ReservationState::Superseded},
      {ReservationState::Reserved, ReservationState::Active, ReservationState::Releasing,
       ReservationState::Reclaiming},
      {ReservationState::Reserved, ReservationState::Active, ReservationState::Expired,
       ReservationState::Reclaiming},
      {ReservationState::Released},
      {ReservationState::Reclaimed},
      {ReservationState::Reserved, ReservationState::Expired, ReservationState::Reclaiming},
      {},
      {ReservationState::Reclaiming, ReservationState::Reclaimed},
      {},
      {},
      {},
      {},
  }};
  return table;
}

[[nodiscard]] bool expectedTransition(ReservationState from, ReservationState to) {
  const std::size_t index = static_cast<std::size_t>(from);
  if (index >= kStateCount) return false;
  const std::vector<ReservationState>& targets = transitionTable()[index];
  for (const ReservationState target : targets) {
    if (target == to) return true;
  }
  return false;
}

[[nodiscard]] const std::array<ReservationState, kStateCount>& allStates() {
  static const std::array<ReservationState, kStateCount> states = {{
      ReservationState::None,        ReservationState::Requested,   ReservationState::Evaluated,
      ReservationState::Committing,  ReservationState::Reserved,    ReservationState::Activating,
      ReservationState::Active,      ReservationState::Deactivating, ReservationState::Renewing,
      ReservationState::Releasing,   ReservationState::Reclaiming,  ReservationState::Recovering,
      ReservationState::Released,    ReservationState::Expired,     ReservationState::Reclaimed,
      ReservationState::Superseded,  ReservationState::Refused,     ReservationState::Retired,
  }};
  return states;
}

[[nodiscard]] const std::array<std::string_view, kStateCount>& allStateTokens() {
  static const std::array<std::string_view, kStateCount> tokens = {{
      "none",       "requested", "evaluated",  "committing", "reserved",  "activating",
      "active",     "deactivating", "renewing", "releasing",  "reclaiming", "recovering",
      "released",   "expired",   "reclaimed",  "superseded", "refused",   "retired",
  }};
  return tokens;
}

}  // namespace

// ---------------------------------------------------------------------------
// Legal transitions through the public API
// ---------------------------------------------------------------------------

WF_TEST(legal_transitions_drive_the_record_exactly) {
  Fixture fx;
  registerGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), gridId, 96);

  SpectrumRequest request = makeAllocation(AllocationRequestId(300), 2, t0, oneHour, fx);
  request.maxRenewals = 2;
  const AllocationDecision allocated = fx.runtime.allocate(request);
  WF_REQUIRE(allocated.allocated());
  const ReservationId id = allocated.reservation;
  const std::optional<SpectrumReservation> committed = fx.runtime.reservation(id);
  WF_REQUIRE(committed.has_value());
  WF_SAME(stateOf(committed->state), "reserved");
  WF_SAME(committed->generation.raw(), std::uint64_t{1});
  WF_SAME(committed->lease.generation.raw(), std::uint64_t{1});
  WF_SAME(committed->lease.renewalCount, 0u);
  WF_SAME(committed->lease.maxRenewals, 2u);
  WF_SAME(committed->lastOperation.raw(), std::uint64_t{1});
  WF_SAME(committed->lease.expiresAt.nanos(), (t0 + oneHour).nanos());

  const std::optional<SpectrumUsage> reservedUsage = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(reservedUsage.has_value());
  WF_SAME(reservedUsage->reservedSlots, 2u);
  WF_SAME(reservedUsage->activeSlots, 0u);
  WF_SAME(reservedUsage->liveSlots, 2u);

  // Reserved -> Activating -> Active
  const Instant activatedAt = t0 + Duration::seconds(10);
  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), activatedAt)), "ok");
  const std::optional<SpectrumReservation> active = fx.runtime.reservation(id);
  WF_REQUIRE(active.has_value());
  WF_SAME(stateOf(active->state), "active");
  WF_SAME(active->activatedAt.nanos(), activatedAt.nanos());
  WF_SAME(active->updatedAt.nanos(), activatedAt.nanos());
  WF_SAME(active->lastOperation.raw(), std::uint64_t{2});
  WF_SAME(active->generation.raw(), std::uint64_t{1});
  WF_CHECK(!active->needsRevalidation);
  WF_SAME(fx.runtime.stats().activations, std::uint64_t{1});
  const std::optional<SpectrumUsage> activeUsage = fx.runtime.usage(SpectrumDomainId(1), activatedAt);
  WF_REQUIRE(activeUsage.has_value());
  WF_SAME(activeUsage->activeSlots, 2u);
  WF_SAME(activeUsage->activeReservations, 1u);
  WF_SAME(activeUsage->reservedSlots, 0u);

  // Active -> Deactivating -> Reserved keeps ownership.
  const Instant deactivatedAt = t0 + Duration::seconds(20);
  WF_SAME(codeOf(fx.runtime.deactivate(id, ReservationGeneration(1), fx.activation(), deactivatedAt)), "ok");
  const std::optional<SpectrumReservation> deactivated = fx.runtime.reservation(id);
  WF_REQUIRE(deactivated.has_value());
  WF_SAME(stateOf(deactivated->state), "reserved");
  WF_SAME(deactivated->deactivatedAt.nanos(), deactivatedAt.nanos());
  WF_SAME(deactivated->lastOperation.raw(), std::uint64_t{3});
  WF_SAME(deactivated->lease.expiresAt.nanos(), (t0 + oneHour).nanos());
  WF_SAME(fx.runtime.stats().deactivations, std::uint64_t{1});
  const std::optional<SpectrumUsage> backToReserved = fx.runtime.usage(SpectrumDomainId(1), deactivatedAt);
  WF_REQUIRE(backToReserved.has_value());
  WF_SAME(backToReserved->reservedSlots, 2u);
  WF_SAME(backToReserved->activeSlots, 0u);
  WF_SAME(backToReserved->liveSlots, 2u);

  // Activating again from Reserved is legal.
  const Instant reactivatedAt = t0 + Duration::seconds(30);
  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), reactivatedAt)), "ok");
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "active");
  WF_SAME(fx.runtime.reservation(id)->activatedAt.nanos(), reactivatedAt.nanos());
  WF_SAME(fx.runtime.reservation(id)->lastOperation.raw(), std::uint64_t{4});
  WF_SAME(fx.runtime.stats().activations, std::uint64_t{2});

  // Renewing bumps both the lease generation and the reservation generation.
  const Instant renewedAt = t0 + Duration::seconds(40);
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(1), Duration::seconds(30), fx.reservation(), renewedAt, 0)), "ok");
  const std::optional<SpectrumReservation> renewed = fx.runtime.reservation(id);
  WF_REQUIRE(renewed.has_value());
  WF_SAME(stateOf(renewed->state), "active");
  WF_SAME(renewed->generation.raw(), std::uint64_t{2});
  WF_SAME(renewed->lease.generation.raw(), std::uint64_t{2});
  WF_SAME(renewed->lease.renewalCount, 1u);
  WF_SAME(renewed->lease.maxRenewals, 2u);
  WF_SAME(renewed->lease.expiresAt.nanos(), (t0 + oneHour + Duration::seconds(30)).nanos());
  WF_SAME(renewed->updatedAt.nanos(), renewedAt.nanos());
  WF_SAME(renewed->lastOperation.raw(), std::uint64_t{5});
  WF_SAME(renewed->deactivatedAt.nanos(), deactivatedAt.nanos());
  WF_SAME(fx.runtime.stats().renewals, std::uint64_t{1});

  // The previous generation is stale for every lifecycle operation.
  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), renewedAt)), "stale-generation");
  WF_SAME(codeOf(fx.runtime.deactivate(id, ReservationGeneration(1), fx.activation(), renewedAt)), "stale-generation");
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), fx.release(), renewedAt)), "stale-generation");
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(1), Duration::seconds(30), fx.reservation(), renewedAt, 0)), "stale-generation");
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{0});
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "active");

  // A second renewal is allowed by the stored cap and keeps the state.
  WF_SAME(codeOf(fx.runtime.deactivate(id, ReservationGeneration(2), fx.activation(), t0 + Duration::seconds(50))), "ok");
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(2), Duration::seconds(60), fx.reservation(), t0 + Duration::seconds(50), 0)), "ok");
  const std::optional<SpectrumReservation> twice = fx.runtime.reservation(id);
  WF_REQUIRE(twice.has_value());
  WF_SAME(stateOf(twice->state), "reserved");
  WF_SAME(twice->generation.raw(), std::uint64_t{3});
  WF_SAME(twice->lease.renewalCount, 2u);
  WF_SAME(twice->lease.maxRenewals, 2u);
  WF_SAME(twice->lease.expiresAt.nanos(), (t0 + oneHour + Duration::seconds(90)).nanos());
  WF_SAME(fx.runtime.stats().renewals, std::uint64_t{2});

  // Reserved -> Releasing -> Released.
  const Instant releasedAt = t0 + Duration::seconds(60);
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(3), fx.release(), releasedAt)), "ok");
  const std::optional<SpectrumReservation> released = fx.runtime.reservation(id);
  WF_REQUIRE(released.has_value());
  WF_SAME(stateOf(released->state), "released");
  WF_SAME(released->releasedAt.nanos(), releasedAt.nanos());
  WF_SAME(released->updatedAt.nanos(), releasedAt.nanos());
  WF_SAME(released->lastOperation.raw(), std::uint64_t{8});
  WF_SAME(released->lease.expiresAt.nanos(), (t0 + oneHour + Duration::seconds(90)).nanos());
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});
  const std::optional<SpectrumUsage> releasedUsage = fx.runtime.usage(SpectrumDomainId(1), releasedAt);
  WF_REQUIRE(releasedUsage.has_value());
  WF_SAME(releasedUsage->liveSlots, 0u);
  WF_SAME(releasedUsage->liveReservations, 0u);
  WF_SAME(releasedUsage->releasedReservations, 1u);
  WF_SAME(releasedUsage->freeSlots, 96u);
  WF_REQUIRE(releasedUsage->freeRuns.size() == 1);
  WF_SAME(releasedUsage->freeRuns.front().first, 0u);
  WF_SAME(releasedUsage->freeRuns.front().count, 96u);

  const RuntimeStats stats = fx.runtime.stats();
  WF_SAME(stats.allocationsCommitted, std::uint64_t{1});
  WF_SAME(stats.allocationsRefused, std::uint64_t{0});
  WF_SAME(stats.activations, std::uint64_t{2});
  WF_SAME(stats.deactivations, std::uint64_t{2});
  WF_SAME(stats.renewals, std::uint64_t{2});
  WF_SAME(stats.releases, std::uint64_t{1});
  WF_SAME(stats.reclamations, std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// Illegal transitions
// ---------------------------------------------------------------------------

WF_TEST(illegal_transitions_are_refused_and_change_nothing) {
  Fixture fx;
  registerGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), gridId, 96);

  const AllocationDecision allocated = fx.runtime.allocate(makeAllocation(AllocationRequestId(310), 2, t0, oneHour, fx));
  WF_REQUIRE(allocated.allocated());
  const ReservationId id = allocated.reservation;
  const Instant at = t0 + Duration::seconds(10);

  // Deactivating a reservation that was never activated.
  WF_SAME(codeOf(fx.runtime.deactivate(id, ReservationGeneration(1), fx.activation(), at)), "illegal-transition");
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "reserved");
  WF_SAME(fx.runtime.reservation(id)->deactivatedAt.nanos(), std::int64_t{0});
  WF_SAME(fx.runtime.reservation(id)->lastOperation.raw(), std::uint64_t{1});
  WF_SAME(fx.runtime.stats().deactivations, std::uint64_t{0});

  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), at)), "ok");
  // Activating an Active reservation.
  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), at)), "illegal-transition");
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "active");
  WF_SAME(fx.runtime.reservation(id)->lastOperation.raw(), std::uint64_t{2});
  WF_SAME(fx.runtime.stats().activations, std::uint64_t{1});

  WF_SAME(codeOf(fx.runtime.deactivate(id, ReservationGeneration(1), fx.activation(), at)), "ok");
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), fx.release(), at)), "ok");
  const std::optional<SpectrumReservation> released = fx.runtime.reservation(id);
  WF_REQUIRE(released.has_value());
  const std::uint64_t releasedOperation = released->lastOperation.raw();
  WF_SAME(stateOf(released->state), "released");

  // Every lifecycle operation on a terminal record is refused.
  const Status cases[4] = {
      fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), at),
      fx.runtime.deactivate(id, ReservationGeneration(1), fx.activation(), at),
      fx.runtime.release(id, ReservationGeneration(1), fx.release(), at),
      fx.runtime.renew(id, ReservationGeneration(1), Duration::seconds(10), fx.reservation(), at, 0),
  };
  for (const Status& status : cases) {
    WF_SAME(codeOf(status), "illegal-transition");
  }
  WF_CHECK(mentions(cases[2].message, "owns no spectrum"));
  WF_CHECK(mentions(cases[2].message, "duplicate release never creates capacity"));
  WF_CHECK(mentions(cases[3].message, "cannot be renewed"));
  WF_CHECK(mentions(cases[0].message, "cannot be activated"));
  WF_CHECK(mentions(cases[1].message, "cannot be deactivated"));
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "released");
  WF_SAME(fx.runtime.reservation(id)->lastOperation.raw(), releasedOperation);
  WF_SAME(fx.runtime.reservation(id)->generation.raw(), std::uint64_t{1});
  const RuntimeStats afterRefusals = fx.runtime.stats();
  WF_SAME(afterRefusals.activations, std::uint64_t{1});
  WF_SAME(afterRefusals.deactivations, std::uint64_t{1});
  WF_SAME(afterRefusals.releases, std::uint64_t{1});
  WF_SAME(afterRefusals.renewals, std::uint64_t{0});

  // Unknown identities are not found; a wrong generation is stale.
  WF_SAME(codeOf(fx.runtime.activate(ReservationId(99), ReservationGeneration(1), fx.activation(), at)), "not-found");
  WF_SAME(codeOf(fx.runtime.deactivate(ReservationId(99), ReservationGeneration(1), fx.activation(), at)), "not-found");
  WF_SAME(codeOf(fx.runtime.release(ReservationId(99), ReservationGeneration(1), fx.release(), at)), "not-found");
  WF_SAME(codeOf(fx.runtime.renew(ReservationId(99), ReservationGeneration(1), Duration::seconds(10), fx.reservation(), at, 0)), "not-found");
  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(7), fx.activation(), at)), "stale-generation");
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(7), fx.release(), at)), "stale-generation");

  // A reclaimed record is terminal as well, and reclamation is what returns the
  // spectrum.
  const AllocationDecision shortLived = fx.runtime.allocate(makeAllocation(AllocationRequestId(311), 1, t0, hundredSeconds, fx));
  WF_REQUIRE(shortLived.allocated());
  const ReclaimReport report = fx.runtime.reclaimExpired(t0 + hundredSeconds);
  WF_REQUIRE(report.reclaimed.size() == 1);
  WF_SAME(report.reclaimed.front().raw(), shortLived.reservation.raw());
  WF_SAME(stateOf(fx.runtime.reservation(shortLived.reservation)->state), "reclaimed");
  WF_SAME(codeOf(fx.runtime.activate(shortLived.reservation, ReservationGeneration(1), fx.activation(), t0 + hundredSeconds)), "illegal-transition");
  WF_SAME(codeOf(fx.runtime.deactivate(shortLived.reservation, ReservationGeneration(1), fx.activation(), t0 + hundredSeconds)), "illegal-transition");
  WF_SAME(codeOf(fx.runtime.release(shortLived.reservation, ReservationGeneration(1), fx.release(), t0 + hundredSeconds)), "illegal-transition");
  WF_SAME(codeOf(fx.runtime.renew(shortLived.reservation, ReservationGeneration(1), Duration::seconds(10), fx.reservation(), t0 + hundredSeconds, 0)), "illegal-transition");
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});
}

WF_TEST(double_release_never_creates_capacity) {
  Fixture fx;
  registerGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), gridId, 96);

  const AllocationDecision allocated = fx.runtime.allocate(makeAllocation(AllocationRequestId(320), 2, t0, oneHour, fx));
  WF_REQUIRE(allocated.allocated());
  const ReservationId id = allocated.reservation;
  const Instant releasedAt = t0 + Duration::seconds(5);
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), fx.release(), releasedAt)), "ok");

  const std::optional<SpectrumReservation> released = fx.runtime.reservation(id);
  WF_REQUIRE(released.has_value());
  WF_SAME(released->lastOperation.raw(), std::uint64_t{2});
  const std::optional<SpectrumUsage> releasedUsage = fx.runtime.usage(SpectrumDomainId(1), releasedAt);
  WF_REQUIRE(releasedUsage.has_value());
  WF_SAME(releasedUsage->liveSlots, 0u);
  WF_SAME(releasedUsage->releasedReservations, 1u);
  WF_SAME(releasedUsage->freeSlots, 96u);

  // Releasing again is refused and cannot manufacture capacity.
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), fx.release(), releasedAt)), "illegal-transition");
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});
  const std::optional<SpectrumReservation> afterDouble = fx.runtime.reservation(id);
  WF_REQUIRE(afterDouble.has_value());
  WF_SAME(stateOf(afterDouble->state), "released");
  WF_SAME(afterDouble->releasedAt.nanos(), releasedAt.nanos());
  WF_SAME(afterDouble->lastOperation.raw(), std::uint64_t{2});
  const std::optional<SpectrumUsage> afterDoubleUsage = fx.runtime.usage(SpectrumDomainId(1), releasedAt);
  WF_REQUIRE(afterDoubleUsage.has_value());
  WF_SAME(afterDoubleUsage->liveSlots, 0u);
  WF_SAME(afterDoubleUsage->releasedReservations, 1u);
  WF_SAME(afterDoubleUsage->freeSlots, 96u);

  // A fresh allocation reuses exactly the released range.
  const AllocationDecision reused = fx.runtime.allocate(makeAllocation(AllocationRequestId(321), 2, t0, oneHour, fx));
  WF_REQUIRE(reused.allocated());
  WF_SAME(reused.explanation.selected.slots.first, 0u);
  WF_SAME(reused.explanation.selected.slots.count, 2u);
  WF_SAME(fx.runtime.reservations().size(), std::size_t{2});

  // The authority check runs before the state check, so a stale token on a
  // released record is reported as stale, not as an illegal transition.
  ReleaseAuthority stale = fx.release();
  stale.generation = ReleaseAuthorityGeneration(9);
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), stale, releasedAt)), "stale-generation");
  ReleaseAuthority wrongEpoch = fx.release();
  wrongEpoch.fence.epoch = ControllerEpoch(fx.runtime.epoch().raw() + 1);
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), wrongEpoch, releasedAt)), "stale-epoch");
  ReleaseAuthority wrongIncarnation = fx.release();
  wrongIncarnation.fence.incarnation = ControllerIncarnation(fx.runtime.incarnation().raw() + 1);
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), wrongIncarnation, releasedAt)), "stale-incarnation");
  ReleaseAuthority missing;
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), missing, releasedAt)), "invalid-argument");
  ActivationAuthority missingActivation;
  WF_SAME(codeOf(fx.runtime.activate(reused.reservation, ReservationGeneration(1), missingActivation, releasedAt)), "invalid-argument");
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "released");
  WF_SAME(stateOf(fx.runtime.reservation(reused.reservation)->state), "reserved");
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});

  // Releasing the live reservation from the same runtime still works.
  WF_SAME(codeOf(fx.runtime.release(reused.reservation, ReservationGeneration(1), fx.release(), releasedAt)), "ok");
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{2});
  const std::optional<SpectrumUsage> finalUsage = fx.runtime.usage(SpectrumDomainId(1), releasedAt);
  WF_REQUIRE(finalUsage.has_value());
  WF_SAME(finalUsage->liveSlots, 0u);
  WF_SAME(finalUsage->releasedReservations, 2u);
  WF_SAME(finalUsage->freeRuns.size(), std::size_t{1});
  WF_SAME(finalUsage->freeRuns.front().count, 96u);
}

WF_TEST(activate_and_renew_require_a_valid_lease_but_release_does_not) {
  Fixture fx;
  registerGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), gridId, 96);

  const AllocationDecision allocated = fx.runtime.allocate(makeAllocation(AllocationRequestId(330), 2, t0, hundredSeconds, fx));
  WF_REQUIRE(allocated.allocated());
  const ReservationId id = allocated.reservation;
  const Instant lapsed = t0 + hundredSeconds;
  WF_REQUIRE(fx.runtime.reservation(id).has_value());
  WF_SAME(fx.runtime.reservation(id)->lease.expiresAt.nanos(), lapsed.nanos());

  // One nanosecond before the expiry instant the lease is still valid.
  WF_SAME(codeOf(fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), lapsed - Duration::nanos(1))), "ok");
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "active");
  WF_SAME(codeOf(fx.runtime.deactivate(id, ReservationGeneration(1), fx.activation(), lapsed - Duration::nanos(1))), "ok");

  // At the expiry instant the lease is not valid: a lapsed lease owns nothing,
  // yet the record is still Reserved until a sweep moves it.
  const std::optional<SpectrumUsage> atExpiry = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(atExpiry.has_value());
  WF_SAME(atExpiry->liveSlots, 0u);
  WF_SAME(atExpiry->liveReservations, 0u);
  WF_SAME(atExpiry->lapsedSlots, 2u);
  WF_SAME(atExpiry->lapsedReservations, 1u);
  WF_SAME(atExpiry->freeSlots, 96u);
  WF_REQUIRE(atExpiry->freeRuns.size() == 1);
  WF_SAME(atExpiry->freeRuns.front().first, 0u);
  WF_SAME(atExpiry->freeRuns.front().count, 96u);

  const Status activation = fx.runtime.activate(id, ReservationGeneration(1), fx.activation(), lapsed);
  WF_SAME(codeOf(activation), "refused");
  WF_CHECK(mentions(activation.message, "the lease lapsed"));
  const Status renewal = fx.runtime.renew(id, ReservationGeneration(1), Duration::seconds(30), fx.reservation(), lapsed, 0);
  WF_SAME(codeOf(renewal), "refused");
  WF_CHECK(mentions(renewal.message, "a fresh allocation request is required"));
  WF_SAME(stateOf(fx.runtime.reservation(id)->state), "reserved");
  WF_SAME(fx.runtime.reservation(id)->activatedAt.nanos(), (lapsed - Duration::nanos(1)).nanos());
  WF_SAME(fx.runtime.reservation(id)->lease.renewalCount, 0u);
  WF_SAME(fx.runtime.reservation(id)->generation.raw(), std::uint64_t{1});
  WF_SAME(fx.runtime.reservation(id)->lease.expiresAt.nanos(), lapsed.nanos());
  const RuntimeStats lapsedStats = fx.runtime.stats();
  WF_SAME(lapsedStats.activations, std::uint64_t{1});
  WF_SAME(lapsedStats.renewals, std::uint64_t{0});
  WF_SAME(lapsedStats.replayRejections, std::uint64_t{2});

  // Release only requires a live state, not a valid lease.
  WF_SAME(codeOf(fx.runtime.release(id, ReservationGeneration(1), fx.release(), lapsed)), "ok");
  const std::optional<SpectrumReservation> released = fx.runtime.reservation(id);
  WF_REQUIRE(released.has_value());
  WF_SAME(stateOf(released->state), "released");
  WF_SAME(released->releasedAt.nanos(), lapsed.nanos());
  WF_SAME(released->lease.expiresAt.nanos(), lapsed.nanos());
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});
  const std::optional<SpectrumUsage> afterRelease = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(afterRelease.has_value());
  WF_SAME(afterRelease->lapsedSlots, 0u);
  WF_SAME(afterRelease->lapsedReservations, 0u);
  WF_SAME(afterRelease->releasedReservations, 1u);
  WF_SAME(afterRelease->liveSlots, 0u);
}

WF_TEST(renewal_arguments_are_checked_before_state_and_identity) {
  Fixture fx;
  registerGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), gridId, 96);

  const AllocationDecision allocated = fx.runtime.allocate(makeAllocation(AllocationRequestId(340), 1, t0, oneHour, fx));
  WF_REQUIRE(allocated.allocated());
  const ReservationId id = allocated.reservation;

  // A non-positive or oversized extension is an invalid argument, whatever the
  // state of the reservation or the identity of the token.
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(1), Duration::zero(), fx.reservation(), t0, 0)), "invalid-argument");
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(1), Duration::nanos(-1), fx.reservation(), t0, 0)), "invalid-argument");
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(1), Duration::nanos(Duration::days(3650).nanos() + 1), fx.reservation(), t0, 0)), "invalid-argument");
  ReservationAuthority missing;
  WF_SAME(codeOf(fx.runtime.renew(ReservationId(99), ReservationGeneration(1), Duration::zero(), missing, t0, 0)), "invalid-argument");
  WF_SAME(codeOf(fx.runtime.renew(ReservationId(99), ReservationGeneration(1), Duration::zero(), fx.reservation(), t0, 0)), "invalid-argument");
  WF_SAME(fx.runtime.stats().renewals, std::uint64_t{0});

  // Exactly 3650 days is accepted.
  SpectrumRequest capped = makeAllocation(AllocationRequestId(341), 1, t0, oneHour, fx);
  capped.maxRenewals = 1;
  const AllocationDecision second = fx.runtime.allocate(capped);
  WF_REQUIRE(second.allocated());
  WF_SAME(codeOf(fx.runtime.renew(second.reservation, ReservationGeneration(1), Duration::days(3650), fx.reservation(), t0 + Duration::seconds(1), 0)), "ok");
  WF_SAME(fx.runtime.reservation(second.reservation)->lease.renewalCount, 1u);
  WF_SAME(fx.runtime.reservation(second.reservation)->lease.expiresAt.nanos(), (t0 + Duration::days(3650) + oneHour).nanos());

  // The stored cap wins: a larger argument cannot raise it, and reaching it
  // refuses further renewals.
  WF_SAME(codeOf(fx.runtime.renew(second.reservation, ReservationGeneration(2), Duration::seconds(10), fx.reservation(), t0 + Duration::seconds(2), 5)), "limit-exceeded");
  WF_SAME(fx.runtime.reservation(second.reservation)->lease.renewalCount, 1u);
  WF_SAME(codeOf(fx.runtime.renew(second.reservation, ReservationGeneration(2), Duration::seconds(10), fx.reservation(), t0 + Duration::seconds(3), 0)), "limit-exceeded");
  WF_SAME(fx.runtime.reservation(second.reservation)->lease.renewalCount, 1u);
  // An uncapped lease accepts a cap argument, which then binds it.
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(1), Duration::seconds(10), fx.reservation(), t0 + Duration::seconds(1), 1)), "ok");
  WF_SAME(fx.runtime.reservation(id)->lease.maxRenewals, 1u);
  WF_SAME(fx.runtime.reservation(id)->lease.renewalCount, 1u);
  WF_SAME(codeOf(fx.runtime.renew(id, ReservationGeneration(2), Duration::seconds(10), fx.reservation(), t0 + Duration::seconds(2), 0)), "limit-exceeded");
  WF_SAME(fx.runtime.stats().renewals, std::uint64_t{2});
}

// ---------------------------------------------------------------------------
// Expiry and reclamation
// ---------------------------------------------------------------------------

WF_TEST(expiry_and_reclamation_sweeps_account_exactly) {
  Fixture fx;
  registerGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), gridId, 96);

  const AllocationDecision shortLived = fx.runtime.allocate(makeAllocation(AllocationRequestId(350), 2, t0, hundredSeconds, fx));
  const AllocationDecision longLived = fx.runtime.allocate(makeAllocation(AllocationRequestId(351), 2, t0, oneHour, fx));
  WF_REQUIRE(shortLived.allocated() && longLived.allocated());
  WF_SAME(shortLived.explanation.selected.slots.first, 0u);
  WF_SAME(longLived.explanation.selected.slots.first, 2u);

  // Nothing has lapsed before the expiry instant.
  const ReclaimReport early = fx.runtime.expireLeases(t0 + hundredSeconds - Duration::nanos(1));
  WF_SAME(early.expired, std::size_t{0});
  WF_SAME(early.scanned, std::size_t{2});
  WF_CHECK(early.lapsed.empty());
  WF_CHECK(early.reclaimed.empty());
  WF_SAME(fx.runtime.stats().expirationSweeps, std::uint64_t{1});

  // expireLeases marks the lapsed record Expired without reclaiming it.
  const Instant lapsed = t0 + hundredSeconds;
  const ReclaimReport expired = fx.runtime.expireLeases(lapsed);
  WF_SAME(codeOf(expired.status), "ok");
  WF_SAME(expired.evaluatedAt.nanos(), lapsed.nanos());
  WF_SAME(expired.scanned, std::size_t{2});
  WF_SAME(expired.expired, std::size_t{1});
  WF_REQUIRE(expired.lapsed.size() == 1);
  WF_SAME(expired.lapsed.front().raw(), shortLived.reservation.raw());
  WF_CHECK(expired.reclaimed.empty());
  const std::optional<SpectrumReservation> expiredRecord = fx.runtime.reservation(shortLived.reservation);
  WF_REQUIRE(expiredRecord.has_value());
  WF_SAME(stateOf(expiredRecord->state), "expired");
  WF_SAME(expiredRecord->updatedAt.nanos(), lapsed.nanos());
  WF_SAME(expiredRecord->lastOperation.raw(), std::uint64_t{2});
  WF_SAME(expiredRecord->reclaimedAt.nanos(), std::int64_t{0});
  const std::optional<SpectrumReservation> survivor = fx.runtime.reservation(longLived.reservation);
  WF_REQUIRE(survivor.has_value());
  WF_SAME(stateOf(survivor->state), "reserved");
  WF_SAME(survivor->updatedAt.nanos(), t0.nanos());
  WF_SAME(survivor->lastOperation.raw(), std::uint64_t{1});
  WF_SAME(fx.runtime.stats().expirationSweeps, std::uint64_t{2});
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{0});

  const std::optional<SpectrumUsage> afterExpiry = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(afterExpiry.has_value());
  WF_SAME(afterExpiry->liveSlots, 2u);
  WF_SAME(afterExpiry->liveReservations, 1u);
  WF_SAME(afterExpiry->lapsedSlots, 0u);
  WF_SAME(afterExpiry->reclaimedReservations, 0u);
  WF_REQUIRE(afterExpiry->freeRuns.size() == 2);
  WF_SAME(afterExpiry->freeRuns[0].first, 0u);
  WF_SAME(afterExpiry->freeRuns[0].count, 2u);
  WF_SAME(afterExpiry->freeRuns[1].first, 4u);
  WF_SAME(afterExpiry->freeRuns[1].count, 92u);

  // An Expired record releases nothing and cannot be released.
  WF_SAME(codeOf(fx.runtime.release(shortLived.reservation, ReservationGeneration(1), fx.release(), lapsed)), "illegal-transition");
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{0});

  // A second sweep with nothing newly lapsed changes nothing.
  const ReclaimReport idle = fx.runtime.expireLeases(t0 + Duration::seconds(200));
  WF_SAME(idle.expired, std::size_t{0});
  WF_CHECK(idle.lapsed.empty());
  WF_SAME(fx.runtime.stats().expirationSweeps, std::uint64_t{3});

  // reclaimExpired moves the Expired record to Reclaimed.
  const Instant reclaimedAt = t0 + Duration::seconds(300);
  const ReclaimReport reclaim = fx.runtime.reclaimExpired(reclaimedAt);
  WF_SAME(reclaim.scanned, std::size_t{2});
  WF_SAME(reclaim.expired, std::size_t{0});
  WF_SAME(reclaim.reclaimed.size(), std::size_t{1});
  WF_SAME(reclaim.reclaimed.front().raw(), shortLived.reservation.raw());
  WF_CHECK(reclaim.lapsed.empty());
  const std::optional<SpectrumReservation> reclaimed = fx.runtime.reservation(shortLived.reservation);
  WF_REQUIRE(reclaimed.has_value());
  WF_SAME(stateOf(reclaimed->state), "reclaimed");
  WF_SAME(reclaimed->reclaimedAt.nanos(), reclaimedAt.nanos());
  WF_SAME(reclaimed->lastOperation.raw(), std::uint64_t{3});
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});

  // Reclaiming again is a no-op.
  const ReclaimReport again = fx.runtime.reclaimExpired(reclaimedAt + Duration::seconds(1));
  WF_SAME(again.reclaimed.size(), std::size_t{0});
  WF_SAME(again.expired, std::size_t{0});
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});

  // The long-lived reservation lapses exactly at its expiry instant and is
  // expired and reclaimed by the same sweep.
  const Instant longLapsed = t0 + oneHour;
  const ReclaimReport finalSweep = fx.runtime.reclaimExpired(longLapsed);
  WF_SAME(finalSweep.expired, std::size_t{1});
  WF_SAME(finalSweep.reclaimed.size(), std::size_t{1});
  WF_SAME(finalSweep.reclaimed.front().raw(), longLived.reservation.raw());
  WF_REQUIRE(finalSweep.lapsed.size() == 1);
  WF_SAME(finalSweep.lapsed.front().raw(), longLived.reservation.raw());
  WF_SAME(stateOf(fx.runtime.reservation(longLived.reservation)->state), "reclaimed");
  WF_SAME(fx.runtime.reservation(longLived.reservation)->reclaimedAt.nanos(), longLapsed.nanos());
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{2});
  const std::optional<SpectrumUsage> finalUsage = fx.runtime.usage(SpectrumDomainId(1), longLapsed);
  WF_REQUIRE(finalUsage.has_value());
  WF_SAME(finalUsage->liveSlots, 0u);
  WF_SAME(finalUsage->liveReservations, 0u);
  WF_SAME(finalUsage->reclaimedReservations, 2u);
  WF_SAME(finalUsage->freeSlots, 96u);
  WF_REQUIRE(finalUsage->freeRuns.size() == 1);
  WF_SAME(finalUsage->freeRuns.front().first, 0u);
  WF_SAME(finalUsage->freeRuns.front().count, 96u);

  // Reclaimed spectrum is allocatable again.
  const AllocationDecision fresh = fx.runtime.allocate(makeAllocation(AllocationRequestId(352), 2, longLapsed, oneHour, fx));
  WF_REQUIRE(fresh.allocated());
  WF_SAME(fresh.explanation.selected.slots.first, 0u);
  WF_SAME(fresh.explanation.selected.slots.count, 2u);

  // An Active reservation lapses like any other.
  const AllocationDecision active = fx.runtime.allocate(makeAllocation(AllocationRequestId(353), 1, t0, hundredSeconds, fx));
  WF_REQUIRE(active.allocated());
  WF_SAME(codeOf(fx.runtime.activate(active.reservation, ReservationGeneration(1), fx.activation(), t0)), "ok");
  const ReclaimReport activeSweep = fx.runtime.expireLeases(lapsed);
  WF_SAME(activeSweep.expired, std::size_t{1});
  WF_SAME(stateOf(fx.runtime.reservation(active.reservation)->state), "expired");
  const std::optional<SpectrumUsage> activeUsage = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(activeUsage.has_value());
  WF_SAME(activeUsage->activeReservations, 0u);
  // The reservation granted at the later instant is still owned: at this
  // instant its lease has not expired, it simply has not started yet.
  WF_SAME(activeUsage->liveReservations, 1u);
  WF_SAME(activeUsage->liveSlots, 2u);
  WF_SAME(activeUsage->lapsedReservations, 0u);
  WF_SAME(fx.runtime.stats().expirationSweeps, std::uint64_t{7});
}

// ---------------------------------------------------------------------------
// The complete state machine matrix
// ---------------------------------------------------------------------------

WF_TEST(the_transition_matrix_is_complete_and_self_consistent) {
  for (const ReservationState from : allStates()) {
    for (const ReservationState to : allStates()) {
      const bool expected = expectedTransition(from, to);
      WF_SAME(isLegalTransition(from, to), expected);
      const std::string_view reason = illegalTransitionReason(from, to);
      if (expected) {
        WF_CHECK(reason.empty());
      } else {
        WF_CHECK(!reason.empty());
        const std::string text = stateOf(from) + " -> " + stateOf(to) + " is not a legal transition";
        WF_SAME(std::string(reason), text);
      }
    }
  }

  // The commit path must pass through the initial state: nothing may jump from
  // None straight to a committed state.
  WF_CHECK(isLegalTransition(ReservationState::None, ReservationState::Requested));
  WF_CHECK(!isLegalTransition(ReservationState::None, ReservationState::Evaluated));
  WF_CHECK(!isLegalTransition(ReservationState::None, ReservationState::Committing));
  WF_CHECK(!isLegalTransition(ReservationState::None, ReservationState::Reserved));
  WF_CHECK(isLegalTransition(ReservationState::Requested, ReservationState::Evaluated));
  WF_CHECK(isLegalTransition(ReservationState::Evaluated, ReservationState::Committing));
  WF_CHECK(isLegalTransition(ReservationState::Committing, ReservationState::Reserved));
  // Activation is two steps, and a commitment is never skipped.
  WF_CHECK(isLegalTransition(ReservationState::Reserved, ReservationState::Activating));
  WF_CHECK(isLegalTransition(ReservationState::Activating, ReservationState::Active));
  WF_CHECK(!isLegalTransition(ReservationState::Reserved, ReservationState::Active));
  WF_CHECK(!isLegalTransition(ReservationState::Committing, ReservationState::Active));

  // Terminal states never transition again, and every other state can move.
  for (const ReservationState state : allStates()) {
    bool hasOutgoing = false;
    for (const ReservationState to : allStates()) {
      if (isLegalTransition(state, to)) hasOutgoing = true;
    }
    WF_SAME(isTerminalState(state), !hasOutgoing);
    if (!isTerminalState(state)) WF_CHECK(hasOutgoing);
  }

  // Recovering is only entered by durable recovery, never by a transition.
  for (const ReservationState from : allStates()) {
    WF_CHECK(!isLegalTransition(from, ReservationState::Recovering));
  }
  WF_CHECK(isLegalTransition(ReservationState::Recovering, ReservationState::Reserved));
  WF_CHECK(isLegalTransition(ReservationState::Recovering, ReservationState::Expired));
  WF_CHECK(isLegalTransition(ReservationState::Recovering, ReservationState::Reclaiming));

  // States outside the defined set have no transitions and no explanation.
  for (const std::uint8_t raw : {static_cast<std::uint8_t>(18), static_cast<std::uint8_t>(19),
                                 static_cast<std::uint8_t>(200), static_cast<std::uint8_t>(255)}) {
    const ReservationState outside = static_cast<ReservationState>(raw);
    for (const ReservationState to : allStates()) {
      WF_CHECK(!isLegalTransition(outside, to));
      WF_CHECK(!isLegalTransition(to, outside));
      WF_CHECK(!illegalTransitionReason(outside, to).empty());
      WF_CHECK(!illegalTransitionReason(to, outside).empty());
    }
    WF_CHECK(!isTerminalState(outside));
    WF_CHECK(!isLiveState(outside));
    WF_CHECK(!isTransientState(outside));
    WF_SAME(stateOf(outside), "none");
  }
  WF_CHECK(!illegalTransitionReason(static_cast<ReservationState>(18), ReservationState::None).empty());
}

WF_TEST(state_classification_and_tokens_round_trip) {
  std::vector<ReservationState> live;
  std::vector<ReservationState> transient;
  std::vector<ReservationState> terminal;
  for (const ReservationState state : allStates()) {
    if (isLiveState(state)) live.push_back(state);
    if (isTransientState(state)) transient.push_back(state);
    if (isTerminalState(state)) terminal.push_back(state);
  }
  WF_SAME(live.size(), std::size_t{7});
  WF_SAME(transient.size(), std::size_t{9});
  WF_SAME(terminal.size(), std::size_t{5});
  const std::array<ReservationState, 7> expectedLive = {
      ReservationState::Reserved,     ReservationState::Activating, ReservationState::Active,
      ReservationState::Deactivating, ReservationState::Renewing,   ReservationState::Releasing,
      ReservationState::Reclaiming,
  };
  for (std::size_t index = 0; index < expectedLive.size(); ++index) {
    WF_SAME(stateOf(live[index]), stateOf(expectedLive[index]));
  }
  const std::array<ReservationState, 9> expectedTransient = {
      ReservationState::Requested,    ReservationState::Evaluated,   ReservationState::Committing,
      ReservationState::Activating,   ReservationState::Deactivating, ReservationState::Renewing,
      ReservationState::Releasing,    ReservationState::Reclaiming,  ReservationState::Recovering,
  };
  for (std::size_t index = 0; index < expectedTransient.size(); ++index) {
    WF_SAME(stateOf(transient[index]), stateOf(expectedTransient[index]));
  }
  const std::array<ReservationState, 5> expectedTerminal = {
      ReservationState::Released, ReservationState::Reclaimed, ReservationState::Superseded,
      ReservationState::Refused,  ReservationState::Retired,
  };
  for (std::size_t index = 0; index < expectedTerminal.size(); ++index) {
    WF_SAME(stateOf(terminal[index]), stateOf(expectedTerminal[index]));
  }

  // No state is ever two of the three at once, and the committed states are
  // live without being transient.
  for (const ReservationState state : allStates()) {
    WF_CHECK(!(isLiveState(state) && isTerminalState(state)));
    WF_CHECK(!(isTransientState(state) && isTerminalState(state)));
    WF_CHECK(isLiveState(state) || isTransientState(state) || isTerminalState(state) ||
             state == ReservationState::None || state == ReservationState::Expired);
  }
  WF_CHECK(isLiveState(ReservationState::Reserved));
  WF_CHECK(isLiveState(ReservationState::Active));
  WF_CHECK(!isTransientState(ReservationState::Reserved));
  WF_CHECK(!isTransientState(ReservationState::Active));
  WF_CHECK(isLiveState(ReservationState::Activating) && isTransientState(ReservationState::Activating));
  WF_CHECK(isLiveState(ReservationState::Deactivating) && isTransientState(ReservationState::Deactivating));
  WF_CHECK(isLiveState(ReservationState::Renewing) && isTransientState(ReservationState::Renewing));
  WF_CHECK(isLiveState(ReservationState::Releasing) && isTransientState(ReservationState::Releasing));
  WF_CHECK(isLiveState(ReservationState::Reclaiming) && isTransientState(ReservationState::Reclaiming));
  WF_CHECK(isTransientState(ReservationState::Requested) && !isLiveState(ReservationState::Requested));
  WF_CHECK(isTransientState(ReservationState::Evaluated) && !isLiveState(ReservationState::Evaluated));
  WF_CHECK(isTransientState(ReservationState::Committing) && !isLiveState(ReservationState::Committing));
  WF_CHECK(isTransientState(ReservationState::Recovering) && !isLiveState(ReservationState::Recovering));
  WF_CHECK(!isLiveState(ReservationState::None));
  WF_CHECK(!isTransientState(ReservationState::None));
  WF_CHECK(!isTerminalState(ReservationState::None));
  WF_CHECK(!isLiveState(ReservationState::Expired));
  WF_CHECK(!isTransientState(ReservationState::Expired));
  WF_CHECK(!isTerminalState(ReservationState::Expired));

  // Tokens are the persisted spelling: exact, unique and reversible.
  for (std::size_t index = 0; index < kStateCount; ++index) {
    const ReservationState state = allStates()[index];
    const std::string_view token = toToken(state);
    WF_CHECK(!token.empty());
    WF_SAME(std::string(token), std::string(allStateTokens()[index]));
    WF_SAME(stateOf(reservationStateFromToken(token)), stateOf(state));
    for (std::size_t other = index + 1; other < kStateCount; ++other) {
      WF_CHECK(token != toToken(allStates()[other]));
    }
  }

  // Anything unrecognised falls back to None, including near misses.
  const std::array<std::string_view, 12> unknown = {"",     " ",       "  none", "none ",
                                                   "None", "RESERVED", "Reserved", "active ",
                                                   "activating2", "retired-", "reserved	", "0"};
  for (const std::string_view token : unknown) {
    WF_SAME(stateOf(reservationStateFromToken(token)), "none");
  }
  WF_SAME(stateOf(reservationStateFromToken("none")), "none");
  WF_SAME(stateOf(reservationStateFromToken("reclaimed")), "reclaimed");
  WF_SAME(stateOf(reservationStateFromToken("recovering")), "recovering");
}

WF_TEST_MAIN()
