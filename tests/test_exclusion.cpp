// Exclusion domain tests: cross-domain mutual exclusion, the exclusion guard
// band, RefusedExclusion with the blocking identity recorded, same-domain
// overlap staying RefusedConflict, per-member membership bounds, membership
// validation, re-registration, and the guarantee that an exclusion constraint
// never leaks into a domain that is not a member.

#include "test_common.hpp"

#include <algorithm>
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
constexpr std::int64_t kFlexSlotMhz = 12'500;

const Instant t0 = Instant::fromSeconds(1'800'000'000);
const Duration oneHour = Duration::hours(1);

const ChannelGridId flexGridId{ChannelGridId(2)};
const GridGeneration flexGridGen{GridGeneration(1)};

constexpr std::uint32_t kFlexSlots = 384;

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

[[nodiscard]] std::int64_t flexLow(std::uint32_t slot) {
  return kAnchorMhz + kFlexSlotMhz * static_cast<std::int64_t>(slot);
}

void registerFlexGrid(SpectrumRuntime& runtime) {
  (void)runtime.registerGrid(flexGrid(flexGridId, flexGridGen));
}

void addFlexDomain(SpectrumRuntime& runtime, std::uint64_t id, std::uint32_t allocatable) {
  (void)runtime.registerDomain(makeDomain(SpectrumDomainId(id), flexGridId));
  const SpectrumCapability capability = makeCapability(SpectrumDomainId(id), flexGridId, SpectrumDomainGeneration(1),
                                                       flexGridGen, SpectrumSupport::Supported, 0, allocatable,
                                                       runtime.fence());
  (void)runtime.publishCapability(capability);
}

// The registration status as a token, so cases can compare it as a value.
[[nodiscard]] std::string registerCode(SpectrumRuntime& runtime, const ExclusionDomain& exclusion) {
  return codeOf(runtime.registerExclusionDomain(exclusion));
}

SpectrumRequest flexRequest(AllocationRequestId requestId, SpectrumDomainId domain, std::uint32_t slots,
                            const Fixture& fixture, Instant requestedAt = t0) {
  return makeRequest(requestId, OwnerId(7), {domain}, {SpectrumDomainGeneration(1)}, flexGridId, flexGridGen,
                     slots, requestedAt, oneHour, fixture.eligibility(), fixture.reservation(),
                     ContiguityRequirement::Required, ContinuityRequirement::Unspecified);
}

SpectrumRequest pinnedRequest(AllocationRequestId requestId, SpectrumDomainId domain, std::uint32_t slots,
                              const Fixture& fixture, FrequencyRange window) {
  SpectrumRequest request = flexRequest(requestId, domain, slots, fixture);
  request.frequencyWindows.push_back(window);
  return request;
}

ExclusionDomain makeExclusion(ExclusionDomainId id, std::vector<SpectrumDomainId> members,
                              std::int64_t guardBandMhz = 0,
                              ExclusionDomainGeneration generation = ExclusionDomainGeneration(1)) {
  ExclusionDomain exclusion;
  exclusion.id = id;
  exclusion.generation = generation;
  exclusion.members = std::move(members);
  exclusion.guardBandMhz = guardBandMhz;
  exclusion.label = "exclusion-" + std::to_string(id.raw());
  return exclusion;
}

// Every exclusion refusal this file cares about: the request is refused because
// an exclusion domain forbids it, and the blocking reservation is named.
void expectExclusionRefusal(const AllocationDecision& decision, ReservationId blocker) {
  WF_SAME(codeOf(decision.status), "excluded");
  WF_SAME(outcomeOf(decision.outcome), "refused-exclusion");
  WF_CHECK(!decision.allocated());
  WF_SAME(decision.reservation.raw(), std::uint64_t{0});
  WF_REQUIRE(!decision.explanation.conflicts.empty());
  WF_CHECK(std::find(decision.explanation.conflicts.begin(), decision.explanation.conflicts.end(), blocker) != decision.explanation.conflicts.end());
  WF_REQUIRE(!decision.explanation.rejected.empty());
  WF_SAME(eligibilityOf(decision.explanation.rejected.front().eligibility), "ineligible-excluded");
  WF_CHECK(std::find(decision.explanation.rejected.front().conflicts.begin(), decision.explanation.rejected.front().conflicts.end(), blocker) != decision.explanation.rejected.front().conflicts.end());
  WF_REQUIRE(!decision.explanation.reasons.empty());
  WF_CHECK(mentions(decision.explanation.reasons, "exclusion domain forbids"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Cross-domain mutual exclusion
// ---------------------------------------------------------------------------

WF_TEST(cross_domain_mutual_exclusion_refuses_the_shared_spectrum) {
  Fixture fx;
  registerFlexGrid(fx.runtime);
  // Two members, each able to hold exactly the two slots this case uses, so the
  // shared spectrum cannot be avoided by moving.
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  (void)fx.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  const ExclusionDomain pair = makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)});
  WF_SAME(registerCode(fx.runtime, pair), "ok");
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{1});
  WF_REQUIRE(fx.runtime.exclusionDomain(ExclusionDomainId(1)).has_value());
  const ExclusionDomain storedExclusion = *fx.runtime.exclusionDomain(ExclusionDomainId(1));
  WF_CHECK(exclusionDomainContains(storedExclusion, SpectrumDomainId(1)));
  WF_CHECK(exclusionDomainContains(storedExclusion, SpectrumDomainId(2)));
  WF_CHECK(!exclusionDomainContains(storedExclusion, SpectrumDomainId(3)));

  const AllocationDecision owner = fx.runtime.allocate(flexRequest(AllocationRequestId(200), SpectrumDomainId(1), 2, fx));
  WF_REQUIRE(owner.allocated());
  WF_SAME(owner.reservation.raw(), std::uint64_t{1});
  WF_SAME(owner.explanation.selected.slots.first, 0u);
  WF_SAME(owner.explanation.selected.slots.count, 2u);
  WF_SAME(owner.explanation.selected.frequency.lowMhz, flexLow(0));
  WF_SAME(owner.explanation.selected.frequency.highMhz, flexLow(2));

  // The sibling member owns nothing itself, so this refusal is purely the
  // exclusion constraint.
  const std::optional<SpectrumUsage> siblingBefore = fx.runtime.usage(SpectrumDomainId(2), t0);
  WF_REQUIRE(siblingBefore.has_value());
  WF_SAME(siblingBefore->liveSlots, 0u);
  WF_SAME(siblingBefore->liveReservations, 0u);

  const AllocationDecision refused = fx.runtime.allocate(flexRequest(AllocationRequestId(201), SpectrumDomainId(2), 2, fx));
  expectExclusionRefusal(refused, ReservationId(1));
  WF_REQUIRE(refused.explanation.conflicts.size() == 1);
  WF_SAME(refused.explanation.conflicts.front().raw(), std::uint64_t{1});
  WF_SAME(refused.explanation.candidatesEnumerated, std::size_t{1});
  WF_SAME(refused.explanation.candidatesRejected, std::size_t{1});
  WF_SAME(refused.explanation.candidatesOmitted, std::size_t{0});
  WF_REQUIRE(refused.explanation.reasons.size() == 2);
  WF_SAME(refused.explanation.reasons.front(), std::string("an exclusion domain forbids the candidate"));

  // The refusal is deterministic and owns nothing.
  const AllocationDecision repeated = fx.runtime.allocate(flexRequest(AllocationRequestId(202), SpectrumDomainId(2), 2, fx));
  expectExclusionRefusal(repeated, ReservationId(1));
  WF_SAME(fx.runtime.reservations().size(), std::size_t{1});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{2});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{1});
  const std::optional<SpectrumUsage> siblingAfter = fx.runtime.usage(SpectrumDomainId(2), t0);
  WF_REQUIRE(siblingAfter.has_value());
  WF_SAME(siblingAfter->liveSlots, 0u);
  WF_SAME(siblingAfter->liveReservations, 0u);
  WF_REQUIRE(siblingAfter->freeRuns.size() == 1);
  WF_SAME(siblingAfter->freeRuns.front().first, 0u);
  WF_SAME(siblingAfter->freeRuns.front().count, 2u);

  // Releasing the owner's spectrum releases the sibling domain too.
  WF_SAME(codeOf(fx.runtime.release(ReservationId(1), ReservationGeneration(1), fx.release(), t0)), "ok");
  const AllocationDecision afterRelease = fx.runtime.allocate(flexRequest(AllocationRequestId(203), SpectrumDomainId(2), 2, fx));
  WF_REQUIRE(afterRelease.allocated());
  WF_SAME(afterRelease.reservation.raw(), std::uint64_t{2});
  WF_SAME(afterRelease.explanation.selected.slots.first, 0u);
  WF_SAME(afterRelease.explanation.selected.slots.count, 2u);

  // Mutual exclusion is symmetric: the first member is now shut out of the
  // spectrum the second member owns.
  const AllocationDecision mirrored = fx.runtime.allocate(flexRequest(AllocationRequestId(204), SpectrumDomainId(1), 2, fx));
  expectExclusionRefusal(mirrored, ReservationId(2));
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{2});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{3});
}

WF_TEST(same_domain_overlap_stays_a_conflict_and_own_spectrum_stays_usable) {
  Fixture fx;
  registerFlexGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  (void)fx.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  const ExclusionDomain pair = makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)});
  WF_SAME(registerCode(fx.runtime, pair), "ok");
  const AllocationDecision owner = fx.runtime.allocate(flexRequest(AllocationRequestId(210), SpectrumDomainId(1), 2, fx));
  WF_REQUIRE(owner.allocated());

  // Overlap inside the owning domain is a conflict, not an exclusion: the
  // blocking reservation is the domain's own.
  const AllocationDecision conflicted = fx.runtime.allocate(flexRequest(AllocationRequestId(211), SpectrumDomainId(1), 2, fx));
  WF_SAME(codeOf(conflicted.status), "conflict");
  WF_SAME(outcomeOf(conflicted.outcome), "refused-conflict");
  WF_REQUIRE(conflicted.explanation.conflicts.size() == 1);
  WF_SAME(conflicted.explanation.conflicts.front().raw(), std::uint64_t{1});
  WF_REQUIRE(conflicted.explanation.rejected.size() == 1);
  WF_SAME(eligibilityOf(conflicted.explanation.rejected.front().eligibility), "ineligible-conflict");
  WF_REQUIRE(!conflicted.explanation.reasons.empty());
  WF_SAME(conflicted.explanation.reasons.front(), std::string("the candidate is owned by a live reservation"));

  // With wide windows the members simply move past the spectrum their sibling
  // owns: exclusion is a constraint on overlap, not a prohibition on use.
  Fixture wide;
  registerFlexGrid(wide.runtime);
  (void)wide.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots);
  (void)wide.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots);
  WF_SAME(registerCode(wide.runtime, pair), "ok");
  const AllocationDecision wideOwner = wide.runtime.allocate(flexRequest(AllocationRequestId(212), SpectrumDomainId(1), 2, wide));
  WF_REQUIRE(wideOwner.allocated());
  WF_SAME(wideOwner.explanation.selected.slots.first, 0u);

  // The member's own earlier reservation is not an exclusion blocker for
  // itself, so its next placement continues immediately after it.
  const AllocationDecision ownNext = wide.runtime.allocate(flexRequest(AllocationRequestId(213), SpectrumDomainId(1), 2, wide));
  WF_REQUIRE(ownNext.allocated());
  WF_SAME(ownNext.explanation.selected.slots.first, 2u);
  WF_SAME(ownNext.explanation.selected.slots.count, 2u);

  // The sibling is pushed past both of them.
  const AllocationDecision sibling = wide.runtime.allocate(flexRequest(AllocationRequestId(215), SpectrumDomainId(2), 2, wide));
  WF_REQUIRE(sibling.allocated());
  WF_SAME(sibling.explanation.selected.slots.first, 4u);
  WF_SAME(sibling.explanation.selected.frequency.lowMhz, flexLow(4));
  WF_SAME(sibling.explanation.selected.frequency.highMhz, flexLow(6));

  // The first member continues after the sibling's range in turn.
  const AllocationDecision ownThird = wide.runtime.allocate(flexRequest(AllocationRequestId(216), SpectrumDomainId(1), 2, wide));
  WF_REQUIRE(ownThird.allocated());
  WF_SAME(ownThird.explanation.selected.slots.first, 6u);

  // Pinning the request onto the sibling's own range is where the exclusion is
  // observable as a refusal.
  const std::optional<SpectrumReservation> siblingStored = wide.runtime.reservation(sibling.reservation);
  WF_REQUIRE(siblingStored.has_value());
  const FrequencyRange siblingRange{siblingStored->frequency.lowMhz, siblingStored->frequency.highMhz};
  const AllocationDecision pinned = wide.runtime.allocate(pinnedRequest(AllocationRequestId(217), SpectrumDomainId(1), 2, wide, siblingRange));
  expectExclusionRefusal(pinned, sibling.reservation);
  WF_CHECK(!pinned.explanation.conflicts.empty());
  WF_CHECK(std::find(pinned.explanation.conflicts.begin(), pinned.explanation.conflicts.end(), ReservationId(1)) == pinned.explanation.conflicts.end());
}

// ---------------------------------------------------------------------------
// The exclusion guard band
// ---------------------------------------------------------------------------

WF_TEST(an_exclusion_guard_band_separates_members_exactly) {
  Fixture guarded;
  registerFlexGrid(guarded.runtime);
  (void)guarded.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots);
  (void)guarded.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots);
  const ExclusionDomain spaced = makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)}, 25'000);
  WF_SAME(registerCode(guarded.runtime, spaced), "ok");
  const AllocationDecision owner = guarded.runtime.allocate(flexRequest(AllocationRequestId(220), SpectrumDomainId(1), 2, guarded));
  WF_REQUIRE(owner.allocated());
  const std::optional<SpectrumReservation> ownerStored = guarded.runtime.reservation(owner.reservation);
  WF_REQUIRE(ownerStored.has_value());
  WF_SAME(ownerStored->frequency.lowMhz, flexLow(0));
  WF_SAME(ownerStored->frequency.highMhz, flexLow(2));

  // A placement inside the guard band is forbidden.
  const AllocationDecision insideGuard = guarded.runtime.allocate(pinnedRequest(AllocationRequestId(221), SpectrumDomainId(2), 2, guarded, FrequencyRange{flexLow(2), flexLow(4)}));
  expectExclusionRefusal(insideGuard, ReservationId(1));
  WF_SAME(insideGuard.explanation.candidatesEnumerated, std::size_t{1});
  WF_SAME(insideGuard.explanation.candidatesOmitted, std::size_t{0});

  // Exactly at the guard distance is allowed.
  const AllocationDecision atGuard = guarded.runtime.allocate(pinnedRequest(AllocationRequestId(222), SpectrumDomainId(2), 2, guarded, FrequencyRange{flexLow(4), flexLow(6)}));
  WF_REQUIRE(atGuard.allocated());
  WF_SAME(atGuard.explanation.selected.slots.first, 4u);
  const std::optional<SpectrumReservation> atGuardStored = guarded.runtime.reservation(atGuard.reservation);
  WF_REQUIRE(atGuardStored.has_value());
  WF_SAME(atGuardStored->frequency.lowMhz, flexLow(4));
  WF_SAME(atGuardStored->frequency.highMhz, flexLow(6));
  WF_SAME(atGuardStored->frequency.lowMhz - ownerStored->frequency.highMhz, std::int64_t{25'000});

  // The unrestricted placement is pushed past the guard band and past the
  // member's own reservation.
  const AllocationDecision sibling = guarded.runtime.allocate(flexRequest(AllocationRequestId(223), SpectrumDomainId(2), 2, guarded));
  WF_REQUIRE(sibling.allocated());
  WF_SAME(sibling.explanation.selected.slots.first, 6u);
  WF_SAME(sibling.explanation.selected.frequency.lowMhz, flexLow(6));

  // The same placement without any guard band is allowed and therefore adjacent.
  Fixture adjacent;
  registerFlexGrid(adjacent.runtime);
  (void)adjacent.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots);
  (void)adjacent.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots);
  const ExclusionDomain touching = makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)}, 0);
  WF_SAME(registerCode(adjacent.runtime, touching), "ok");
  const AllocationDecision adjacentOwner = adjacent.runtime.allocate(flexRequest(AllocationRequestId(224), SpectrumDomainId(1), 2, adjacent));
  WF_REQUIRE(adjacentOwner.allocated());
  const AllocationDecision adjacentSibling = adjacent.runtime.allocate(flexRequest(AllocationRequestId(225), SpectrumDomainId(2), 2, adjacent));
  WF_REQUIRE(adjacentSibling.allocated());
  WF_SAME(adjacentSibling.explanation.selected.slots.first, 2u);
  const std::optional<SpectrumReservation> adjacentOwnerStored = adjacent.runtime.reservation(adjacentOwner.reservation);
  const std::optional<SpectrumReservation> adjacentSiblingStored = adjacent.runtime.reservation(adjacentSibling.reservation);
  WF_REQUIRE(adjacentOwnerStored.has_value());
  WF_REQUIRE(adjacentSiblingStored.has_value());
  WF_SAME(adjacentSiblingStored->frequency.lowMhz, adjacentOwnerStored->frequency.highMhz);
}

// ---------------------------------------------------------------------------
// Membership bounds and validation
// ---------------------------------------------------------------------------

WF_TEST(exclusion_membership_is_bounded_per_member_at_eight) {
  Fixture fx;
  registerFlexGrid(fx.runtime);
  for (std::uint64_t index = 1; index <= 4; ++index) {
    addFlexDomain(fx.runtime, index, kFlexSlots);
  }

  for (std::uint64_t index = 1; index <= 8; ++index) {
    const ExclusionDomain linked = makeExclusion(ExclusionDomainId(index), {SpectrumDomainId(1), SpectrumDomainId(2)});
    WF_SAME(registerCode(fx.runtime, linked), "ok");
  }
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{8});
  for (std::uint64_t index = 1; index <= 8; ++index) {
    WF_CHECK(fx.runtime.exclusionDomain(ExclusionDomainId(index)).has_value());
  }

  // A ninth exclusion domain for an already saturated member is refused, and
  // the refusal registers nothing.
  const Status saturated = fx.runtime.registerExclusionDomain(makeExclusion(ExclusionDomainId(9), {SpectrumDomainId(1)}));
  WF_SAME(codeOf(saturated), "limit-exceeded");
  WF_CHECK(saturated.message.find("already belongs to 8 exclusion domains") != std::string::npos);
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{8});
  WF_CHECK(!fx.runtime.exclusionDomain(ExclusionDomainId(9)).has_value());
  const ExclusionDomain forSecond = makeExclusion(ExclusionDomainId(10), {SpectrumDomainId(2)});
  WF_SAME(registerCode(fx.runtime, forSecond), "limit-exceeded");
  // Any member at the bound refuses the registration, even though the other
  // member of the same exclusion domain would still have room.
  const ExclusionDomain mixed = makeExclusion(ExclusionDomainId(11), {SpectrumDomainId(1), SpectrumDomainId(3)});
  WF_SAME(registerCode(fx.runtime, mixed), "limit-exceeded");
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{8});

  // A member with room is unaffected by its neighbour's bound.
  const ExclusionDomain freeMember = makeExclusion(ExclusionDomainId(12), {SpectrumDomainId(3)});
  WF_SAME(registerCode(fx.runtime, freeMember), "ok");
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{9});
  WF_REQUIRE(fx.runtime.exclusionDomain(ExclusionDomainId(12)).has_value());
  WF_SAME(fx.runtime.exclusionDomain(ExclusionDomainId(12))->members.size(), std::size_t{1});

  // Eight simultaneous membership links still bind end to end.
  Fixture shared;
  registerFlexGrid(shared.runtime);
  addFlexDomain(shared.runtime, 1, 1);
  addFlexDomain(shared.runtime, 2, 1);
  for (std::uint64_t index = 1; index <= 8; ++index) {
    const ExclusionDomain linked = makeExclusion(ExclusionDomainId(index), {SpectrumDomainId(1), SpectrumDomainId(2)});
    WF_SAME(registerCode(shared.runtime, linked), "ok");
  }
  const AllocationDecision owner = shared.runtime.allocate(flexRequest(AllocationRequestId(230), SpectrumDomainId(2), 1, shared));
  WF_REQUIRE(owner.allocated());
  WF_SAME(owner.explanation.selected.slots.first, 0u);
  const AllocationDecision blocked = shared.runtime.allocate(flexRequest(AllocationRequestId(231), SpectrumDomainId(1), 1, shared));
  expectExclusionRefusal(blocked, owner.reservation);
  WF_REQUIRE(blocked.explanation.conflicts.size() == 1);
  WF_SAME(blocked.explanation.conflicts.front().raw(), std::uint64_t{1});

  // Two distinct exclusion domains contribute two distinct blockers, reported in
  // ascending identity order.
  Fixture twoLinks;
  registerFlexGrid(twoLinks.runtime);
  addFlexDomain(twoLinks.runtime, 1, 1);
  addFlexDomain(twoLinks.runtime, 2, 1);
  addFlexDomain(twoLinks.runtime, 3, 1);
  WF_SAME(registerCode(twoLinks.runtime, makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)})), "ok");
  WF_SAME(registerCode(twoLinks.runtime, makeExclusion(ExclusionDomainId(2), {SpectrumDomainId(1), SpectrumDomainId(3)})), "ok");
  const AllocationDecision second = twoLinks.runtime.allocate(flexRequest(AllocationRequestId(232), SpectrumDomainId(2), 1, twoLinks));
  WF_REQUIRE(second.allocated());
  WF_SAME(second.reservation.raw(), std::uint64_t{1});
  const AllocationDecision third = twoLinks.runtime.allocate(flexRequest(AllocationRequestId(233), SpectrumDomainId(3), 1, twoLinks));
  WF_REQUIRE(third.allocated());
  WF_SAME(third.reservation.raw(), std::uint64_t{2});
  const AllocationDecision doublyBlocked = twoLinks.runtime.allocate(flexRequest(AllocationRequestId(234), SpectrumDomainId(1), 1, twoLinks));
  expectExclusionRefusal(doublyBlocked, ReservationId(2));
  WF_REQUIRE(doublyBlocked.explanation.conflicts.size() == 2);
  WF_SAME(doublyBlocked.explanation.conflicts[0].raw(), std::uint64_t{1});
  WF_SAME(doublyBlocked.explanation.conflicts[1].raw(), std::uint64_t{2});
}

WF_TEST(exclusion_membership_validation_and_reregistration) {
  Fixture fx;
  registerFlexGrid(fx.runtime);
  for (std::uint64_t index = 1; index <= 4; ++index) {
    (void)fx.addDomain(SpectrumDomainId(index), flexGridId, kFlexSlots);
  }

  const SpectrumDomainId one{1};
  const SpectrumDomainId two{2};
  const SpectrumDomainId three{3};

  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(0), {one})), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {one}, 0, ExclusionDomainGeneration(0))), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {})), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(0)})), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {two, one})), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {one, one})), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {one}, -1)), "invalid-argument");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {one}, kMaxGuardBandMhz + 1)), "invalid-argument");
  ExclusionDomain longLabel = makeExclusion(ExclusionDomainId(1), {one});
  longLabel.label.assign(300, 'x');
  WF_SAME(registerCode(fx.runtime, longLabel), "invalid-argument");
  // An unregistered member is a not-found, not a validation error.
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(9)})), "not-found");
  // Too many members.
  ExclusionDomain huge = makeExclusion(ExclusionDomainId(1), {});
  for (std::uint64_t index = 1; index <= kMaxExclusionDomainMembers + 1; ++index) {
    huge.members.push_back(SpectrumDomainId(index));
  }
  WF_SAME(registerCode(fx.runtime, huge), "invalid-argument");
  WF_CHECK(fx.runtime.exclusionDomains().empty());

  // A valid registration, then the same generation again, then an older one.
  const ExclusionDomain allThree = makeExclusion(ExclusionDomainId(1), {one, two, three});
  WF_SAME(registerCode(fx.runtime, allThree), "ok");
  WF_REQUIRE(fx.runtime.exclusionDomain(ExclusionDomainId(1)).has_value());
  const ExclusionDomain stored = *fx.runtime.exclusionDomain(ExclusionDomainId(1));
  WF_SAME(stored.members.size(), std::size_t{3});
  WF_CHECK(exclusionDomainContains(stored, one));
  WF_CHECK(exclusionDomainContains(stored, two));
  WF_CHECK(exclusionDomainContains(stored, three));
  WF_CHECK(!exclusionDomainContains(stored, SpectrumDomainId(4)));
  WF_CHECK(!exclusionDomainContains(stored, SpectrumDomainId(0)));
  WF_SAME(registerCode(fx.runtime, allThree), "duplicate");
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {one}, 0, ExclusionDomainGeneration(0))), "invalid-argument");
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{1});

  // The constraint binds on every member: pinning a request onto the owner's
  // own range makes the exclusion observable as a refusal.
  const AllocationDecision owner = fx.runtime.allocate(flexRequest(AllocationRequestId(240), SpectrumDomainId(3), 2, fx));
  WF_REQUIRE(owner.allocated());
  const std::optional<SpectrumReservation> ownerStored = fx.runtime.reservation(owner.reservation);
  WF_REQUIRE(ownerStored.has_value());
  const FrequencyRange ownerRange{ownerStored->frequency.lowMhz, ownerStored->frequency.highMhz};
  const AllocationDecision onOne = fx.runtime.allocate(pinnedRequest(AllocationRequestId(241), one, 2, fx, ownerRange));
  expectExclusionRefusal(onOne, owner.reservation);
  const AllocationDecision onTwo = fx.runtime.allocate(pinnedRequest(AllocationRequestId(242), two, 2, fx, ownerRange));
  expectExclusionRefusal(onTwo, owner.reservation);

  // Re-registering at a higher generation replaces the membership exactly.
  const ExclusionDomain shrunk = makeExclusion(ExclusionDomainId(1), {one, three}, 0, ExclusionDomainGeneration(2));
  WF_SAME(registerCode(fx.runtime, shrunk), "ok");
  WF_REQUIRE(fx.runtime.exclusionDomain(ExclusionDomainId(1)).has_value());
  WF_SAME(fx.runtime.exclusionDomain(ExclusionDomainId(1))->generation.raw(), std::uint64_t{2});
  WF_SAME(fx.runtime.exclusionDomain(ExclusionDomainId(1))->members.size(), std::size_t{2});
  WF_SAME(fx.runtime.exclusionDomains().size(), std::size_t{1});
  WF_CHECK(!exclusionDomainContains(*fx.runtime.exclusionDomain(ExclusionDomainId(1)), two));

  // The member that was removed is free to own the same spectrum.
  const AllocationDecision releasedMember = fx.runtime.allocate(pinnedRequest(AllocationRequestId(250), two, 2, fx, ownerRange));
  WF_REQUIRE(releasedMember.allocated());
  WF_SAME(releasedMember.explanation.selected.slots.first, 0u);
  const std::optional<SpectrumReservation> releasedStored = fx.runtime.reservation(releasedMember.reservation);
  WF_REQUIRE(releasedStored.has_value());
  WF_CHECK(releasedStored->frequency.overlaps(ownerStored->frequency));
  // The members that are still listed are still bound.
  const AllocationDecision stillBound = fx.runtime.allocate(pinnedRequest(AllocationRequestId(251), one, 2, fx, ownerRange));
  expectExclusionRefusal(stillBound, owner.reservation);
}

WF_TEST(members_that_are_not_registered_are_refused_and_do_not_leak) {
  Fixture fx;
  registerFlexGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  (void)fx.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  (void)fx.addDomain(SpectrumDomainId(3), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);

  // An exclusion domain may only name registered domains.
  WF_SAME(registerCode(fx.runtime, makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(4)})), "not-found");
  WF_CHECK(fx.runtime.exclusionDomains().empty());
  WF_CHECK(!fx.runtime.domain(SpectrumDomainId(4)).has_value());

  const ExclusionDomain pair = makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)});
  WF_SAME(registerCode(fx.runtime, pair), "ok");

  // A domain registered after the exclusion domain exists is not a member.
  const SpectrumDomainId late{5};
  (void)fx.addDomain(late, flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  WF_REQUIRE(fx.runtime.exclusionDomain(ExclusionDomainId(1)).has_value());
  WF_SAME(fx.runtime.exclusionDomain(ExclusionDomainId(1))->members.size(), std::size_t{2});
  WF_CHECK(!exclusionDomainContains(*fx.runtime.exclusionDomain(ExclusionDomainId(1)), late));

  const AllocationDecision owner = fx.runtime.allocate(flexRequest(AllocationRequestId(260), SpectrumDomainId(1), 2, fx));
  WF_REQUIRE(owner.allocated());
  const std::optional<SpectrumReservation> ownerStored = fx.runtime.reservation(owner.reservation);
  WF_REQUIRE(ownerStored.has_value());

  // The member is refused, the non-member is not: the constraint never leaks
  // outside the member list.
  const AllocationDecision member = fx.runtime.allocate(flexRequest(AllocationRequestId(261), SpectrumDomainId(2), 2, fx));
  expectExclusionRefusal(member, owner.reservation);

  const AllocationDecision outsider = fx.runtime.allocate(flexRequest(AllocationRequestId(262), late, 2, fx));
  WF_REQUIRE(outsider.allocated());
  WF_SAME(outsider.explanation.selected.slots.first, ownerStored->slots.first);
  WF_SAME(outsider.explanation.selected.slots.count, ownerStored->slots.count);
  WF_SAME(outsider.explanation.selected.frequency.lowMhz, ownerStored->frequency.lowMhz);
  WF_SAME(outsider.explanation.selected.frequency.highMhz, ownerStored->frequency.highMhz);
  const std::optional<SpectrumReservation> outsiderStored = fx.runtime.reservation(outsider.reservation);
  WF_REQUIRE(outsiderStored.has_value());
  WF_CHECK(outsiderStored->slots.overlaps(ownerStored->slots));
  WF_CHECK(outsiderStored->frequency.overlaps(ownerStored->frequency));

  // Domain 3 is registered but is not a member either.
  const AllocationDecision alsoOutside = fx.runtime.allocate(flexRequest(AllocationRequestId(263), SpectrumDomainId(3), 2, fx));
  WF_REQUIRE(alsoOutside.allocated());
  WF_SAME(alsoOutside.explanation.selected.slots.first, 0u);
  const std::optional<SpectrumUsage> outsiderUsage = fx.runtime.usage(late, t0);
  WF_REQUIRE(outsiderUsage.has_value());
  WF_SAME(outsiderUsage->liveSlots, 2u);
  WF_SAME(outsiderUsage->liveReservations, 1u);
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{3});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});
}

WF_TEST(an_exclusion_domain_registered_later_binds_the_spectrum_immediately) {
  Fixture fx;
  registerFlexGrid(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);
  (void)fx.addDomain(SpectrumDomainId(2), flexGridId, kFlexSlots, SpectrumDomainGeneration(1), flexGridGen, 2);

  // The reservation exists before the exclusion domain that constrains it.
  const AllocationDecision owner = fx.runtime.allocate(flexRequest(AllocationRequestId(270), SpectrumDomainId(2), 2, fx));
  WF_REQUIRE(owner.allocated());
  const ExclusionDomain pair = makeExclusion(ExclusionDomainId(1), {SpectrumDomainId(1), SpectrumDomainId(2)});
  WF_SAME(registerCode(fx.runtime, pair), "ok");

  const AllocationDecision refused = fx.runtime.allocate(flexRequest(AllocationRequestId(271), SpectrumDomainId(1), 2, fx));
  expectExclusionRefusal(refused, owner.reservation);
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});

  // The constraint stops binding the moment the owner releases.
  WF_SAME(codeOf(fx.runtime.release(owner.reservation, ReservationGeneration(1), fx.release(), t0)), "ok");
  const AllocationDecision afterRelease = fx.runtime.allocate(flexRequest(AllocationRequestId(272), SpectrumDomainId(1), 2, fx));
  WF_REQUIRE(afterRelease.allocated());
  WF_SAME(afterRelease.explanation.selected.slots.first, 0u);
  WF_REQUIRE(fx.runtime.reservation(owner.reservation).has_value());
  WF_SAME(stateOf(fx.runtime.reservation(owner.reservation)->state), "released");
}

WF_TEST_MAIN()
