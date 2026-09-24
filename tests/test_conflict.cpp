// Conflict tests: the ownership invariant that two live reservations never own
// the same spectrum on the same domain, adjacency, guard-band separation,
// blocking identity reporting, exact release and reclamation, and a seeded
// deterministic allocate/release sequence that re-checks the invariant after
// every single step.

#include "test_common.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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
constexpr std::int64_t kFixedSlotMhz = 50'000;

const Instant t0 = Instant::fromSeconds(1'800'000'000);
const Duration oneHour = Duration::hours(1);
const Duration hundredSeconds = Duration::seconds(100);

const ChannelGridId fixedGridId{ChannelGridId(1)};
const GridGeneration fixedGridGen{GridGeneration(1)};
const ChannelGridId flexGridId{ChannelGridId(2)};
const GridGeneration flexGridGen{GridGeneration(1)};

constexpr std::uint32_t kFixedSlots = 96;
constexpr std::uint32_t kSmallWindow = 24;

[[nodiscard]] std::string codeOf(const Status& status) { return std::string(toToken(status.code)); }

[[nodiscard]] std::string outcomeOf(AllocationOutcome outcome) {
  return std::string(toToken(outcome));
}

[[nodiscard]] std::string stateOf(ReservationState state) { return std::string(toToken(state)); }

[[nodiscard]] bool mentions(const std::vector<std::string>& reasons, std::string_view needle) {
  for (const std::string& reason : reasons) {
    if (reason.find(needle) != std::string::npos) return true;
  }
  return false;
}

[[nodiscard]] std::int64_t fixedLow(std::uint32_t slot) {
  return kAnchorMhz + kFixedSlotMhz * static_cast<std::int64_t>(slot);
}

// The "fixed" identifier carries a flex grid with exactly the same 50 GHz
// arithmetic: a fixed grid expresses exactly one slot per channel, so every
// multi-slot channel in this file lives on the flex grid.
void registerGrids(SpectrumRuntime& runtime) {
  (void)runtime.registerGrid(flexGrid(fixedGridId, fixedGridGen, kAnchorMhz, kFixedSlotMhz, 96, 1, 32));
  (void)runtime.registerGrid(flexGrid(flexGridId, flexGridGen));
}

// A reservation owns spectrum only while its state is live and its lease has
// not lapsed. Everything below is expressed in exactly those terms.
[[nodiscard]] bool ownsSpectrumAt(const SpectrumReservation& reservation, Instant now) {
  return isLiveState(reservation.state) && reservation.lease.validAt(now);
}

// Slot range this reservation owns on the domain at the given index of its own
// domain list.
[[nodiscard]] SlotRange ownedRange(const SpectrumReservation& reservation, std::size_t index) {
  if (index < reservation.perDomainSlots.size() && !reservation.perDomainSlots[index].empty()) {
    return reservation.perDomainSlots[index];
  }
  return reservation.slots;
}

[[nodiscard]] SlotRange ownedRangeOn(const SpectrumReservation& reservation, SpectrumDomainId domain) {
  for (std::size_t index = 0; index < reservation.domains.size(); ++index) {
    if (reservation.domains[index] == domain) return ownedRange(reservation, index);
  }
  return SlotRange{};
}

[[nodiscard]] FrequencyRange ownedFrequencyOn(const SpectrumReservation& reservation,
                                              SpectrumDomainId domain) {
  if (ownedRangeOn(reservation, domain).empty()) return FrequencyRange{};
  return reservation.frequency;
}

std::vector<SlotRange> mergedRanges(std::vector<SlotRange> ranges) {
  std::sort(ranges.begin(), ranges.end(), [](const SlotRange& a, const SlotRange& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.count < b.count;
  });
  std::vector<SlotRange> merged;
  for (const SlotRange& range : ranges) {
    if (range.empty()) continue;
    if (!merged.empty() && range.first <= merged.back().end()) {
      const std::uint32_t end = merged.back().end() > range.end() ? merged.back().end() : range.end();
      merged.back().count = end - merged.back().first;
    } else {
      merged.push_back(range);
    }
  }
  return merged;
}

// The core ownership invariant: no two reservations that own spectrum at the
// same instant may own overlapping spectrum on a domain they both span.
void expectOwnershipInvariant(const SpectrumRuntime& runtime, Instant now) {
  const std::vector<SpectrumReservation> reservations = runtime.reservations();
  for (std::size_t left = 0; left < reservations.size(); ++left) {
    for (std::size_t right = left + 1; right < reservations.size(); ++right) {
      const SpectrumReservation& a = reservations[left];
      const SpectrumReservation& b = reservations[right];
      if (!ownsSpectrumAt(a, now) || !ownsSpectrumAt(b, now)) continue;
      for (std::size_t index = 0; index < a.domains.size(); ++index) {
        const SpectrumDomainId shared = a.domains[index];
        if (std::find(b.domains.begin(), b.domains.end(), shared) == b.domains.end()) continue;
        const SlotRange leftRange = ownedRange(a, index);
        const SlotRange rightRange = ownedRangeOn(b, shared);
        WF_CHECK(!leftRange.overlaps(rightRange));
        WF_CHECK(!ownedFrequencyOn(a, shared).overlaps(ownedFrequencyOn(b, shared)));
      }
    }
  }
}

// usage() must be the exact complement of what the live reservations own on the
// domain, in the capability's allocatable window.
void expectFreeRunAccounting(const SpectrumRuntime& runtime, SpectrumDomainId domain, Instant now) {
  const std::optional<SpectrumUsage> usage = runtime.usage(domain, now);
  WF_REQUIRE(usage.has_value());
  const std::optional<SpectrumCapability> capability = runtime.capability(domain);
  WF_REQUIRE(capability.has_value());
  const SlotRange window = allocatableWindow(*capability);
  WF_SAME(usage->allocatableSlots, window.count);

  std::vector<SlotRange> live;
  std::uint32_t liveReservations = 0;
  for (const SpectrumReservation& reservation : runtime.reservations()) {
    if (!ownsSpectrumAt(reservation, now)) continue;
    const SlotRange range = ownedRangeOn(reservation, domain);
    if (range.empty()) continue;
    live.push_back(range);
    ++liveReservations;
  }
  const std::vector<SlotRange> merged = mergedRanges(live);
  std::uint32_t covered = 0;
  for (const SlotRange& range : merged) covered += range.count;
  WF_SAME(usage->liveSlots, covered);
  WF_SAME(usage->liveReservations, liveReservations);
  WF_SAME(usage->freeSlots, window.count - covered);

  std::vector<SlotRange> free{window};
  for (const SlotRange& cut : merged) {
    std::vector<SlotRange> next;
    for (const SlotRange& piece : free) {
      if (!piece.overlaps(cut)) {
        next.push_back(piece);
        continue;
      }
      if (cut.first > piece.first) next.push_back(SlotRange{piece.first, cut.first - piece.first});
      if (cut.end() < piece.end()) next.push_back(SlotRange{cut.end(), piece.end() - cut.end()});
    }
    free.swap(next);
  }
  WF_SAME(usage->freeRuns.size(), free.size());
  for (std::size_t index = 0; index < free.size() && index < usage->freeRuns.size(); ++index) {
    WF_SAME(usage->freeRuns[index].first, free[index].first);
    WF_SAME(usage->freeRuns[index].count, free[index].count);
  }
}

// Authority tokens minted by the runtime under test: a token built from another
// runtime's fence would be stale by construction.
[[nodiscard]] EligibilityAuthority eligibilityToken(const SpectrumRuntime& runtime) {
  EligibilityAuthority token;
  token.generation = runtime.authorityState().eligibilityGeneration;
  token.fence = runtime.fence();
  return token;
}

[[nodiscard]] ReservationAuthority reservationToken(const SpectrumRuntime& runtime) {
  ReservationAuthority token;
  token.generation = runtime.authorityState().reservationGeneration;
  token.fence = runtime.fence();
  return token;
}

[[nodiscard]] ReleaseAuthority releaseToken(const SpectrumRuntime& runtime) {
  ReleaseAuthority token;
  token.generation = runtime.authorityState().releaseGeneration;
  token.fence = runtime.fence();
  return token;
}

SpectrumRequest request(AllocationRequestId requestId, SpectrumDomainId domain, ChannelGridId grid,
                        GridGeneration gridGeneration, std::uint32_t slots, Instant requestedAt,
                        Duration lease, const Fixture& fixture) {
  return makeRequest(requestId, OwnerId(7), {domain}, {SpectrumDomainGeneration(1)}, grid,
                     gridGeneration, slots, requestedAt, lease, fixture.eligibility(),
                     fixture.reservation(), ContiguityRequirement::Required,
                     ContinuityRequirement::Unspecified);
}

}  // namespace

// ---------------------------------------------------------------------------
// Adjacency and guard bands
// ---------------------------------------------------------------------------

WF_TEST(adjacent_reservations_are_allowed_and_fill_first_fit) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, kFixedSlots);

  ReservationId ids[3] = {ReservationId(1), ReservationId(2), ReservationId(3)};
  for (std::uint32_t index = 0; index < 3; ++index) {
    const AllocationDecision decision =
        fx.runtime.allocate(request(AllocationRequestId(100 + index), SpectrumDomainId(1), fixedGridId,
                                    fixedGridGen, 2, t0, oneHour, fx));
    WF_REQUIRE(decision.allocated());
    WF_SAME(decision.reservation.raw(), ids[index].raw());
    WF_SAME(decision.explanation.selected.slots.first, 2u * index);
    WF_SAME(decision.explanation.selected.slots.count, 2u);
    expectOwnershipInvariant(fx.runtime, t0);
    expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), t0);
  }

  const std::optional<SpectrumReservation> first = fx.runtime.reservation(ReservationId(1));
  const std::optional<SpectrumReservation> second = fx.runtime.reservation(ReservationId(2));
  const std::optional<SpectrumReservation> third = fx.runtime.reservation(ReservationId(3));
  WF_REQUIRE(first.has_value() && second.has_value() && third.has_value());
  // Touching but not overlapping: adjacency is allowed.
  WF_CHECK(first->slots.touches(second->slots));
  WF_CHECK(!first->slots.overlaps(second->slots));
  WF_CHECK(second->slots.touches(third->slots));
  WF_CHECK(!second->slots.overlaps(third->slots));
  WF_CHECK(!first->slots.overlaps(third->slots));
  WF_SAME(first->slots.end(), second->slots.first);
  WF_SAME(second->slots.end(), third->slots.first);
  WF_SAME(first->frequency.highMhz, second->frequency.lowMhz);
  WF_SAME(second->frequency.highMhz, third->frequency.lowMhz);
  WF_SAME(first->frequency.lowMhz, fixedLow(0));
  WF_SAME(third->frequency.highMhz, fixedLow(6));
  WF_SAME(second->frequency.lowMhz, fixedLow(2));
  WF_SAME(second->frequency.highMhz, fixedLow(4));

  const std::optional<SpectrumUsage> usage = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(usage.has_value());
  WF_SAME(usage->liveSlots, 6u);
  WF_SAME(usage->liveReservations, 3u);
  WF_SAME(usage->freeSlots, 90u);
  WF_REQUIRE(usage->freeRuns.size() == 1);
  WF_SAME(usage->freeRuns.front().first, 6u);
  WF_SAME(usage->freeRuns.front().count, 90u);
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{3});
}

WF_TEST(guard_bands_separate_neighbours_by_exactly_the_guard) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, kFixedSlots);

  SpectrumRequest guardedFirst =
      request(AllocationRequestId(110), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  guardedFirst.guardBandMhz = kFixedSlotMhz;
  const AllocationDecision first = fx.runtime.allocate(guardedFirst);
  WF_REQUIRE(first.allocated());
  WF_SAME(first.explanation.selected.slots.first, 0u);
  const std::optional<SpectrumReservation> firstStored = fx.runtime.reservation(ReservationId(1));
  WF_REQUIRE(firstStored.has_value());
  WF_SAME(firstStored->guardBandMhz, kFixedSlotMhz);
  WF_SAME(firstStored->frequency.lowMhz, fixedLow(0));
  WF_SAME(firstStored->frequency.highMhz, fixedLow(1));

  // The committed reservation's own guard band keeps the next neighbour away
  // even though that request asks for no separation at all.
  const AllocationDecision plain = fx.runtime.allocate(request(AllocationRequestId(111), SpectrumDomainId(1),
                                                              fixedGridId, fixedGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(plain.allocated());
  WF_SAME(plain.explanation.selected.slots.first, 2u);
  const std::optional<SpectrumReservation> plainStored = fx.runtime.reservation(ReservationId(2));
  WF_REQUIRE(plainStored.has_value());
  WF_SAME(plainStored->guardBandMhz, std::int64_t{0});
  WF_SAME(plainStored->frequency.lowMhz, fixedLow(2));
  WF_SAME(plainStored->frequency.lowMhz - firstStored->frequency.highMhz, kFixedSlotMhz);

  // A guarded request keeps the same distance from both neighbours.
  SpectrumRequest guardedThird =
      request(AllocationRequestId(112), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour, fx);
  guardedThird.guardBandMhz = kFixedSlotMhz;
  const AllocationDecision third = fx.runtime.allocate(guardedThird);
  WF_REQUIRE(third.allocated());
  WF_SAME(third.explanation.selected.slots.first, 4u);
  const std::optional<SpectrumReservation> thirdStored = fx.runtime.reservation(ReservationId(3));
  WF_REQUIRE(thirdStored.has_value());
  WF_SAME(thirdStored->frequency.lowMhz - plainStored->frequency.highMhz, kFixedSlotMhz);
  WF_SAME(plainStored->frequency.lowMhz - firstStored->frequency.highMhz, kFixedSlotMhz);

  // The guard slots are not free even though no reservation owns them.
  const std::optional<SpectrumUsage> usage = fx.runtime.usage(SpectrumDomainId(1), t0);
  WF_REQUIRE(usage.has_value());
  WF_SAME(usage->liveSlots, 3u);
  WF_REQUIRE(usage->freeRuns.size() == 3);
  WF_SAME(usage->freeRuns[0].first, 1u);
  WF_SAME(usage->freeRuns[0].count, 1u);
  WF_SAME(usage->freeRuns[1].first, 3u);
  WF_SAME(usage->freeRuns[1].count, 1u);
  WF_SAME(usage->freeRuns[2].first, 5u);
  WF_SAME(usage->freeRuns[2].count, 91u);
  expectOwnershipInvariant(fx.runtime, t0);

  // A guard band stated only by the request separates it from an unguarded
  // neighbour in exactly the same way.
  Fixture requestOnly;
  registerGrids(requestOnly.runtime);
  (void)requestOnly.addDomain(SpectrumDomainId(1), fixedGridId, kFixedSlots);
  const AllocationDecision plainFirst = requestOnly.runtime.allocate(request(
      AllocationRequestId(113), SpectrumDomainId(1), fixedGridId, fixedGridGen, 1, t0, oneHour, requestOnly));
  WF_REQUIRE(plainFirst.allocated());
  SpectrumRequest guardedSecond = request(AllocationRequestId(114), SpectrumDomainId(1), fixedGridId,
                                          fixedGridGen, 1, t0, oneHour, requestOnly);
  guardedSecond.guardBandMhz = kFixedSlotMhz;
  const AllocationDecision requestGuarded = requestOnly.runtime.allocate(guardedSecond);
  WF_REQUIRE(requestGuarded.allocated());
  WF_SAME(requestGuarded.explanation.selected.slots.first, 2u);
  const std::optional<SpectrumReservation> guardedStored =
      requestOnly.runtime.reservation(requestGuarded.reservation);
  const std::optional<SpectrumReservation> plainFirstStored =
      requestOnly.runtime.reservation(plainFirst.reservation);
  WF_REQUIRE(guardedStored.has_value() && plainFirstStored.has_value());
  WF_SAME(guardedStored->frequency.lowMhz - plainFirstStored->frequency.highMhz, kFixedSlotMhz);
  expectOwnershipInvariant(requestOnly.runtime, t0);
  expectFreeRunAccounting(requestOnly.runtime, SpectrumDomainId(1), t0);
}

// ---------------------------------------------------------------------------
// Blocking identities
// ---------------------------------------------------------------------------

WF_TEST(a_conflict_reports_every_blocking_reservation_identity) {
  Fixture fx;
  registerGrids(fx.runtime);
  // Two allocatable flex slots, each owned by a one-slot reservation.
  (void)fx.addDomain(SpectrumDomainId(1), flexGridId, kFixedSlots, SpectrumDomainGeneration(1), flexGridGen,
                     2);
  // A second domain holds an unrelated reservation that must never be reported.
  (void)fx.addDomain(SpectrumDomainId(2), flexGridId, kFixedSlots);
  const AllocationDecision unrelated = fx.runtime.allocate(request(
      AllocationRequestId(120), SpectrumDomainId(2), flexGridId, flexGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(unrelated.allocated());
  WF_SAME(unrelated.reservation.raw(), std::uint64_t{1});

  const AllocationDecision fillOne = fx.runtime.allocate(request(
      AllocationRequestId(121), SpectrumDomainId(1), flexGridId, flexGridGen, 1, t0, oneHour, fx));
  const AllocationDecision fillTwo = fx.runtime.allocate(request(
      AllocationRequestId(122), SpectrumDomainId(1), flexGridId, flexGridGen, 1, t0, oneHour, fx));
  WF_REQUIRE(fillOne.allocated() && fillTwo.allocated());
  WF_SAME(fillOne.reservation.raw(), std::uint64_t{2});
  WF_SAME(fillTwo.reservation.raw(), std::uint64_t{3});
  WF_SAME(fillOne.explanation.selected.slots.first, 0u);
  WF_SAME(fillTwo.explanation.selected.slots.first, 1u);

  const AllocationDecision refused = fx.runtime.allocate(request(
      AllocationRequestId(123), SpectrumDomainId(1), flexGridId, flexGridGen, 2, t0, oneHour, fx));
  WF_SAME(codeOf(refused.status), "conflict");
  WF_SAME(outcomeOf(refused.outcome), "refused-conflict");
  WF_CHECK(!refused.allocated());
  WF_SAME(refused.reservation.raw(), std::uint64_t{0});
  WF_REQUIRE(refused.explanation.conflicts.size() == 2);
  WF_SAME(refused.explanation.conflicts[0].raw(), std::uint64_t{2});
  WF_SAME(refused.explanation.conflicts[1].raw(), std::uint64_t{3});
  WF_SAME(refused.explanation.candidatesEnumerated, std::size_t{1});
  WF_SAME(refused.explanation.candidatesRejected, std::size_t{1});
  WF_SAME(refused.explanation.candidatesOmitted, std::size_t{0});
  WF_REQUIRE(refused.explanation.rejected.size() == 1);
  WF_SAME(std::string(toToken(refused.explanation.rejected.front().eligibility)), "ineligible-conflict");
  WF_REQUIRE(refused.explanation.rejected.front().conflicts.size() == 2);
  WF_SAME(refused.explanation.rejected.front().conflicts[0].raw(), std::uint64_t{2});
  WF_SAME(refused.explanation.rejected.front().conflicts[1].raw(), std::uint64_t{3});
  WF_REQUIRE(!refused.explanation.reasons.empty());
  WF_SAME(refused.explanation.reasons.front(), std::string("the candidate is owned by a live reservation"));
  // The unrelated live reservation on the other domain is not a blocker.
  WF_CHECK(std::find(refused.explanation.conflicts.begin(), refused.explanation.conflicts.end(),
                     ReservationId(1)) == refused.explanation.conflicts.end());
  // The refusal changed nothing.
  WF_SAME(fx.runtime.reservations().size(), std::size_t{3});
  WF_SAME(stateOf(fx.runtime.reservation(ReservationId(2))->state), "reserved");
  WF_SAME(stateOf(fx.runtime.reservation(ReservationId(3))->state), "reserved");
  WF_SAME(fx.runtime.stats().allocationsRefused, std::uint64_t{1});
  expectOwnershipInvariant(fx.runtime, t0);
  expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), t0);

  // A single blocker is reported alone.
  Fixture narrow;
  registerGrids(narrow.runtime);
  (void)narrow.addDomain(SpectrumDomainId(1), flexGridId, kFixedSlots, SpectrumDomainGeneration(1),
                         flexGridGen, 2);
  const AllocationDecision wide = narrow.runtime.allocate(request(
      AllocationRequestId(124), SpectrumDomainId(1), flexGridId, flexGridGen, 2, t0, oneHour, narrow));
  WF_REQUIRE(wide.allocated());
  const AllocationDecision single = narrow.runtime.allocate(request(
      AllocationRequestId(125), SpectrumDomainId(1), flexGridId, flexGridGen, 1, t0, oneHour, narrow));
  WF_SAME(codeOf(single.status), "conflict");
  WF_SAME(outcomeOf(single.outcome), "refused-conflict");
  WF_REQUIRE(single.explanation.conflicts.size() == 1);
  WF_SAME(single.explanation.conflicts.front().raw(), std::uint64_t{1});
  WF_REQUIRE(single.explanation.rejected.size() == 1);
  WF_SAME(std::string(toToken(single.explanation.rejected.front().eligibility)), "ineligible-conflict");
}

// ---------------------------------------------------------------------------
// Lapsed leases, release and reclamation
// ---------------------------------------------------------------------------

WF_TEST(a_lapsed_lease_owns_nothing_until_it_is_reclaimed) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, kFixedSlots);

  const AllocationDecision shortLived =
      fx.runtime.allocate(request(AllocationRequestId(130), SpectrumDomainId(1), fixedGridId, fixedGridGen,
                                  2, t0, hundredSeconds, fx));
  const AllocationDecision longLived =
      fx.runtime.allocate(request(AllocationRequestId(131), SpectrumDomainId(1), fixedGridId, fixedGridGen,
                                  1, t0, oneHour, fx));
  WF_REQUIRE(shortLived.allocated() && longLived.allocated());
  WF_SAME(shortLived.explanation.selected.slots.first, 0u);
  WF_SAME(longLived.explanation.selected.slots.first, 2u);
  const Instant lapsed = t0 + hundredSeconds;
  WF_SAME(fx.runtime.reservation(shortLived.reservation)->lease.expiresAt.nanos(), lapsed.nanos());

  // At the expiry instant the lease is no longer valid, so the reservation owns
  // nothing even though its state is still Reserved.
  const std::optional<SpectrumUsage> atExpiry = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(atExpiry.has_value());
  WF_SAME(atExpiry->liveSlots, 1u);
  WF_SAME(atExpiry->liveReservations, 1u);
  WF_SAME(atExpiry->lapsedSlots, 2u);
  WF_SAME(atExpiry->lapsedReservations, 1u);
  WF_SAME(atExpiry->freeSlots, 95u);
  WF_REQUIRE(atExpiry->freeRuns.size() == 2);
  WF_SAME(atExpiry->freeRuns[0].first, 0u);
  WF_SAME(atExpiry->freeRuns[0].count, 2u);
  WF_SAME(atExpiry->freeRuns[1].first, 3u);
  WF_SAME(atExpiry->freeRuns[1].count, 93u);
  WF_SAME(codeOf(fx.runtime.activate(shortLived.reservation, ReservationGeneration(1), fx.activation(),
                                     lapsed)),
          "refused");
  WF_SAME(codeOf(fx.runtime.renew(shortLived.reservation, ReservationGeneration(1), Duration::seconds(30),
                                  fx.reservation(), lapsed, 0)),
          "refused");

  // The lapsed spectrum is allocatable at that instant, while the lapsed record
  // is untouched: only reclaimExpired moves it.
  const AllocationDecision reuse = fx.runtime.allocate(request(AllocationRequestId(132), SpectrumDomainId(1),
                                                              fixedGridId, fixedGridGen, 2, lapsed, oneHour, fx));
  WF_REQUIRE(reuse.allocated());
  WF_SAME(reuse.explanation.selected.slots.first, 0u);
  WF_SAME(reuse.explanation.selected.slots.count, 2u);
  const std::optional<SpectrumReservation> lapsedRecord = fx.runtime.reservation(shortLived.reservation);
  WF_REQUIRE(lapsedRecord.has_value());
  WF_SAME(stateOf(lapsedRecord->state), "reserved");
  WF_SAME(lapsedRecord->reclaimedAt.nanos(), std::int64_t{0});
  WF_SAME(lapsedRecord->lease.expiresAt.nanos(), lapsed.nanos());
  expectOwnershipInvariant(fx.runtime, lapsed);
  expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), lapsed);

  const ReclaimReport report = fx.runtime.reclaimExpired(lapsed);
  WF_SAME(codeOf(report.status), "ok");
  WF_SAME(report.evaluatedAt.nanos(), lapsed.nanos());
  WF_SAME(report.scanned, std::size_t{3});
  WF_SAME(report.expired, std::size_t{1});
  WF_REQUIRE(report.reclaimed.size() == 1);
  WF_SAME(report.reclaimed.front().raw(), shortLived.reservation.raw());
  WF_REQUIRE(report.lapsed.size() == 1);
  WF_SAME(report.lapsed.front().raw(), shortLived.reservation.raw());
  const std::optional<SpectrumReservation> reclaimed = fx.runtime.reservation(shortLived.reservation);
  WF_REQUIRE(reclaimed.has_value());
  WF_SAME(stateOf(reclaimed->state), "reclaimed");
  WF_SAME(reclaimed->reclaimedAt.nanos(), lapsed.nanos());
  WF_SAME(reclaimed->updatedAt.nanos(), lapsed.nanos());
  // One operation generation for the lapse and one for the reclamation.
  WF_SAME(reclaimed->lastOperation.raw(), std::uint64_t{3});
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().expirationSweeps, std::uint64_t{1});
  const std::optional<SpectrumUsage> afterReclaim = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(afterReclaim.has_value());
  WF_SAME(afterReclaim->liveSlots, 3u);
  WF_SAME(afterReclaim->reclaimedReservations, 1u);
  WF_SAME(afterReclaim->lapsedReservations, 0u);
  expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), lapsed);

  // Reclaiming again is a no-op, not a second reclamation.
  const ReclaimReport again = fx.runtime.reclaimExpired(lapsed + Duration::seconds(1));
  WF_SAME(again.reclaimed.size(), std::size_t{0});
  WF_SAME(again.expired, std::size_t{0});
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().expirationSweeps, std::uint64_t{2});
}

WF_TEST(release_and_reclamation_return_spectrum_exactly) {
  Fixture fx;
  registerGrids(fx.runtime);
  (void)fx.addDomain(SpectrumDomainId(1), fixedGridId, kFixedSlots);

  const AllocationDecision first = fx.runtime.allocate(request(
      AllocationRequestId(140), SpectrumDomainId(1), fixedGridId, fixedGridGen, 3, t0, oneHour, fx));
  WF_REQUIRE(first.allocated());
  WF_SAME(first.explanation.selected.slots.first, 0u);
  expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), t0);

  const Instant releasedAt = t0 + Duration::seconds(1);
  WF_SAME(codeOf(fx.runtime.release(first.reservation, ReservationGeneration(1), fx.release(), releasedAt)),
          "ok");
  const std::optional<SpectrumReservation> released = fx.runtime.reservation(first.reservation);
  WF_REQUIRE(released.has_value());
  WF_SAME(stateOf(released->state), "released");
  WF_SAME(released->releasedAt.nanos(), releasedAt.nanos());
  WF_SAME(released->updatedAt.nanos(), releasedAt.nanos());
  WF_SAME(released->lastOperation.raw(), std::uint64_t{2});
  WF_SAME(released->slots.first, 0u);
  WF_SAME(released->slots.count, 3u);
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});

  const std::optional<SpectrumUsage> freed = fx.runtime.usage(SpectrumDomainId(1), releasedAt);
  WF_REQUIRE(freed.has_value());
  WF_SAME(freed->liveSlots, 0u);
  WF_SAME(freed->freeSlots, 96u);
  WF_SAME(freed->releasedReservations, 1u);
  WF_REQUIRE(freed->freeRuns.size() == 1);
  WF_SAME(freed->freeRuns.front().first, 0u);
  WF_SAME(freed->freeRuns.front().count, 96u);
  expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), releasedAt);

  // A duplicate release never creates capacity and never touches the record.
  WF_SAME(codeOf(fx.runtime.release(first.reservation, ReservationGeneration(1), fx.release(), releasedAt)),
          "illegal-transition");
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});
  WF_SAME(fx.runtime.reservation(first.reservation)->releasedAt.nanos(), releasedAt.nanos());
  WF_SAME(fx.runtime.reservation(first.reservation)->lastOperation.raw(), std::uint64_t{2});
  const std::optional<SpectrumUsage> afterDoubleRelease = fx.runtime.usage(SpectrumDomainId(1), releasedAt);
  WF_REQUIRE(afterDoubleRelease.has_value());
  WF_SAME(afterDoubleRelease->liveSlots, 0u);
  WF_SAME(afterDoubleRelease->releasedReservations, 1u);

  // First fit reuses the released range exactly.
  const AllocationDecision reused = fx.runtime.allocate(request(
      AllocationRequestId(141), SpectrumDomainId(1), fixedGridId, fixedGridGen, 3, t0, oneHour, fx));
  WF_REQUIRE(reused.allocated());
  WF_SAME(reused.reservation.raw(), std::uint64_t{2});
  WF_SAME(reused.explanation.selected.slots.first, 0u);
  WF_SAME(reused.explanation.selected.slots.count, 3u);
  WF_SAME(reused.explanation.selected.frequency.lowMhz, fixedLow(0));
  WF_SAME(reused.explanation.selected.frequency.highMhz, fixedLow(3));

  // A lapsed reservation returns its spectrum through reclamation; the next
  // first fit lands on exactly the reclaimed range.
  const AllocationDecision shortLived = fx.runtime.allocate(request(
      AllocationRequestId(142), SpectrumDomainId(1), fixedGridId, fixedGridGen, 2, t0, hundredSeconds, fx));
  WF_REQUIRE(shortLived.allocated());
  WF_SAME(shortLived.explanation.selected.slots.first, 3u);
  const Instant lapsed = t0 + hundredSeconds;
  const ReclaimReport report = fx.runtime.reclaimExpired(lapsed);
  WF_REQUIRE(report.reclaimed.size() == 1);
  WF_SAME(report.reclaimed.front().raw(), shortLived.reservation.raw());
  WF_SAME(stateOf(fx.runtime.reservation(shortLived.reservation)->state), "reclaimed");
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});

  const AllocationDecision afterReclaim = fx.runtime.allocate(
      request(AllocationRequestId(143), SpectrumDomainId(1), fixedGridId, fixedGridGen, 2, lapsed, oneHour, fx));
  WF_REQUIRE(afterReclaim.allocated());
  WF_SAME(afterReclaim.reservation.raw(), std::uint64_t{4});
  WF_SAME(afterReclaim.explanation.selected.slots.first, 3u);
  WF_SAME(afterReclaim.explanation.selected.slots.count, 2u);
  WF_SAME(afterReclaim.explanation.selected.frequency.lowMhz, fixedLow(3));
  WF_SAME(afterReclaim.explanation.selected.frequency.highMhz, fixedLow(5));

  const std::optional<SpectrumUsage> finalUsage = fx.runtime.usage(SpectrumDomainId(1), lapsed);
  WF_REQUIRE(finalUsage.has_value());
  WF_SAME(finalUsage->liveSlots, 5u);
  WF_SAME(finalUsage->liveReservations, 2u);
  WF_SAME(finalUsage->releasedReservations, 1u);
  WF_SAME(finalUsage->reclaimedReservations, 1u);
  WF_SAME(finalUsage->freeSlots, 91u);
  WF_REQUIRE(finalUsage->freeRuns.size() == 1);
  WF_SAME(finalUsage->freeRuns.front().first, 5u);
  WF_SAME(finalUsage->freeRuns.front().count, 91u);
  WF_SAME(fx.runtime.reservations().size(), std::size_t{4});
  WF_SAME(fx.runtime.stats().allocationsCommitted, std::uint64_t{4});
  WF_SAME(fx.runtime.stats().releases, std::uint64_t{1});
  WF_SAME(fx.runtime.stats().reclamations, std::uint64_t{1});
  expectOwnershipInvariant(fx.runtime, lapsed);
  expectFreeRunAccounting(fx.runtime, SpectrumDomainId(1), lapsed);
}

// ---------------------------------------------------------------------------
// Seeded deterministic pressure
// ---------------------------------------------------------------------------

struct SequenceResult {
  std::uint64_t committed{0};
  std::uint64_t refused{0};
  std::uint64_t released{0};
  std::uint64_t reclaimed{0};
  std::uint64_t sweeps{0};
  std::vector<std::string> trace;
};

SequenceResult runSeededSequence(SpectrumRuntime& runtime) {
  const SpectrumDomainId domains[3] = {SpectrumDomainId(1), SpectrumDomainId(2), SpectrumDomainId(3)};
  const std::uint32_t guards[3] = {0, 25'000, 50'000};
  const Duration leases[4] = {Duration::seconds(100), Duration::seconds(300), Duration::seconds(900),
                              oneHour};
  std::mt19937_64 engine(0x5EED1234ull);
  SequenceResult result;

  for (std::uint32_t step = 0; step < 400; ++step) {
    const Instant now = t0 + Duration::seconds(step);
    const SpectrumDomainId domain = domains[engine() % 3];
    const std::uint64_t action = engine() % 100;
    std::vector<ReservationId> releasable;
    for (const SpectrumReservation& reservation : runtime.reservations()) {
      if (isLiveState(reservation.state)) releasable.push_back(reservation.id);
    }

    if (action < 55) {
      const std::uint32_t slots = 1 + static_cast<std::uint32_t>(engine() % 4);
      const Duration lease = leases[engine() % 4];
      const std::int64_t guard = static_cast<std::int64_t>(guards[engine() % 3]);
      const std::uint32_t renewals = static_cast<std::uint32_t>(engine() % 3);
      SpectrumRequest next = makeRequest(AllocationRequestId(500 + step), OwnerId(7), {domain},
                                         {SpectrumDomainGeneration(1)}, flexGridId, flexGridGen, slots, now,
                                         lease, eligibilityToken(runtime), reservationToken(runtime),
                                         ContiguityRequirement::Required, ContinuityRequirement::Unspecified);
      next.guardBandMhz = guard;
      next.maxRenewals = renewals;
      const std::size_t before = runtime.reservations().size();
      const std::optional<SpectrumUsage> usage = runtime.usage(domain, now);
      WF_CHECK(usage.has_value());
      if (!usage.has_value()) continue;
      const std::vector<SlotRange> freeRuns = usage->freeRuns;
      const AllocationDecision decision = runtime.allocate(next);
      if (decision.allocated()) {
        ++result.committed;
        const std::optional<SpectrumReservation> stored = runtime.reservation(decision.reservation);
        WF_CHECK(stored.has_value());
        if (!stored.has_value()) continue;
        bool coveredByAFreeRun = false;
        for (const SlotRange& run : freeRuns) {
          if (run.contains(stored->slots)) coveredByAFreeRun = true;
        }
        WF_CHECK(coveredByAFreeRun);
        WF_SAME(stored->slots.count, slots);
        WF_SAME(stored->guardBandMhz, guard);
        WF_SAME(stored->lease.maxRenewals, renewals);
        WF_SAME(stored->lease.grantedAt.nanos(), now.nanos());
        WF_SAME(stored->lease.expiresAt.nanos(), (now + lease).nanos());
        WF_SAME(stored->domains.size(), std::size_t{1});
        WF_SAME(stored->domains.front().raw(), domain.raw());
        WF_SAME(runtime.reservations().size(), before + 1);
        std::ostringstream line;
        line << "commit " << stored->id.raw() << " d" << domain.raw() << " [" << stored->slots.first << "+"
             << stored->slots.count << ") guard=" << stored->guardBandMhz
             << " exp=" << stored->lease.expiresAt.nanos();
        result.trace.push_back(line.str());
      } else {
        ++result.refused;
        WF_CHECK(!decision.status.ok());
        WF_CHECK(isRefusal(decision.outcome));
        WF_CHECK(!decision.allocated());
        WF_SAME(runtime.reservations().size(), before);
        WF_SAME(decision.reservation.raw(), std::uint64_t{0});
      }
    } else if (action < 80) {
      if (!releasable.empty()) {
        const ReservationId id = releasable[engine() % releasable.size()];
        const Status status = runtime.release(id, ReservationGeneration(1), releaseToken(runtime), now);
        WF_SAME(codeOf(status), "ok");
        ++result.released;
        std::ostringstream line;
        line << "release " << id.raw();
        result.trace.push_back(line.str());
      }
    } else {
      const ReclaimReport report = runtime.reclaimExpired(now);
      ++result.sweeps;
      result.reclaimed += report.reclaimed.size();
      if (!report.reclaimed.empty()) {
        std::ostringstream line;
        line << "reclaim";
        for (const ReservationId id : report.reclaimed) line << " " << id.raw();
        result.trace.push_back(line.str());
      }
    }

    expectOwnershipInvariant(runtime, now);
    for (const SpectrumDomainId candidate : domains) {
      expectFreeRunAccounting(runtime, candidate, now);
    }
  }

  for (const SpectrumReservation& reservation : runtime.reservations()) {
    std::ostringstream line;
    line << "final " << reservation.id.raw() << " " << toToken(reservation.state) << " ["
         << reservation.slots.first << "+" << reservation.slots.count << ") exp="
         << reservation.lease.expiresAt.nanos();
    result.trace.push_back(line.str());
  }
  return result;
}

WF_TEST(a_seeded_allocate_release_sequence_never_overlaps) {
  RuntimeConfig config;
  config.maxCandidatesPerRequest = 64;
  Fixture fx(config);
  registerGrids(fx.runtime);
  for (std::uint64_t index = 1; index <= 3; ++index) {
    (void)fx.addDomain(SpectrumDomainId(index), flexGridId, kFixedSlots, SpectrumDomainGeneration(1),
                       flexGridGen, kSmallWindow);
  }

  const SequenceResult first = runSeededSequence(fx.runtime);
  WF_CHECK(first.committed > 0);
  WF_CHECK(first.refused > 0);
  WF_CHECK(first.released > 0);
  WF_CHECK(first.reclaimed > 0);
  WF_SAME(fx.runtime.stats().allocationsCommitted, first.committed);
  WF_SAME(fx.runtime.stats().allocationsRefused, first.refused);
  WF_SAME(fx.runtime.stats().releases, first.released);
  WF_SAME(fx.runtime.stats().reclamations, first.reclaimed);
  WF_SAME(fx.runtime.stats().expirationSweeps, first.sweeps);
  WF_SAME(fx.runtime.reservations().size(), static_cast<std::size_t>(first.committed));
  expectOwnershipInvariant(fx.runtime, t0 + Duration::seconds(399));
  for (std::uint64_t index = 1; index <= 3; ++index) {
    expectFreeRunAccounting(fx.runtime, SpectrumDomainId(index), t0 + Duration::seconds(399));
  }

  // The identical seed against an identical runtime produces an identical trace.
  Fixture other(config);
  registerGrids(other.runtime);
  for (std::uint64_t index = 1; index <= 3; ++index) {
    (void)other.addDomain(SpectrumDomainId(index), flexGridId, kFixedSlots, SpectrumDomainGeneration(1),
                          flexGridGen, kSmallWindow);
  }
  const SequenceResult second = runSeededSequence(other.runtime);
  WF_SAME(second.sweeps, first.sweeps);
  WF_SAME(second.committed, first.committed);
  WF_SAME(second.refused, first.refused);
  WF_SAME(second.released, first.released);
  WF_SAME(second.reclaimed, first.reclaimed);
  WF_SAME(other.runtime.reservations().size(), static_cast<std::size_t>(first.committed));
  WF_REQUIRE(second.trace.size() == first.trace.size());
  for (std::size_t index = 0; index < first.trace.size(); ++index) {
    WF_SAME(second.trace[index], first.trace[index]);
  }
  WF_SAME(other.runtime.stats().allocationsCommitted, first.committed);
  WF_SAME(other.runtime.stats().allocationsRefused, first.refused);
}

// ---------------------------------------------------------------------------
// Concurrent mutation of disjoint domains
// ---------------------------------------------------------------------------

WF_TEST(concurrent_allocations_on_disjoint_domains_keep_accounting) {
  Fixture fx;
  registerGrids(fx.runtime);
  for (std::uint64_t index = 1; index <= 4; ++index) {
    (void)fx.addDomain(SpectrumDomainId(index), fixedGridId, kFixedSlots);
  }

  constexpr std::uint32_t kCycles = 40;
  struct ThreadResult {
    bool allocated{true};
    bool released{true};
    bool exact{true};
    std::uint32_t committed{0};
    std::uint32_t releasedCount{0};
  };
  std::vector<ThreadResult> results(4);
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (std::uint32_t worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&fx, &results, worker]() {
      ThreadResult& result = results[worker];
      const SpectrumDomainId domain(worker + 1);
      for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        const std::uint32_t slots = 1 + (cycle % 3);
        const Instant now = t0 + Duration::seconds(cycle);
        const AllocationDecision decision = fx.runtime.allocate(request(
            AllocationRequestId(1000 + worker * 1000 + cycle), domain, fixedGridId, fixedGridGen, slots, now,
            oneHour, fx));
        if (!decision.allocated()) {
          result.allocated = false;
          continue;
        }
        ++result.committed;
        const std::optional<SpectrumReservation> stored = fx.runtime.reservation(decision.reservation);
        if (!stored.has_value()) {
          result.exact = false;
          continue;
        }
        if (stored->slots.count != slots) result.exact = false;
        if (stored->domains.size() != 1) result.exact = false;
        if (stored->domains.front() != domain) result.exact = false;
        if (stored->anchorDomain != domain) result.exact = false;
        if (!stored->lease.validAt(now)) result.exact = false;
        const Status released =
            fx.runtime.release(decision.reservation, stored->generation, fx.release(), now);
        if (!released.ok()) {
          result.released = false;
          continue;
        }
        ++result.releasedCount;
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  std::uint64_t committed = 0;
  std::uint64_t released = 0;
  for (const ThreadResult& result : results) {
    WF_CHECK(result.allocated);
    WF_CHECK(result.released);
    WF_CHECK(result.exact);
    WF_SAME(result.committed, kCycles);
    WF_SAME(result.releasedCount, kCycles);
    committed += result.committed;
    released += result.releasedCount;
  }
  WF_SAME(committed, static_cast<std::uint64_t>(4 * kCycles));
  WF_SAME(released, static_cast<std::uint64_t>(4 * kCycles));
  const RuntimeStats stats = fx.runtime.stats();
  WF_SAME(stats.allocationsCommitted, committed);
  WF_SAME(stats.allocationsRefused, std::uint64_t{0});
  WF_SAME(stats.releases, released);
  WF_SAME(fx.runtime.reservations().size(), static_cast<std::size_t>(4 * kCycles));

  const Instant end = t0 + Duration::seconds(kCycles);
  for (std::uint64_t index = 1; index <= 4; ++index) {
    const std::optional<SpectrumUsage> usage = fx.runtime.usage(SpectrumDomainId(index), end);
    WF_REQUIRE(usage.has_value());
    WF_SAME(usage->liveSlots, 0u);
    WF_SAME(usage->liveReservations, 0u);
    WF_SAME(usage->freeSlots, 96u);
    WF_SAME(usage->releasedReservations, kCycles);
    WF_REQUIRE(usage->freeRuns.size() == 1);
    WF_SAME(usage->freeRuns.front().first, 0u);
    WF_SAME(usage->freeRuns.front().count, 96u);
  }
  expectOwnershipInvariant(fx.runtime, end);
}

WF_TEST_MAIN()
